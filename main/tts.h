/**
 * Text-to-Speech Module
 *
 * Streams PCM audio from ElevenLabs or OpenAI API directly to I2S output.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief TTS provider selection
 */
typedef enum {
    TTS_PROVIDER_ELEVENLABS = 0,
    TTS_PROVIDER_OPENAI = 1,
} tts_provider_t;

/**
 * @brief Initialize the TTS module
 *
 * Must be called after WiFi is connected and audio codec is initialized.
 * Creates ring buffer and playback task.
 *
 * @return ESP_OK on success, error code on failure
 */
esp_err_t tts_init(void);

/**
 * @brief Set the TTS provider
 *
 * @param provider The provider to use (TTS_PROVIDER_ELEVENLABS or TTS_PROVIDER_OPENAI)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if provider not configured
 */
esp_err_t tts_set_provider(tts_provider_t provider);

/**
 * @brief Get the current TTS provider
 *
 * @return The current provider
 */
tts_provider_t tts_get_provider(void);

/**
 * @brief Get the name of a TTS provider
 *
 * @param provider The provider
 * @return Provider name string
 */
const char *tts_get_provider_name(tts_provider_t provider);

/**
 * @brief Check if a provider is available (API key configured)
 *
 * @param provider The provider to check
 * @return true if available, false otherwise
 */
bool tts_is_provider_available(tts_provider_t provider);

/**
 * @brief Speak the given text using current TTS provider
 *
 * Streams PCM audio from TTS API and plays it through the speaker.
 * Blocks until playback is complete. Uses default speed (1.0x).
 *
 * @param text The text to synthesize (max 5000 chars recommended)
 * @return ESP_OK on success, error code on failure
 */
esp_err_t tts_speak(const char *text);

/**
 * @brief Speak the given text with custom speed
 *
 * Streams PCM audio from TTS API and plays it through the speaker.
 * Blocks until playback is complete.
 *
 * @param text The text to synthesize (max 5000 chars recommended)
 * @param speed Speech speed multiplier (ElevenLabs: 0.5-2.0, OpenAI: 0.25-4.0)
 * @return ESP_OK on success, error code on failure
 */
esp_err_t tts_speak_with_speed(const char *text, float speed);

/**
 * @brief Speak a test message
 *
 * Uses hardcoded sample text for debugging.
 *
 * @return ESP_OK on success, error code on failure
 */
esp_err_t tts_speak_test(void);

/**
 * @brief Stop current TTS playback
 *
 * Stops any ongoing TTS streaming and playback.
 *
 * @return ESP_OK on success
 */
esp_err_t tts_stop(void);

/**
 * @brief Check if TTS is currently playing
 *
 * @return true if TTS is playing, false otherwise
 */
bool tts_is_playing(void);

/**
 * @brief Clean up TTS resources
 *
 * Frees ring buffer and deletes playback task.
 */
void tts_cleanup(void);

#ifdef __cplusplus
}
#endif
