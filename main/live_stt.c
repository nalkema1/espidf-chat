/**
 * Live Speech-to-Text Module
 *
 * Real-time audio streaming and transcription via Deepgram WebSocket API.
 */

#include "live_stt.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "bsp_board_extra.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "live_stt";

// Audio configuration (matches batch STT)
#define LIVE_STT_SAMPLE_RATE 16000
#define LIVE_STT_BITS_PER_SAMPLE 16

// Streaming chunk configuration
// 200ms chunks at 16kHz, 16-bit mono = 6400 bytes
#define CHUNK_DURATION_MS 200
#define CHUNK_SIZE_BYTES ((LIVE_STT_SAMPLE_RATE * 2 * CHUNK_DURATION_MS) / 1000)

// Transcript buffer size (32KB in PSRAM)
#define TRANSCRIPT_BUFFER_SIZE (32 * 1024)

// Deepgram WebSocket URL with query parameters
#define DEEPGRAM_WS_URL "wss://api.deepgram.com/v1/listen?encoding=linear16&sample_rate=16000&channels=1&punctuate=true&interim_results=false"

/**
 * Module state
 */
typedef struct {
    live_stt_state_t state;
    char *transcript;               // Accumulated transcript (PSRAM)
    size_t transcript_len;          // Current transcript length
    size_t transcript_capacity;     // Buffer capacity
    char *error_message;            // Error message (heap allocated)
    SemaphoreHandle_t mutex;        // Protect state access
    esp_websocket_client_handle_t ws_client;  // WebSocket client
    TaskHandle_t streaming_task;    // Audio streaming task handle
    volatile bool stop_requested;   // Signal to stop streaming
} live_stt_context_t;

static live_stt_context_t s_ctx = {0};
static bool s_initialized = false;

// Forward declarations
static void streaming_task(void *arg);
static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
static void parse_deepgram_response(const char *data, int len);

/**
 * Check if Deepgram API key is configured
 */
static bool is_deepgram_configured(void)
{
#ifdef CONFIG_DEEPGRAM_API_KEY
    return strlen(CONFIG_DEEPGRAM_API_KEY) > 0;
#else
    return false;
#endif
}

/**
 * Initialize Live STT module
 */
esp_err_t live_stt_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    if (!is_deepgram_configured()) {
        ESP_LOGE(TAG, "Deepgram API key not configured. Use 'idf.py menuconfig' to set it.");
        return ESP_ERR_INVALID_STATE;
    }

    // Create mutex
    s_ctx.mutex = xSemaphoreCreateMutex();
    if (!s_ctx.mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    // Allocate transcript buffer in PSRAM
    s_ctx.transcript = heap_caps_calloc(1, TRANSCRIPT_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_ctx.transcript) {
        ESP_LOGE(TAG, "Failed to allocate transcript buffer in PSRAM");
        vSemaphoreDelete(s_ctx.mutex);
        return ESP_ERR_NO_MEM;
    }
    s_ctx.transcript_capacity = TRANSCRIPT_BUFFER_SIZE;
    s_ctx.transcript_len = 0;

    s_ctx.state = LIVE_STT_STATE_IDLE;
    s_initialized = true;

    ESP_LOGI(TAG, "Live STT initialized (transcript buffer: %d KB)", TRANSCRIPT_BUFFER_SIZE / 1024);
    return ESP_OK;
}

/**
 * Start live transcription
 */
esp_err_t live_stt_start(void)
{
    if (!s_initialized) {
        esp_err_t err = live_stt_init();
        if (err != ESP_OK) {
            return err;
        }
    }

    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);

    if (s_ctx.state == LIVE_STT_STATE_STREAMING || s_ctx.state == LIVE_STT_STATE_CONNECTING) {
        xSemaphoreGive(s_ctx.mutex);
        return ESP_ERR_INVALID_STATE;
    }

    // Clear previous error
    if (s_ctx.error_message) {
        free(s_ctx.error_message);
        s_ctx.error_message = NULL;
    }

    s_ctx.state = LIVE_STT_STATE_CONNECTING;
    s_ctx.stop_requested = false;

    xSemaphoreGive(s_ctx.mutex);

