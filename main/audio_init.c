/**
 * Audio initialization for WiFi connection notification
 */

#include "audio_init.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"
#include "audio_player.h"

static const char *TAG = "audio_init";

// Path to WAV file on SD card
#define WAV_FILE_PATH "/sdcard/house_lo.wav"

static void audio_player_callback(audio_player_cb_ctx_t *ctx)
{
    switch (ctx->audio_event) {
        case AUDIO_PLAYER_CALLBACK_EVENT_IDLE:
            ESP_LOGI(TAG, "Playback finished");
            break;
        case AUDIO_PLAYER_CALLBACK_EVENT_PLAYING:
            ESP_LOGI(TAG, "Playback started");
            break;
        case AUDIO_PLAYER_CALLBACK_EVENT_PAUSE:
            ESP_LOGI(TAG, "Playback paused");
            break;
        default:
            break;
    }
}

esp_err_t audio_play_wifi_connected(void)
{
    esp_err_t err;

    // Mount SD card
    err = bsp_sdcard_mount();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "SD card mounted at /sdcard");

    // Initialize audio codec (ES8311)
    err = bsp_extra_codec_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize audio codec: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Audio codec initialized");

    // Set volume (0-100)
    int volume_set;
    bsp_extra_codec_volume_set(80, &volume_set);
    ESP_LOGI(TAG, "Volume set to %d", volume_set);

    // Initialize audio player
    err = bsp_extra_player_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize audio player: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Audio player initialized");

    // Register callback for playback events
    bsp_extra_player_register_callback(audio_player_callback, NULL);

    // Play the notification sound once
    ESP_LOGI(TAG, "Playing %s", WAV_FILE_PATH);
    err = bsp_extra_player_play_file(WAV_FILE_PATH);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to play file: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "Make sure '%s' exists on the SD card", WAV_FILE_PATH);
        return err;
    }

    return ESP_OK;
}
