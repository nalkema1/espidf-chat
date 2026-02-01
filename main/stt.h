/**
 * Speech-to-Text Module
 *
 * Records audio from microphone and transcribes via OpenAI Whisper API.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief STT state machine states
 */
typedef enum {
    STT_STATE_IDLE = 0,       // Ready to record
    STT_STATE_RECORDING,      // Currently recording
    STT_STATE_TRANSCRIBING,   // Uploading and waiting for transcription
    STT_STATE_DONE,           // Transcription complete
    STT_STATE_ERROR,          // Error occurred
} stt_state_t;

/**
 * @brief STT status structure (for API responses)
 */
typedef struct {
    stt_state_t state;
    const char *transcription;  // NULL if not available
    const char *error_message;  // NULL if no error
    uint32_t recording_ms;      // Duration of recording in ms
    size_t audio_bytes;         // Size of audio data
} stt_status_t;

/**
 * @brief Initialize STT module
 *
 * Allocates audio buffer in PSRAM and initializes state.
 *
 * @return ESP_OK on success, error code on failure
 */
esp_err_t stt_init(void);

/**
 * @brief Start recording audio from microphone
 *
 * Begins capturing audio into the internal buffer.
 * Recording continues until stt_stop_recording() is called
 * or max duration (5 minutes) is reached.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if already recording
 */
esp_err_t stt_start_recording(void);

/**
 * @brief Stop recording and start transcription
 *
 * Stops audio capture and initiates upload to Whisper API.
 * Transcription happens asynchronously - poll stt_get_status()
 * to check for completion.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not recording
 */
esp_err_t stt_stop_recording(void);

/**
 * @brief Get current STT status
 *
 * @param status Pointer to status structure to fill
 * @return ESP_OK on success
 */
esp_err_t stt_get_status(stt_status_t *status);

/**
 * @brief Get current state
 *
 * @return Current STT state
 */
stt_state_t stt_get_state(void);

/**
 * @brief Reset STT to idle state
 *
 * Clears transcription result and error message.
 * Can only be called when in DONE or ERROR state.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if busy
 */
esp_err_t stt_reset(void);

/**
 * @brief Check if STT is busy
 *
 * @return true if recording or transcribing, false if idle/done/error
 */
bool stt_is_busy(void);

/**
 * @brief Cleanup STT resources
 *
 * Frees audio buffer and stops any ongoing operations.
 */
void stt_cleanup(void);

#ifdef __cplusplus
}
#endif
