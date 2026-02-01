# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Build
idf.py build

# Flash and monitor (replace COM3 with your port)
idf.py -p COM3 flash monitor

# Configure WiFi credentials and other settings
idf.py menuconfig

# Clean build (use when changing targets or after major changes)
idf.py fullclean && idf.py build

# Set target (only needed once or after fullclean)
idf.py set-target esp32p4
```

## Architecture

This is an ESP32-P4 project using ESP-IDF v5.5.2 for the Waveshare ESP32-P4-WIFI6-M board.

### Dual-Processor WiFi Architecture
- **ESP32-P4**: Main processor running this application
- **ESP32-C6**: Co-processor handling WiFi via ESP-Hosted over SDIO
- WiFi is 2.4GHz only (5GHz not supported)

### Application Flow
1. `app_main()` initializes NVS and starts WiFi
2. WiFi event handler manages connection states
3. On `IP_EVENT_STA_GOT_IP`: plays WAV notification, speaks TTS message, starts HTTP server
4. HTTP server serves status page on port 80

### Audio System Stack
```
WAV/MP3 files on SD card (/sdcard/)
    → esp-audio-player component
    → ES8311 codec (bsp_extra wrapper)
    → I2S output to speaker
```

### TTS System Stack (ElevenLabs)
```
Text → HTTPS POST to ElevenLabs /stream endpoint
    → PCM audio chunks (16kHz, 16-bit)
    → Ring buffer (64KB, smooths network jitter)
    → bsp_extra_i2s_write() → ES8311 codec → Speaker
```

## Key Components

| Path | Purpose |
|------|---------|
| `main/main.c` | Entry point, WiFi initialization and event handling |
| `main/http_server.c` | HTTP endpoints: `/` (HTML), `/api/status` (JSON) |
| `main/audio_init.c` | WAV playback triggered on WiFi connect |
| `main/tts.c` | ElevenLabs TTS with PCM streaming to I2S |
| `components/bsp_extra/` | Custom audio codec wrapper (ES8311 + audio player) |

## Dependencies

Managed via `main/idf_component.yml`:
- `espressif/esp_wifi_remote` + `esp_hosted` - WiFi via ESP32-C6
- `waveshare/esp32_p4_nano` - Board support package
- `chmorgan/esp-audio-player` - Audio playback
- `espressif/cjson` - JSON parsing for TTS API

## Configuration

- **WiFi credentials**: Set via `idf.py menuconfig` → "WiFi Configuration"
- **ElevenLabs API key**: Set via `idf.py menuconfig` → "ElevenLabs TTS Configuration"
- **sdkconfig.defaults**: PSRAM, audio codecs, HTTPS certs, custom partitions
- **partitions.csv**: 4MB factory app partition

## Hardware Notes

- SD card must contain audio files (e.g., `/sdcard/house_lo.wav`)
- PSRAM required for audio buffering (enabled in sdkconfig.defaults)
- If WiFi shows MAC 00:00:00:00:00:00, try 1-bit SDIO mode in menuconfig
