/**
 * Text-to-Speech Module
 *
 * Streams PCM audio from ElevenLabs or OpenAI API directly to I2S output.
 * Uses a ring buffer to smooth network jitter.
 */

#include "tts.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "bsp_board_extra.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "tts";

// ElevenLabs API configuration
#define ELEVENLABS_API_BASE "https://api.elevenlabs.io/v1/text-to-speech/"
#define ELEVENLABS_MODEL_ID "eleven_multilingual_v2"
#define ELEVENLABS_OUTPUT_FORMAT "pcm_16000"
#define ELEVENLABS_LATENCY_OPT "3"
#define ELEVENLABS_SAMPLE_RATE 16000

// OpenAI API configuration
#define OPENAI_API_URL "https://api.openai.com/v1/audio/speech"
#define OPENAI_SAMPLE_RATE 24000

// Ring buffer configuration
#define RING_BUFFER_SIZE (1024 * 1024)  // 1MB
#define PLAYBACK_CHUNK_SIZE 2048        // Mono bytes to read at once
#define MIN_BUFFER_BEFORE_PLAY (32 * 1024)  // Start playing after 32KB buffered

// Test message
#define TTS_TEST_MESSAGE "Hello! The WiFi connection is now active and text to speech is working."

// Ring buffer structure
typedef struct {
    uint8_t *buffer;
    size_t size;
    size_t head;  // Write position
    size_t tail;  // Read position
    SemaphoreHandle_t mutex;
    volatile bool overflow;
} ring_buffer_t;

// Module state
static bool s_initialized = false;
static volatile bool s_streaming = false;
static volatile bool s_playing = false;
static volatile bool s_stop_requested = false;
static ring_buffer_t s_ring_buffer = {0};
static TaskHandle_t s_playback_task = NULL;
static SemaphoreHandle_t s_playback_done_sem = NULL;
static tts_provider_t s_current_provider = TTS_PROVIDER_ELEVENLABS;
static uint32_t s_current_sample_rate = ELEVENLABS_SAMPLE_RATE;

// Forward declarations
static esp_err_t ring_buffer_init(ring_buffer_t *rb, size_t size);
static void ring_buffer_free(ring_buffer_t *rb);
static size_t ring_buffer_write(ring_buffer_t *rb, const uint8_t *data, size_t len);
static size_t ring_buffer_read(ring_buffer_t *rb, uint8_t *data, size_t len);
static size_t ring_buffer_available(ring_buffer_t *rb);
static void ring_buffer_reset(ring_buffer_t *rb);
static void playback_task(void *arg);
static esp_err_t http_event_handler(esp_http_client_event_t *evt);

/**
 * Initialize ring buffer in PSRAM
 */
