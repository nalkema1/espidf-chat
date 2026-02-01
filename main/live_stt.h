/**
 * Live Speech-to-Text Module
 *
 * Real-time audio streaming and transcription via Deepgram WebSocket API.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Live STT state machine states
 */
typedef enum {
    LIVE_STT_STATE_IDLE = 0,      // Ready to stream
    LIVE_STT_STATE_CONNECTING,    // Connecting to Deepgram
    LIVE_STT_STATE_STREAMING,     // Actively streaming audio
    LIVE_STT_STATE_ERROR,         // Error occurred
} live_stt_state_t;

/**
 * @brief Live STT status structure (for API responses)
 */
typedef struct {
    live_stt_state_t state;
    const char *transcript;       // Accumulated transcript (NULL if empty)
    const char *error_message;    // NULL if no error
} live_stt_status_t;

/**
 * @brief Initialize Live STT module
 *
 * Allocates transcript buffer in PSRAM and initializes state.
 *
 * @return ESP_OK on success, error code on failure
 */
esp_err_t live_stt_init(void);

/**
 * @brief Start live transcription
 *
 * Connects to Deepgram WebSocket API and begins streaming
 * audio from the microphone in real-time.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if already streaming
 */
esp_err_t live_stt_start(void);

/**
 * @brief Stop live transcription
 *
 * Stops audio streaming and closes WebSocket connection.
 *
 * @return ESP_OK on success
 */
esp_err_t live_stt_stop(void);

/**
 * @brief Get current Live STT status
 *
 * @param status Pointer to status structure to fill
 * @return ESP_OK on success
 */
esp_err_t live_stt_get_status(live_stt_status_t *status);

/**
 * @brief Get current state
 *
 * @return Current Live STT state
 */
live_stt_state_t live_stt_get_state(void);

/**
 * @brief Get accumulated transcript
 *
 * @return Pointer to transcript string, or NULL if empty
 */
const char *live_stt_get_transcript(void);

/**
 * @brief Clear accumulated transcript
 */
void live_stt_clear_transcript(void);

/**
 * @brief Check if Live STT is busy
 *
 * @return true if connecting or streaming, false otherwise
 */
bool live_stt_is_busy(void);

/**
 * @brief Cleanup Live STT resources
 *
 * Frees transcript buffer and stops any ongoing operations.
 */
void live_stt_cleanup(void);

#ifdef __cplusplus
}
#endif
