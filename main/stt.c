/**
 * Speech-to-Text Module
 *
 * Records audio from microphone and transcribes via OpenAI Whisper API.
 */

#include "stt.h"
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

static const char *TAG = "stt";

// Whisper API configuration
#define WHISPER_API_URL "https://api.openai.com/v1/audio/transcriptions"
#define WHISPER_MODEL "whisper-1"

// Audio configuration
#define STT_SAMPLE_RATE 16000
#define STT_BITS_PER_SAMPLE 16
#define STT_CHANNELS 1  // Mono output (converted from stereo input)

// Recording limits
#define MAX_RECORDING_SECONDS 300  // 5 minutes
#define MAX_AUDIO_BUFFER_SIZE (STT_SAMPLE_RATE * 2 * MAX_RECORDING_SECONDS)  // ~9.4MB
#define RECORDING_CHUNK_SIZE 1024  // Bytes per I2S read

// WAV header size
#define WAV_HEADER_SIZE 44

// Multipart boundary
#define MULTIPART_BOUNDARY "----ESP32P4AudioBoundary"

/**
 * WAV file header (44 bytes)
 */
typedef struct __attribute__((packed)) {
    char riff_tag[4];        // "RIFF"
    uint32_t file_size;      // File size - 8
    char wave_tag[4];        // "WAVE"
    char fmt_tag[4];         // "fmt "
    uint32_t fmt_size;       // 16 for PCM
    uint16_t audio_format;   // 1 for PCM
    uint16_t num_channels;   // 1 for mono
    uint32_t sample_rate;    // 16000
    uint32_t byte_rate;      // sample_rate * num_channels * bits/8
    uint16_t block_align;    // num_channels * bits/8
    uint16_t bits_per_sample;// 16
    char data_tag[4];        // "data"
    uint32_t data_size;      // Raw audio size
} wav_header_t;

/**
 * Module state
 */
typedef struct {
    stt_state_t state;
    uint8_t *audio_buffer;        // PSRAM buffer for recording
    size_t audio_size;            // Current size of recorded data
    size_t audio_capacity;        // Buffer capacity
    uint32_t recording_start_ms;  // Timestamp when recording started
    uint32_t recording_duration_ms;
    char *transcription;          // Result text (heap allocated)
    char *error_message;          // Error message (heap allocated)
    SemaphoreHandle_t mutex;      // Protect state access
    TaskHandle_t recording_task;  // Recording task handle
    TaskHandle_t transcribe_task; // Transcription task handle
    volatile bool stop_requested; // Signal to stop recording
} stt_context_t;

static stt_context_t s_ctx = {0};
static bool s_initialized = false;

// HTTP response buffer for capturing Whisper API response
typedef struct {
    char *buffer;
    size_t size;
    size_t capacity;
} http_response_t;

// Forward declarations
static void recording_task(void *arg);
static void transcribe_task(void *arg);
static void build_wav_header(wav_header_t *header, size_t pcm_data_size);
static size_t build_multipart_body(uint8_t **body_out, const uint8_t *audio_data, size_t audio_size);
static esp_err_t whisper_http_event_handler(esp_http_client_event_t *evt);

/**
 * Build WAV header for PCM audio
 */
static void build_wav_header(wav_header_t *header, size_t pcm_data_size)
{
    memcpy(header->riff_tag, "RIFF", 4);
    header->file_size = pcm_data_size + WAV_HEADER_SIZE - 8;
    memcpy(header->wave_tag, "WAVE", 4);

    memcpy(header->fmt_tag, "fmt ", 4);
    header->fmt_size = 16;
    header->audio_format = 1;  // PCM
    header->num_channels = STT_CHANNELS;
    header->sample_rate = STT_SAMPLE_RATE;
    header->byte_rate = STT_SAMPLE_RATE * STT_CHANNELS * (STT_BITS_PER_SAMPLE / 8);
    header->block_align = STT_CHANNELS * (STT_BITS_PER_SAMPLE / 8);
    header->bits_per_sample = STT_BITS_PER_SAMPLE;

    memcpy(header->data_tag, "data", 4);
    header->data_size = pcm_data_size;
}

/**
 * Build multipart/form-data body for Whisper API
 */