static esp_err_t ring_buffer_init(ring_buffer_t *rb, size_t size)
{
    rb->buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!rb->buffer) {
        ESP_LOGE(TAG, "Failed to allocate ring buffer (%d bytes)", size);
        return ESP_ERR_NO_MEM;
    }

    rb->size = size;
    rb->head = 0;
    rb->tail = 0;
    rb->overflow = false;

    rb->mutex = xSemaphoreCreateMutex();
    if (!rb->mutex) {
        heap_caps_free(rb->buffer);
        rb->buffer = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/**
 * Free ring buffer resources
 */
static void ring_buffer_free(ring_buffer_t *rb)
{
    if (rb->mutex) {
        vSemaphoreDelete(rb->mutex);
        rb->mutex = NULL;
    }
    if (rb->buffer) {
        heap_caps_free(rb->buffer);
        rb->buffer = NULL;
    }
    rb->size = 0;
    rb->head = 0;
    rb->tail = 0;
}

/**
 * Reset ring buffer (clear all data)
 */
static void ring_buffer_reset(ring_buffer_t *rb)
{
    xSemaphoreTake(rb->mutex, portMAX_DELAY);
    rb->head = 0;
    rb->tail = 0;
    rb->overflow = false;
    xSemaphoreGive(rb->mutex);
}

/**
 * Get free space in ring buffer (call with mutex held)
 */
static size_t ring_buffer_free_space_internal(ring_buffer_t *rb)
{
    if (rb->head >= rb->tail) {
        return rb->size - rb->head + rb->tail - 1;
    } else {
        return rb->tail - rb->head - 1;
    }
}

/**
 * Write data to ring buffer using bulk memcpy
 * Returns number of bytes written
 */
static size_t ring_buffer_write(ring_buffer_t *rb, const uint8_t *data, size_t len)
{
    if (!rb->buffer || !data || len == 0) {
        return 0;
    }

    xSemaphoreTake(rb->mutex, portMAX_DELAY);

    size_t free_space = ring_buffer_free_space_internal(rb);
    if (free_space == 0) {
        rb->overflow = true;
        xSemaphoreGive(rb->mutex);
        return 0;
    }

    size_t to_write = (len < free_space) ? len : free_space;
    size_t written = 0;

    // Write in up to two chunks (handle wrap-around)
    while (written < to_write) {
        size_t chunk_size;
        if (rb->head >= rb->tail) {
            // Write from head to end of buffer
            chunk_size = rb->size - rb->head;
            if (rb->tail == 0) {
                chunk_size--;  // Leave one byte gap
            }
        } else {
            // Write from head to tail-1
            chunk_size = rb->tail - rb->head - 1;
        }

        if (chunk_size == 0) break;
        if (chunk_size > to_write - written) {
            chunk_size = to_write - written;
        }

        memcpy(&rb->buffer[rb->head], &data[written], chunk_size);
        rb->head = (rb->head + chunk_size) % rb->size;
        written += chunk_size;
    }

    if (written < len) {
        rb->overflow = true;
    }

    xSemaphoreGive(rb->mutex);
    return written;
}

/**
 * Read data from ring buffer using bulk memcpy
 * Returns number of bytes read
 */
static size_t ring_buffer_read(ring_buffer_t *rb, uint8_t *data, size_t len)
{
    if (!rb->buffer || !data || len == 0) {
        return 0;
    }

    xSemaphoreTake(rb->mutex, portMAX_DELAY);

    size_t available;
    if (rb->head >= rb->tail) {
        available = rb->head - rb->tail;
    } else {
        available = rb->size - rb->tail + rb->head;
    }

    if (available == 0) {
        xSemaphoreGive(rb->mutex);
        return 0;
    }

    size_t to_read = (len < available) ? len : available;
    size_t read_count = 0;

    // Read in up to two chunks (handle wrap-around)
    while (read_count < to_read) {
        size_t chunk_size;
        if (rb->head >= rb->tail) {
            // Read from tail to head
            chunk_size = rb->head - rb->tail;
        } else {
            // Read from tail to end of buffer
            chunk_size = rb->size - rb->tail;
        }

        if (chunk_size == 0) break;
        if (chunk_size > to_read - read_count) {
            chunk_size = to_read - read_count;
        }

        memcpy(&data[read_count], &rb->buffer[rb->tail], chunk_size);
        rb->tail = (rb->tail + chunk_size) % rb->size;
        read_count += chunk_size;
    }

    xSemaphoreGive(rb->mutex);
    return read_count;
}

/**
 * Get number of bytes available to read
 */
static size_t ring_buffer_available(ring_buffer_t *rb)
{
    xSemaphoreTake(rb->mutex, portMAX_DELAY);
    size_t available;
    if (rb->head >= rb->tail) {
        available = rb->head - rb->tail;
    } else {
        available = rb->size - rb->tail + rb->head;
    }
    xSemaphoreGive(rb->mutex);
    return available;
}

/**
 * Playback task - reads mono PCM from ring buffer, converts to stereo, writes to I2S
 */
static void playback_task(void *arg)
{
    // Allocate buffers: mono input and stereo output (2x size for stereo)
    uint8_t *mono_chunk = heap_caps_malloc(PLAYBACK_CHUNK_SIZE, MALLOC_CAP_INTERNAL);
    uint8_t *stereo_chunk = heap_caps_malloc(PLAYBACK_CHUNK_SIZE * 2, MALLOC_CAP_INTERNAL);

    if (!mono_chunk || !stereo_chunk) {
        ESP_LOGE(TAG, "Failed to allocate playback chunks");
        if (mono_chunk) heap_caps_free(mono_chunk);
        if (stereo_chunk) heap_caps_free(stereo_chunk);
        s_playing = false;
        xSemaphoreGive(s_playback_done_sem);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Playback task started");

    // Configure codec for current sample rate (16kHz for ElevenLabs, 24kHz for OpenAI)
    esp_err_t err = bsp_extra_codec_set_fs(s_current_sample_rate, 16, I2S_SLOT_MODE_STEREO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure codec: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Codec configured for %luHz stereo", (unsigned long)s_current_sample_rate);
    }

    // Unmute codec (audio player may have left codec muted)
    // Don't change volume - respect the user's volume setting from web interface
    bsp_extra_codec_mute_set(false);
    int current_volume = bsp_extra_codec_volume_get();
    ESP_LOGI(TAG, "Codec unmuted, volume is %d", current_volume);

    // Wait for minimum buffer before starting playback
    while (s_streaming && !s_stop_requested) {
        size_t available = ring_buffer_available(&s_ring_buffer);
        if (available >= MIN_BUFFER_BEFORE_PLAY) {
            ESP_LOGI(TAG, "Buffer ready (%d bytes), starting playback", available);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    s_playing = true;

    // Playback loop
    int loop_count = 0;
    while (!s_stop_requested) {
        size_t available = ring_buffer_available(&s_ring_buffer);

        if (available > 0) {
            // Read mono samples (must be even number of bytes for 16-bit samples)
            size_t to_read = (available < PLAYBACK_CHUNK_SIZE) ? available : PLAYBACK_CHUNK_SIZE;
            to_read = to_read & ~1;  // Ensure even number of bytes

            size_t read_len = ring_buffer_read(&s_ring_buffer, mono_chunk, to_read);

            if (read_len > 0) {
                // Convert mono to stereo by duplicating each 16-bit sample
                // Also apply digital gain (2x boost) to increase loudness
                int16_t *mono_samples = (int16_t *)mono_chunk;
                int16_t *stereo_samples = (int16_t *)stereo_chunk;
                size_t num_samples = read_len / 2;  // Number of 16-bit samples

                for (size_t i = 0; i < num_samples; i++) {
                    // Apply 2x digital gain with clipping protection
                    int32_t amplified = (int32_t)mono_samples[i] * 2;
                    if (amplified > 32767) amplified = 32767;
                    if (amplified < -32768) amplified = -32768;
                    int16_t sample = (int16_t)amplified;

                    stereo_samples[i * 2] = sample;      // Left channel
                    stereo_samples[i * 2 + 1] = sample;  // Right channel
                }

                // Write stereo data to I2S (2x the mono size)
                size_t stereo_len = read_len * 2;
                size_t written = 0;
                esp_err_t err = bsp_extra_i2s_write(stereo_chunk, stereo_len, &written, 1000);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "I2S write error: %s", esp_err_to_name(err));
                }
            }

            // Yield periodically to prevent watchdog and allow HTTP task to run
            if (++loop_count >= 10) {
                loop_count = 0;
                vTaskDelay(1);  // Use delay instead of yield to let lower-priority tasks run
            }
        } else if (!s_streaming) {
            // No more data and streaming is done
            ESP_LOGI(TAG, "Playback complete");
            break;
        } else {
            // Buffer underrun - wait for more data
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

    heap_caps_free(mono_chunk);
    heap_caps_free(stereo_chunk);
    s_playing = false;

    ESP_LOGI(TAG, "Playback task finished");
    xSemaphoreGive(s_playback_done_sem);
    vTaskDelete(NULL);
}

/**
 * Get free space in ring buffer
 */
static size_t ring_buffer_free_space(ring_buffer_t *rb)
{
    xSemaphoreTake(rb->mutex, portMAX_DELAY);
    size_t free_space = ring_buffer_free_space_internal(rb);
    xSemaphoreGive(rb->mutex);
    return free_space;
}

/**
 * HTTP event handler - writes PCM data to ring buffer with flow control
 */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    static int total_bytes = 0;

    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGE(TAG, "HTTP error");
            break;

        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI(TAG, "Connected to %s API", tts_get_provider_name(s_current_provider));
            total_bytes = 0;
            break;

        case HTTP_EVENT_ON_DATA:
            if (s_stop_requested) {
                return ESP_FAIL;  // Abort request
            }

            // Flow control: wait if buffer is too full
            int wait_count = 0;
            while (ring_buffer_free_space(&s_ring_buffer) < evt->data_len && !s_stop_requested) {
                vTaskDelay(pdMS_TO_TICKS(10));
                wait_count++;
                if (wait_count > 500) {  // 5 second timeout
                    ESP_LOGE(TAG, "Flow control timeout - playback stalled?");
                    return ESP_FAIL;
                }
            }

            if (s_stop_requested) {
                return ESP_FAIL;
            }

            // Write PCM data to ring buffer
            size_t written = ring_buffer_write(&s_ring_buffer, evt->data, evt->data_len);
            total_bytes += written;

            // Log progress periodically
            if (total_bytes % 131072 == 0) {
                ESP_LOGI(TAG, "Streaming: %d KB received", total_bytes / 1024);
            }
            break;

        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(TAG, "Download complete: %d bytes total", total_bytes);
            break;

        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGD(TAG, "Disconnected");
            break;

        default:
            break;
    }

    return ESP_OK;
}

/**
 * Build ElevenLabs API URL
 */
static void build_elevenlabs_url(char *url, size_t url_size)
{
    snprintf(url, url_size,
             "%s%s/stream?output_format=%s&optimize_streaming_latency=%s",
             ELEVENLABS_API_BASE,
             CONFIG_ELEVENLABS_VOICE_ID,
             ELEVENLABS_OUTPUT_FORMAT,
             ELEVENLABS_LATENCY_OPT);
}

/**
 * Build ElevenLabs JSON request body
 */
static char *build_elevenlabs_body(const char *text, float speed)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON_AddStringToObject(root, "text", text);
    cJSON_AddStringToObject(root, "model_id", ELEVENLABS_MODEL_ID);

    // Add voice settings with speed if not default
    if (speed != 1.0f) {
        cJSON *voice_settings = cJSON_CreateObject();
        if (voice_settings) {
            cJSON_AddNumberToObject(voice_settings, "speed", speed);
            cJSON_AddItemToObject(root, "voice_settings", voice_settings);
        }
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    return json_str;
}

/**
 * Build OpenAI JSON request body
 */
static char *build_openai_body(const char *text, float speed)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON_AddStringToObject(root, "model", CONFIG_OPENAI_TTS_MODEL);
    cJSON_AddStringToObject(root, "input", text);
    cJSON_AddStringToObject(root, "voice", CONFIG_OPENAI_TTS_VOICE);
    cJSON_AddStringToObject(root, "response_format", "pcm");

    // Add speed if not default (OpenAI range: 0.25-4.0)
    if (speed != 1.0f) {
        cJSON_AddNumberToObject(root, "speed", speed);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    return json_str;
}

/**
 * Initialize the TTS module
 */
esp_err_t tts_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "TTS already initialized");
        return ESP_OK;
    }

    // Check if at least one provider is available
    bool elevenlabs_available = tts_is_provider_available(TTS_PROVIDER_ELEVENLABS);
    bool openai_available = tts_is_provider_available(TTS_PROVIDER_OPENAI);

    if (!elevenlabs_available && !openai_available) {
        ESP_LOGE(TAG, "No TTS provider configured. Run 'idf.py menuconfig'");
        return ESP_ERR_INVALID_STATE;
    }

    // Set default provider to first available
    if (elevenlabs_available) {
        s_current_provider = TTS_PROVIDER_ELEVENLABS;
        s_current_sample_rate = ELEVENLABS_SAMPLE_RATE;
    } else {
        s_current_provider = TTS_PROVIDER_OPENAI;
        s_current_sample_rate = OPENAI_SAMPLE_RATE;
    }

    // Initialize ring buffer
    esp_err_t err = ring_buffer_init(&s_ring_buffer, RING_BUFFER_SIZE);
    if (err != ESP_OK) {
        return err;
    }

    // Create semaphore for playback completion
    s_playback_done_sem = xSemaphoreCreateBinary();
    if (!s_playback_done_sem) {
        ring_buffer_free(&s_ring_buffer);
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "TTS initialized (provider: %s, ring buffer: %d bytes)",
             tts_get_provider_name(s_current_provider), RING_BUFFER_SIZE);

    return ESP_OK;
}

