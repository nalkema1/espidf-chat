/**
 * OpenAI Realtime API Live Speech-to-Text Module
 *
 * Real-time audio streaming and transcription via OpenAI's Realtime API.
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * OpenAI Live STT states
 */
typedef enum {
    OPENAI_LIVE_STT_STATE_IDLE = 0,
    OPENAI_LIVE_STT_STATE_CONNECTING,
    OPENAI_LIVE_STT_STATE_STREAMING,
    OPENAI_LIVE_STT_STATE_ERROR,
} openai_live_stt_state_t;

/**
 * Status structure for API responses
 */
typedef struct {
    openai_live_stt_state_t state;
    const char *transcript;
    const char *error_message;
} openai_live_stt_status_t;

/**
 * Initialize OpenAI Live STT module
 * @return ESP_OK on success
 */
esp_err_t openai_live_stt_init(void);

/**
 * Start live transcription
 * Connects to OpenAI Realtime API and begins streaming audio
 * @return ESP_OK on success
 */
esp_err_t openai_live_stt_start(void);

/**
 * Stop live transcription
 * Stops audio streaming and disconnects from API
 * @return ESP_OK on success
 */
esp_err_t openai_live_stt_stop(void);

/**
 * Get current state
 * @return Current state
 */
openai_live_stt_state_t openai_live_stt_get_state(void);

/**
 * Get accumulated transcript
 * @return Pointer to transcript string or NULL if empty
 */
const char *openai_live_stt_get_transcript(void);

/**
 * Get full status including state, transcript, and error
 * @param status Pointer to status structure to fill
 * @return ESP_OK on success
 */
esp_err_t openai_live_stt_get_status(openai_live_stt_status_t *status);

/**
 * Clear accumulated transcript
 */
void openai_live_stt_clear_transcript(void);

/**
 * Check if module is busy (connecting or streaming)
 * @return true if busy
 */
bool openai_live_stt_is_busy(void);

/**
 * Cleanup resources
 */
void openai_live_stt_cleanup(void);

#ifdef __cplusplus
}
#endif