static size_t build_multipart_body(uint8_t **body_out, const uint8_t *audio_data, size_t audio_size)
{
    wav_header_t wav_header;
    build_wav_header(&wav_header, audio_size);

    // Part 1: model field
    char part1[256];
    int part1_len = snprintf(part1, sizeof(part1),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
        "%s\r\n",
        MULTIPART_BOUNDARY, WHISPER_MODEL);

    // Part 2: file field header
    char part2_header[256];
    int part2_header_len = snprintf(part2_header, sizeof(part2_header),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n",
        MULTIPART_BOUNDARY);

    // Trailer
    char trailer[128];
    int trailer_len = snprintf(trailer, sizeof(trailer),
        "\r\n--%s--\r\n",
        MULTIPART_BOUNDARY);

    // Total size
    size_t total_size = part1_len + part2_header_len + WAV_HEADER_SIZE + audio_size + trailer_len;

    ESP_LOGI(TAG, "Building multipart body: %d bytes total", total_size);

    // Allocate in PSRAM
    uint8_t *body = heap_caps_malloc(total_size, MALLOC_CAP_SPIRAM);
    if (!body) {
        ESP_LOGE(TAG, "Failed to allocate multipart body (%d bytes)", total_size);
        return 0;
    }

    // Assemble body
    size_t offset = 0;

    memcpy(body + offset, part1, part1_len);
    offset += part1_len;

    memcpy(body + offset, part2_header, part2_header_len);
    offset += part2_header_len;

    memcpy(body + offset, &wav_header, WAV_HEADER_SIZE);
    offset += WAV_HEADER_SIZE;

    memcpy(body + offset, audio_data, audio_size);
    offset += audio_size;

    memcpy(body + offset, trailer, trailer_len);
    offset += trailer_len;

    *body_out = body;
    return total_size;
}

/**
 * HTTP event handler to capture Whisper API response
 */
static esp_err_t whisper_http_event_handler(esp_http_client_event_t *evt)
{
    http_response_t *resp = (http_response_t *)evt->user_data;

    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (resp && resp->buffer) {
                // Append data to response buffer
                size_t new_size = resp->size + evt->data_len;
                if (new_size < resp->capacity) {
                    memcpy(resp->buffer + resp->size, evt->data, evt->data_len);
                    resp->size = new_size;
                    resp->buffer[resp->size] = '\0';  // Null terminate
                } else {
                    ESP_LOGW(TAG, "Response buffer full, truncating");
                }
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(TAG, "HTTP response received: %d bytes", resp ? resp->size : 0);
            break;
        case HTTP_EVENT_ERROR:
            ESP_LOGE(TAG, "HTTP error event");
            break;
        default:
            break;
    }
    return ESP_OK;
}

/**
 * Recording task - reads from microphone and fills buffer
 */