/**
 * Set the TTS provider
 */
esp_err_t tts_set_provider(tts_provider_t provider)
{
    if (!tts_is_provider_available(provider)) {
        ESP_LOGE(TAG, "Provider %s not available (API key not configured)",
                 tts_get_provider_name(provider));
        return ESP_ERR_INVALID_ARG;
    }

    s_current_provider = provider;
    s_current_sample_rate = (provider == TTS_PROVIDER_OPENAI) ?
                            OPENAI_SAMPLE_RATE : ELEVENLABS_SAMPLE_RATE;

    ESP_LOGI(TAG, "TTS provider set to %s (%luHz)",
             tts_get_provider_name(provider), (unsigned long)s_current_sample_rate);

    return ESP_OK;
}

/**
 * Get the current TTS provider
 */
tts_provider_t tts_get_provider(void)
{
    return s_current_provider;
}

/**
 * Get the name of a TTS provider
 */
const char *tts_get_provider_name(tts_provider_t provider)
{
    switch (provider) {
        case TTS_PROVIDER_ELEVENLABS:
            return "ElevenLabs";
        case TTS_PROVIDER_OPENAI:
            return "OpenAI";
        default:
            return "Unknown";
    }
}

/**
 * Check if a provider is available (API key configured)
 */
bool tts_is_provider_available(tts_provider_t provider)
{
    switch (provider) {
        case TTS_PROVIDER_ELEVENLABS:
#ifdef CONFIG_ELEVENLABS_API_KEY
            return strlen(CONFIG_ELEVENLABS_API_KEY) > 0;
#else
            return false;
#endif
        case TTS_PROVIDER_OPENAI:
#ifdef CONFIG_OPENAI_API_KEY
            return strlen(CONFIG_OPENAI_API_KEY) > 0;
#else
            return false;
#endif
        default:
            return false;
    }
}