#ifdef CONFIG_DEEPGRAM_API_KEY
    // Build authorization header
    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Token %s", CONFIG_DEEPGRAM_API_KEY);

    // Configure WebSocket client with SSL certificate bundle
    esp_websocket_client_config_t ws_cfg = {
        .uri = DEEPGRAM_WS_URL,
        .buffer_size = 8192,
        .task_stack = 8192,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    s_ctx.ws_client = esp_websocket_client_init(&ws_cfg);
    if (!s_ctx.ws_client) {
        ESP_LOGE(TAG, "Failed to create WebSocket client");
        xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
        s_ctx.state = LIVE_STT_STATE_ERROR;
        s_ctx.error_message = strdup("Failed to create WebSocket client");
        xSemaphoreGive(s_ctx.mutex);
        return ESP_FAIL;
    }

    // Set authorization header
    esp_websocket_client_append_header(s_ctx.ws_client, "Authorization", auth_header);

    // Register event handler
    esp_websocket_register_events(s_ctx.ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);

    // Start WebSocket connection
    esp_err_t err = esp_websocket_client_start(s_ctx.ws_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket client: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(s_ctx.ws_client);
        s_ctx.ws_client = NULL;
        xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
        s_ctx.state = LIVE_STT_STATE_ERROR;
        s_ctx.error_message = strdup("Failed to connect to Deepgram");
        xSemaphoreGive(s_ctx.mutex);
        return err;
    }

    ESP_LOGI(TAG, "Connecting to Deepgram...");
    return ESP_OK;
#else
    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    s_ctx.state = LIVE_STT_STATE_ERROR;
    s_ctx.error_message = strdup("Deepgram API key not configured");
    xSemaphoreGive(s_ctx.mutex);
    return ESP_ERR_INVALID_STATE;
#endif
}

/**
 * Stop live transcription
 */
esp_err_t live_stt_stop(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);

    if (s_ctx.state != LIVE_STT_STATE_STREAMING && s_ctx.state != LIVE_STT_STATE_CONNECTING) {
        xSemaphoreGive(s_ctx.mutex);
        return ESP_OK;
    }

    s_ctx.stop_requested = true;
    xSemaphoreGive(s_ctx.mutex);

    // Wait for streaming task to finish
    if (s_ctx.streaming_task) {
        for (int i = 0; i < 50; i++) {  // Wait up to 5 seconds
            if (s_ctx.streaming_task == NULL) break;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    // Close WebSocket
    if (s_ctx.ws_client) {
        esp_websocket_client_stop(s_ctx.ws_client);
        esp_websocket_client_destroy(s_ctx.ws_client);
        s_ctx.ws_client = NULL;
    }

    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    s_ctx.state = LIVE_STT_STATE_IDLE;
    xSemaphoreGive(s_ctx.mutex);

    ESP_LOGI(TAG, "Live STT stopped");
    return ESP_OK;
}

/**
 * Get current status
 */
esp_err_t live_stt_get_status(live_stt_status_t *status)
{
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_initialized) {
        status->state = LIVE_STT_STATE_IDLE;
        status->transcript = NULL;
        status->error_message = NULL;
        return ESP_OK;
    }

    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    status->state = s_ctx.state;
    status->transcript = (s_ctx.transcript_len > 0) ? s_ctx.transcript : NULL;
    status->error_message = s_ctx.error_message;
    xSemaphoreGive(s_ctx.mutex);

    return ESP_OK;
}

/**
 * Get current state
 */
live_stt_state_t live_stt_get_state(void)
{
    if (!s_initialized) {
        return LIVE_STT_STATE_IDLE;
    }
    return s_ctx.state;
}

/**
 * Get accumulated transcript
 */
const char *live_stt_get_transcript(void)
{
    if (!s_initialized || s_ctx.transcript_len == 0) {
        return NULL;
    }
    return s_ctx.transcript;
}

/**
 * Clear accumulated transcript
 */
void live_stt_clear_transcript(void)
{
    if (!s_initialized) {
        return;
    }

    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    if (s_ctx.transcript) {
        s_ctx.transcript[0] = '\0';
        s_ctx.transcript_len = 0;
    }
    xSemaphoreGive(s_ctx.mutex);
}

/**
 * Check if busy
 */
bool live_stt_is_busy(void)
{
    if (!s_initialized) {
        return false;
    }
    return s_ctx.state == LIVE_STT_STATE_STREAMING || s_ctx.state == LIVE_STT_STATE_CONNECTING;
}

/**
 * Cleanup
 */