static void recording_task(void *arg)
{
    ESP_LOGI(TAG, "Recording task started");

    // Configure codec for recording (16kHz, 16-bit, stereo I2S but we extract mono)
    esp_err_t err = bsp_extra_codec_set_fs(STT_SAMPLE_RATE, STT_BITS_PER_SAMPLE, I2S_SLOT_MODE_STEREO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure codec: %s", esp_err_to_name(err));
        xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
        s_ctx.state = STT_STATE_ERROR;
        s_ctx.error_message = strdup("Failed to configure audio codec");
        s_ctx.recording_task = NULL;
        xSemaphoreGive(s_ctx.mutex);
        vTaskDelete(NULL);
        return;
    }

    // Allocate chunk buffer (stereo input = 2x mono size)
    uint8_t *chunk_buffer = heap_caps_malloc(RECORDING_CHUNK_SIZE * 2, MALLOC_CAP_INTERNAL);
    if (!chunk_buffer) {
        ESP_LOGE(TAG, "Failed to allocate chunk buffer");
        xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
        s_ctx.state = STT_STATE_ERROR;
        s_ctx.error_message = strdup("Memory allocation failed");
        s_ctx.recording_task = NULL;
        xSemaphoreGive(s_ctx.mutex);
        vTaskDelete(NULL);
        return;
    }

    s_ctx.recording_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    s_ctx.audio_size = 0;

    while (!s_ctx.stop_requested) {
        // Check recording duration limit
        uint32_t elapsed = (xTaskGetTickCount() * portTICK_PERIOD_MS) - s_ctx.recording_start_ms;
        if (elapsed >= MAX_RECORDING_SECONDS * 1000) {
            ESP_LOGI(TAG, "Max recording duration reached (%d seconds)", MAX_RECORDING_SECONDS);
            break;
        }

        // Check buffer capacity
        if (s_ctx.audio_size + RECORDING_CHUNK_SIZE > s_ctx.audio_capacity) {
            ESP_LOGW(TAG, "Audio buffer full (%d bytes)", s_ctx.audio_size);
            break;
        }

        // Read from microphone (stereo I2S data)
        size_t bytes_read = 0;
        err = bsp_extra_i2s_read(chunk_buffer, RECORDING_CHUNK_SIZE * 2, &bytes_read, 100);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "I2S read error: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Convert stereo to mono (take left channel only)
        // Stereo format: L0_lo L0_hi R0_lo R0_hi L1_lo L1_hi R1_lo R1_hi ...
        int16_t *stereo_samples = (int16_t *)chunk_buffer;
        int16_t *mono_dest = (int16_t *)(s_ctx.audio_buffer + s_ctx.audio_size);
        size_t num_stereo_samples = bytes_read / 4;  // 4 bytes per stereo sample pair

        for (size_t i = 0; i < num_stereo_samples; i++) {
            mono_dest[i] = stereo_samples[i * 2];  // Left channel
        }

        s_ctx.audio_size += num_stereo_samples * 2;  // 2 bytes per mono sample

        // Periodic logging
        if ((s_ctx.audio_size % 65536) < RECORDING_CHUNK_SIZE) {
            ESP_LOGI(TAG, "Recording: %d KB, %lu ms",
                     s_ctx.audio_size / 1024, (unsigned long)elapsed);
        }
    }

    s_ctx.recording_duration_ms = (xTaskGetTickCount() * portTICK_PERIOD_MS) - s_ctx.recording_start_ms;
    ESP_LOGI(TAG, "Recording stopped: %d bytes (%.1f KB), %lu ms",
             s_ctx.audio_size, s_ctx.audio_size / 1024.0f,
             (unsigned long)s_ctx.recording_duration_ms);

    heap_caps_free(chunk_buffer);

    // Check minimum recording length
    if (s_ctx.audio_size < STT_SAMPLE_RATE) {  // Less than 0.5 seconds
        ESP_LOGE(TAG, "Recording too short");
        xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
        s_ctx.state = STT_STATE_ERROR;
        s_ctx.error_message = strdup("Recording too short (minimum 0.5 seconds)");
        s_ctx.recording_task = NULL;
        xSemaphoreGive(s_ctx.mutex);
        vTaskDelete(NULL);
        return;
    }

    // Transition to transcribing state
    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    s_ctx.state = STT_STATE_TRANSCRIBING;
    s_ctx.recording_task = NULL;
    xSemaphoreGive(s_ctx.mutex);

    // Start transcription task
    BaseType_t task_created = xTaskCreate(
        transcribe_task,
        "stt_transcribe",
        16384,  // Large stack for HTTPS
        NULL,
        5,
        &s_ctx.transcribe_task
    );

    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create transcription task");
        xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
        s_ctx.state = STT_STATE_ERROR;
        s_ctx.error_message = strdup("Failed to start transcription");
        xSemaphoreGive(s_ctx.mutex);
    }

    vTaskDelete(NULL);
}

/**
 * Transcription task - uploads audio to Whisper API
 */
