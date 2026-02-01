/**
 * Audio initialization for WiFi connection notification
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize and play the WiFi connected notification sound.
 *
 * This function initializes the SD card, audio codec, and audio player,
 * then plays the notification sound once.
 *
 * @return
 *      - ESP_OK: Success
 *      - Others: Fail
 */
esp_err_t audio_play_wifi_connected(void);

#ifdef __cplusplus
}
#endif