/**
 * Speak the given text using current TTS provider (default speed)
 */
esp_err_t tts_speak(const char *text)
{
    return tts_speak_with_speed(text, 1.0f);
}

/**
 * Speak the given text using current TTS provider with custom speed
 */
esp_err_t tts_speak_with_speed(const char *text, float speed)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "TTS not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!text || strlen(text) == 0) {
        ESP_LOGE(TAG, "Empty text");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_streaming || s_playing) {
        ESP_LOGW(TAG, "TTS already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    // Clamp speed to valid range based on provider
    if (s_current_provider == TTS_PROVIDER_OPENAI) {
        if (speed < 0.25f) speed = 0.25f;
        if (speed > 4.0f) speed = 4.0f;
    } else {
        if (speed < 0.5f) speed = 0.5f;
        if (speed > 2.0f) speed = 2.0f;
    }

    esp_err_t ret = ESP_OK;
    char *json_body = NULL;
    char url[256];

    // Reset state
    ring_buffer_reset(&s_ring_buffer);
    s_stop_requested = false;
    s_streaming = true;

    // Build request based on provider
    if (s_current_provider == TTS_PROVIDER_OPENAI) {
        strncpy(url, OPENAI_API_URL, sizeof(url));
        json_body = build_openai_body(text, speed);
    } else {
        build_elevenlabs_url(url, sizeof(url));
        json_body = build_elevenlabs_body(text, speed);
    }

    if (!json_body) {
        ESP_LOGE(TAG, "Failed to build request body");
        s_streaming = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Starting TTS (%s) for: %.50s%s (speed: %.2fx)",
             tts_get_provider_name(s_current_provider),
             text, strlen(text) > 50 ? "..." : "", speed);

    // Create playback task
    BaseType_t task_created = xTaskCreate(
        playback_task,
        "tts_playback",
        4096,
        NULL,
        5,
        &s_playback_task
    );

    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create playback task");
        free(json_body);
        s_streaming = false;
        return ESP_ERR_NO_MEM;
    }

    // Configure HTTP client
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        s_streaming = false;
        s_stop_requested = true;
        free(json_body);
        return ESP_FAIL;
    }

    // Set headers based on provider
    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (s_current_provider == TTS_PROVIDER_OPENAI) {
#ifdef CONFIG_OPENAI_API_KEY
        char auth_header[256];
        snprintf(auth_header, sizeof(auth_header), "Bearer %s", CONFIG_OPENAI_API_KEY);
        esp_http_client_set_header(client, "Authorization", auth_header);
#endif
    } else {
#ifdef CONFIG_ELEVENLABS_API_KEY
        esp_http_client_set_header(client, "xi-api-key", CONFIG_ELEVENLABS_API_KEY);
        esp_http_client_set_header(client, "Accept", "audio/pcm");
#endif
    }

    // Set POST body
    esp_http_client_set_post_field(client, json_body, strlen(json_body));

    // Perform HTTP request (this streams data to ring buffer via event handler)
    ret = esp_http_client_perform(client);

    // Streaming is done
    s_streaming = false;

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(ret));
        s_stop_requested = true;
    } else {
        int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            ESP_LOGE(TAG, "API error (HTTP %d)", status);
            ret = ESP_FAIL;
            s_stop_requested = true;
        }
    }

    // Clean up HTTP client
    esp_http_client_cleanup(client);
    free(json_body);

    // Wait for playback to complete (indefinitely - playback task will always finish)
    xSemaphoreTake(s_playback_done_sem, portMAX_DELAY);

    s_playback_task = NULL;

    return ret;
}

/**
 * Speak test message
 */
esp_err_t tts_speak_test(void)
{
    ESP_LOGI(TAG, "Speaking test message");
    return tts_speak(TTS_TEST_MESSAGE);
}

/**
 * Stop current TTS playback
 */
esp_err_t tts_stop(void)
{
    if (!s_streaming && !s_playing) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping TTS");
    s_stop_requested = true;

    // Wait for playback task to finish
    if (s_playback_task) {
        xSemaphoreTake(s_playback_done_sem, pdMS_TO_TICKS(5000));
    }

    return ESP_OK;
}

/**
 * Check if TTS is currently playing
 */
bool tts_is_playing(void)
{
    return s_streaming || s_playing;
}

/**
 * Clean up TTS resources
 */
void tts_cleanup(void)
{
    tts_stop();

    if (s_playback_done_sem) {
        vSemaphoreDelete(s_playback_done_sem);
        s_playback_done_sem = NULL;
    }

    ring_buffer_free(&s_ring_buffer);

    s_initialized = false;
    ESP_LOGI(TAG, "TTS cleaned up");
}