static void transcribe_task(void *arg)
{
    ESP_LOGI(TAG, "Transcription task started (%d bytes audio)", s_ctx.audio_size);

    // Build multipart request body
    uint8_t *body = NULL;
    size_t body_size = build_multipart_body(&body, s_ctx.audio_buffer, s_ctx.audio_size);
    if (body_size == 0 || !body) {
        xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
        s_ctx.state = STT_STATE_ERROR;
        s_ctx.error_message = strdup("Failed to build request body");
        s_ctx.transcribe_task = NULL;
        xSemaphoreGive(s_ctx.mutex);
        vTaskDelete(NULL);
        return;
    }

    // Allocate response buffer
    http_response_t response = {0};
    response.capacity = 8192;  // 8KB should be enough for transcription JSON
    response.buffer = heap_caps_malloc(response.capacity, MALLOC_CAP_SPIRAM);
    if (!response.buffer) {
        ESP_LOGE(TAG, "Failed to allocate response buffer");
        heap_caps_free(body);
        xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
        s_ctx.state = STT_STATE_ERROR;
        s_ctx.error_message = strdup("Memory allocation failed");
        s_ctx.transcribe_task = NULL;
        xSemaphoreGive(s_ctx.mutex);
        vTaskDelete(NULL);
        return;
    }
    response.size = 0;
    response.buffer[0] = '\0';

    // Configure HTTP client with event handler
    esp_http_client_config_t config = {
        .url = WHISPER_API_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 120000,  // 2 minute timeout for large uploads
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
        .event_handler = whisper_http_event_handler,
        .user_data = &response,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        heap_caps_free(body);
        heap_caps_free(response.buffer);
        xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
        s_ctx.state = STT_STATE_ERROR;
        s_ctx.error_message = strdup("HTTP client init failed");
        s_ctx.transcribe_task = NULL;
        xSemaphoreGive(s_ctx.mutex);
        vTaskDelete(NULL);
        return;
    }

    // Set headers
    char content_type[128];
    snprintf(content_type, sizeof(content_type),
             "multipart/form-data; boundary=%s", MULTIPART_BOUNDARY);
    esp_http_client_set_header(client, "Content-Type", content_type);

#ifdef CONFIG_OPENAI_API_KEY
    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", CONFIG_OPENAI_API_KEY);
    esp_http_client_set_header(client, "Authorization", auth_header);
#else
    ESP_LOGE(TAG, "OpenAI API key not configured");
    heap_caps_free(body);
    heap_caps_free(response.buffer);
    esp_http_client_cleanup(client);
    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    s_ctx.state = STT_STATE_ERROR;
    s_ctx.error_message = strdup("OpenAI API key not configured");
    s_ctx.transcribe_task = NULL;
    xSemaphoreGive(s_ctx.mutex);
    vTaskDelete(NULL);
    return;
#endif

    // Set POST body
    esp_http_client_set_post_field(client, (const char *)body, body_size);

    ESP_LOGI(TAG, "Uploading %d bytes to Whisper API...", body_size);

    // Perform request (response captured via event handler)
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);

        ESP_LOGI(TAG, "HTTP status: %d, response size: %d", status, response.size);

        if (status == 200 && response.size > 0) {
            ESP_LOGI(TAG, "Response: %s", response.buffer);

            // Parse JSON response
            cJSON *root = cJSON_Parse(response.buffer);
            if (root) {
                cJSON *text = cJSON_GetObjectItem(root, "text");
                if (text && cJSON_IsString(text)) {
                    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
                    if (s_ctx.transcription) {
                        free(s_ctx.transcription);
                    }
                    s_ctx.transcription = strdup(text->valuestring);
                    s_ctx.state = STT_STATE_DONE;
                    xSemaphoreGive(s_ctx.mutex);
                    ESP_LOGI(TAG, "Transcription: %.100s%s",
                             s_ctx.transcription,
                             strlen(s_ctx.transcription) > 100 ? "..." : "");
                } else {
                    ESP_LOGE(TAG, "No 'text' field in response");
                    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
                    s_ctx.state = STT_STATE_ERROR;
                    s_ctx.error_message = strdup("Invalid API response format");
                    xSemaphoreGive(s_ctx.mutex);
                }
                cJSON_Delete(root);
            } else {
                ESP_LOGE(TAG, "Failed to parse JSON: %s", response.buffer);
                xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
                s_ctx.state = STT_STATE_ERROR;
                s_ctx.error_message = strdup("Failed to parse API response");
                xSemaphoreGive(s_ctx.mutex);
            }
        } else if (status != 200) {
            ESP_LOGE(TAG, "API error (HTTP %d): %s", status, response.buffer);
            char error_msg[256];
            snprintf(error_msg, sizeof(error_msg), "API error: HTTP %d", status);
            xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
            s_ctx.state = STT_STATE_ERROR;
            s_ctx.error_message = strdup(error_msg);
            xSemaphoreGive(s_ctx.mutex);
        } else {
            ESP_LOGE(TAG, "Empty response from API");
            xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
            s_ctx.state = STT_STATE_ERROR;
            s_ctx.error_message = strdup("Empty response from API");
            xSemaphoreGive(s_ctx.mutex);
        }
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
        s_ctx.state = STT_STATE_ERROR;
        s_ctx.error_message = strdup("Network request failed");
        xSemaphoreGive(s_ctx.mutex);
    }

    // Cleanup
    esp_http_client_cleanup(client);
    heap_caps_free(body);
    heap_caps_free(response.buffer);

    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    s_ctx.transcribe_task = NULL;
    xSemaphoreGive(s_ctx.mutex);

    vTaskDelete(NULL);
}