void live_stt_cleanup(void)
{
    if (!s_initialized) {
        return;
    }

    live_stt_stop();

    if (s_ctx.transcript) {
        heap_caps_free(s_ctx.transcript);
        s_ctx.transcript = NULL;
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
    ESP_LOGI(TAG, "Live STT cleaned up");
}

/**
 * WebSocket event handler
 */
static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WebSocket connected to Deepgram");
            xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
            s_ctx.state = LIVE_STT_STATE_STREAMING;
            xSemaphoreGive(s_ctx.mutex);

            // Start audio streaming task
            BaseType_t ret = xTaskCreate(streaming_task, "live_stt_stream", 8192, NULL, 5, &s_ctx.streaming_task);
            if (ret != pdPASS) {
                ESP_LOGE(TAG, "Failed to create streaming task");
                xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
                s_ctx.state = LIVE_STT_STATE_ERROR;
                s_ctx.error_message = strdup("Failed to start audio streaming");
                xSemaphoreGive(s_ctx.mutex);
            }
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "WebSocket disconnected");
            xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
            if (s_ctx.state == LIVE_STT_STATE_STREAMING) {
                s_ctx.stop_requested = true;
            }
            if (!s_ctx.stop_requested) {
                s_ctx.state = LIVE_STT_STATE_ERROR;
                if (!s_ctx.error_message) {
                    s_ctx.error_message = strdup("Connection lost");
                }
            } else {
                s_ctx.state = LIVE_STT_STATE_IDLE;
            }
            xSemaphoreGive(s_ctx.mutex);
            break;

        case WEBSOCKET_EVENT_DATA:
            if (data->op_code == 0x01) {  // Text frame
                ESP_LOGD(TAG, "Received: %.*s", data->data_len, (char *)data->data_ptr);
                parse_deepgram_response((const char *)data->data_ptr, data->data_len);
            }
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket error");
            xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
            s_ctx.state = LIVE_STT_STATE_ERROR;
            if (!s_ctx.error_message) {
                s_ctx.error_message = strdup("WebSocket error");
            }
            xSemaphoreGive(s_ctx.mutex);
            break;

        default:
            break;
    }
}

/**
 * Parse Deepgram JSON response and append transcript
 */
static void parse_deepgram_response(const char *data, int len)
{
    // Make a null-terminated copy
    char *json_str = malloc(len + 1);
    if (!json_str) {
        return;
    }
    memcpy(json_str, data, len);
    json_str[len] = '\0';

    cJSON *root = cJSON_Parse(json_str);
    free(json_str);

    if (!root) {
        ESP_LOGW(TAG, "Failed to parse Deepgram response");
        return;
    }

    // Check for error
    cJSON *error = cJSON_GetObjectItem(root, "error");
    if (error) {
        cJSON *message = cJSON_GetObjectItem(error, "message");
        if (message && cJSON_IsString(message)) {
            ESP_LOGE(TAG, "Deepgram error: %s", message->valuestring);
            xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
            if (s_ctx.error_message) free(s_ctx.error_message);
            s_ctx.error_message = strdup(message->valuestring);
            s_ctx.state = LIVE_STT_STATE_ERROR;
            xSemaphoreGive(s_ctx.mutex);
        }
        cJSON_Delete(root);
        return;
    }

    // Extract transcript from channel.alternatives[0].transcript
    cJSON *channel = cJSON_GetObjectItem(root, "channel");
    if (!channel) {
        cJSON_Delete(root);
        return;
    }

    cJSON *alternatives = cJSON_GetObjectItem(channel, "alternatives");
    if (!alternatives || !cJSON_IsArray(alternatives) || cJSON_GetArraySize(alternatives) == 0) {
        cJSON_Delete(root);
        return;
    }

    cJSON *first_alt = cJSON_GetArrayItem(alternatives, 0);
    if (!first_alt) {
        cJSON_Delete(root);
        return;
    }

    cJSON *transcript = cJSON_GetObjectItem(first_alt, "transcript");
    if (!transcript || !cJSON_IsString(transcript) || strlen(transcript->valuestring) == 0) {
        cJSON_Delete(root);
        return;
    }

    // Append to accumulated transcript
    const char *text = transcript->valuestring;
    size_t text_len = strlen(text);

    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);

    // Add space before new text if not at start
    if (s_ctx.transcript_len > 0 && s_ctx.transcript_len + 1 < s_ctx.transcript_capacity) {
        s_ctx.transcript[s_ctx.transcript_len] = ' ';
        s_ctx.transcript_len++;
    }

    // Append text
    size_t copy_len = text_len;
    if (s_ctx.transcript_len + copy_len >= s_ctx.transcript_capacity) {
        copy_len = s_ctx.transcript_capacity - s_ctx.transcript_len - 1;
    }

    if (copy_len > 0) {
        memcpy(s_ctx.transcript + s_ctx.transcript_len, text, copy_len);
        s_ctx.transcript_len += copy_len;
        s_ctx.transcript[s_ctx.transcript_len] = '\0';
        ESP_LOGI(TAG, "Transcript: %s", text);
    }

    xSemaphoreGive(s_ctx.mutex);
    cJSON_Delete(root);
}