/**
 * Initialize STT module
 */
esp_err_t stt_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "STT already initialized");
        return ESP_OK;
    }

#ifndef CONFIG_OPENAI_API_KEY
    ESP_LOGE(TAG, "OpenAI API key not configured. Run 'idf.py menuconfig'");
    return ESP_ERR_INVALID_STATE;
#else
    if (strlen(CONFIG_OPENAI_API_KEY) == 0) {
        ESP_LOGE(TAG, "OpenAI API key is empty. Run 'idf.py menuconfig'");
        return ESP_ERR_INVALID_STATE;
    }
#endif

    // Create mutex
    s_ctx.mutex = xSemaphoreCreateMutex();
    if (!s_ctx.mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    // Allocate audio buffer in PSRAM
    s_ctx.audio_capacity = MAX_AUDIO_BUFFER_SIZE;
    s_ctx.audio_buffer = heap_caps_malloc(s_ctx.audio_capacity, MALLOC_CAP_SPIRAM);
    if (!s_ctx.audio_buffer) {
        ESP_LOGE(TAG, "Failed to allocate audio buffer (%d bytes)", s_ctx.audio_capacity);
        vSemaphoreDelete(s_ctx.mutex);
        s_ctx.mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_ctx.state = STT_STATE_IDLE;
    s_ctx.audio_size = 0;
    s_ctx.transcription = NULL;
    s_ctx.error_message = NULL;
    s_ctx.recording_task = NULL;
    s_ctx.transcribe_task = NULL;
    s_ctx.stop_requested = false;

    s_initialized = true;
    ESP_LOGI(TAG, "STT initialized (buffer: %.1f MB, max: %d seconds)",
             s_ctx.audio_capacity / (1024.0f * 1024.0f), MAX_RECORDING_SECONDS);

    return ESP_OK;
}

/**
 * Start recording
 */
esp_err_t stt_start_recording(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "STT not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);

    if (s_ctx.state != STT_STATE_IDLE && s_ctx.state != STT_STATE_DONE && s_ctx.state != STT_STATE_ERROR) {
        ESP_LOGE(TAG, "Cannot start recording in state %d", s_ctx.state);
        xSemaphoreGive(s_ctx.mutex);
        return ESP_ERR_INVALID_STATE;
    }

    // Clear previous results
    if (s_ctx.transcription) {
        free(s_ctx.transcription);
        s_ctx.transcription = NULL;
    }
    if (s_ctx.error_message) {
        free(s_ctx.error_message);
        s_ctx.error_message = NULL;
    }

    s_ctx.state = STT_STATE_RECORDING;
    s_ctx.stop_requested = false;
    s_ctx.audio_size = 0;

    xSemaphoreGive(s_ctx.mutex);

    // Create recording task
    BaseType_t task_created = xTaskCreate(
        recording_task,
        "stt_record",
        4096,
        NULL,
        5,
        &s_ctx.recording_task
    );

    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create recording task");
        xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
        s_ctx.state = STT_STATE_ERROR;
        s_ctx.error_message = strdup("Failed to start recording task");
        xSemaphoreGive(s_ctx.mutex);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Recording started");
    return ESP_OK;
}

/**
 * Stop recording and start transcription
 */
esp_err_t stt_stop_recording(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "STT not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);

    if (s_ctx.state != STT_STATE_RECORDING) {
        ESP_LOGE(TAG, "Not recording (state: %d)", s_ctx.state);
        xSemaphoreGive(s_ctx.mutex);
        return ESP_ERR_INVALID_STATE;
    }

    s_ctx.stop_requested = true;
    xSemaphoreGive(s_ctx.mutex);

    ESP_LOGI(TAG, "Stop requested");
    return ESP_OK;
}

/**
 * Get current status
 */
esp_err_t stt_get_status(stt_status_t *status)
{
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_initialized) {
        status->state = STT_STATE_IDLE;
        status->transcription = NULL;
        status->error_message = NULL;
        status->recording_ms = 0;
        status->audio_bytes = 0;
        return ESP_OK;
    }

    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);

    status->state = s_ctx.state;
    status->transcription = s_ctx.transcription;
    status->error_message = s_ctx.error_message;
    status->audio_bytes = s_ctx.audio_size;

    if (s_ctx.state == STT_STATE_RECORDING) {
        status->recording_ms = (xTaskGetTickCount() * portTICK_PERIOD_MS) - s_ctx.recording_start_ms;
    } else {
        status->recording_ms = s_ctx.recording_duration_ms;
    }

    xSemaphoreGive(s_ctx.mutex);

    return ESP_OK;
}