/**
 * Audio streaming task - reads from microphone and sends to WebSocket
 */
static void streaming_task(void *arg)
{
    ESP_LOGI(TAG, "Streaming task started");

    // Configure codec for recording (16kHz, 16-bit, stereo I2S but we extract mono)
    esp_err_t err = bsp_extra_codec_set_fs(LIVE_STT_SAMPLE_RATE, LIVE_STT_BITS_PER_SAMPLE, I2S_SLOT_MODE_STEREO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure codec: %s", esp_err_to_name(err));
        xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
        s_ctx.state = LIVE_STT_STATE_ERROR;
        s_ctx.error_message = strdup("Failed to configure audio codec");
        s_ctx.streaming_task = NULL;
        xSemaphoreGive(s_ctx.mutex);
        vTaskDelete(NULL);
        return;
    }

    // Allocate chunk buffers (stereo input = 2x mono size)
    uint8_t *stereo_chunk = heap_caps_malloc(CHUNK_SIZE_BYTES * 2, MALLOC_CAP_INTERNAL);
    uint8_t *mono_chunk = heap_caps_malloc(CHUNK_SIZE_BYTES, MALLOC_CAP_INTERNAL);

    if (!stereo_chunk || !mono_chunk) {
        ESP_LOGE(TAG, "Failed to allocate chunk buffers");
        if (stereo_chunk) heap_caps_free(stereo_chunk);
        if (mono_chunk) heap_caps_free(mono_chunk);
        xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
        s_ctx.state = LIVE_STT_STATE_ERROR;
        s_ctx.error_message = strdup("Memory allocation failed");
        s_ctx.streaming_task = NULL;
        xSemaphoreGive(s_ctx.mutex);
        vTaskDelete(NULL);
        return;
    }

    uint32_t chunk_count = 0;

    while (!s_ctx.stop_requested && s_ctx.ws_client && esp_websocket_client_is_connected(s_ctx.ws_client)) {
        // Read from microphone (stereo I2S data)
        size_t bytes_read = 0;
        err = bsp_extra_i2s_read(stereo_chunk, CHUNK_SIZE_BYTES * 2, &bytes_read, CHUNK_DURATION_MS + 50);

        if (err != ESP_OK || bytes_read == 0) {
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "I2S read error: %s", esp_err_to_name(err));
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Convert stereo to mono (take left channel only)
        int16_t *stereo_samples = (int16_t *)stereo_chunk;
        int16_t *mono_samples = (int16_t *)mono_chunk;
        size_t num_stereo_samples = bytes_read / 4;  // 4 bytes per stereo sample pair

        for (size_t i = 0; i < num_stereo_samples; i++) {
            mono_samples[i] = stereo_samples[i * 2];  // Left channel
        }

        size_t mono_size = num_stereo_samples * 2;

        // Send via WebSocket
        int sent = esp_websocket_client_send_bin(s_ctx.ws_client, (const char *)mono_chunk, mono_size, pdMS_TO_TICKS(1000));
        if (sent < 0) {
            ESP_LOGW(TAG, "WebSocket send failed");
        } else {
            chunk_count++;
            if ((chunk_count % 25) == 0) {  // Log every 5 seconds
                ESP_LOGI(TAG, "Streamed %lu chunks (%.1f seconds)",
                         (unsigned long)chunk_count, chunk_count * CHUNK_DURATION_MS / 1000.0f);
            }
        }
    }

    ESP_LOGI(TAG, "Streaming task stopped after %lu chunks", (unsigned long)chunk_count);

    heap_caps_free(stereo_chunk);
    heap_caps_free(mono_chunk);

    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    s_ctx.streaming_task = NULL;
    xSemaphoreGive(s_ctx.mutex);

    vTaskDelete(NULL);
}