/**
 * Get current state
 */
stt_state_t stt_get_state(void)
{
    if (!s_initialized) {
        return STT_STATE_IDLE;
    }

    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    stt_state_t state = s_ctx.state;
    xSemaphoreGive(s_ctx.mutex);

    return state;
}

/**
 * Reset to idle state
 */
esp_err_t stt_reset(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);

    if (s_ctx.state == STT_STATE_RECORDING || s_ctx.state == STT_STATE_TRANSCRIBING) {
        ESP_LOGE(TAG, "Cannot reset while busy");
        xSemaphoreGive(s_ctx.mutex);
        return ESP_ERR_INVALID_STATE;
    }

    if (s_ctx.transcription) {
        free(s_ctx.transcription);
        s_ctx.transcription = NULL;
    }
    if (s_ctx.error_message) {
        free(s_ctx.error_message);
        s_ctx.error_message = NULL;
    }

    s_ctx.state = STT_STATE_IDLE;
    s_ctx.audio_size = 0;
    s_ctx.recording_duration_ms = 0;

    xSemaphoreGive(s_ctx.mutex);

    ESP_LOGI(TAG, "Reset to idle");
    return ESP_OK;
}

/**
 * Check if busy
 */
bool stt_is_busy(void)
{
    if (!s_initialized) {
        return false;
    }

    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    bool busy = (s_ctx.state == STT_STATE_RECORDING || s_ctx.state == STT_STATE_TRANSCRIBING);
    xSemaphoreGive(s_ctx.mutex);

    return busy;
}

/**
 * Cleanup resources
 */
void stt_cleanup(void)
{
    if (!s_initialized) {
        return;
    }

    // Stop any ongoing operations
    s_ctx.stop_requested = true;

    // Wait for tasks to finish
    int wait_count = 0;
    while ((s_ctx.recording_task || s_ctx.transcribe_task) && wait_count < 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
        wait_count++;
    }

    // Force delete if still running
    if (s_ctx.recording_task) {
        vTaskDelete(s_ctx.recording_task);
        s_ctx.recording_task = NULL;
    }
    if (s_ctx.transcribe_task) {
        vTaskDelete(s_ctx.transcribe_task);
        s_ctx.transcribe_task = NULL;
    }

    // Free resources
    if (s_ctx.audio_buffer) {
        heap_caps_free(s_ctx.audio_buffer);
        s_ctx.audio_buffer = NULL;
    }
    if (s_ctx.transcription) {
        free(s_ctx.transcription);
        s_ctx.transcription = NULL;
    }
    if (s_ctx.error_message) {
        free(s_ctx.error_message);
        s_ctx.error_message = NULL;
    }
    if (s_ctx.mutex) {
        vSemaphoreDelete(s_ctx.mutex);
        s_ctx.mutex = NULL;
    }

    s_initialized = false;
    ESP_LOGI(TAG, "STT cleaned up");
}
