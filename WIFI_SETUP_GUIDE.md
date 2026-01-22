# ESP32-P4-WIFI6-M WiFi Setup Guide

## Board Overview

The Waveshare ESP32-P4-WIFI6-M is a development board based on the ESP32-P4 with an integrated ESP32-C6 co-processor for WiFi 6 and Bluetooth 5 connectivity. The ESP32-P4 itself does not have built-in WiFi - it communicates with the ESP32-C6 via SDIO using the ESP-Hosted library.

## Documentation Links

### Waveshare Resources
- **Wiki Page**: https://www.waveshare.com/wiki/ESP32-P4-WIFI6
- **Product Page**: https://www.waveshare.com/esp32-p4-wifi6.htm
- **Datasheet/Schematic PDF**: https://files.waveshare.com/wiki/ESP32-P4-WIFI6/ESP32-P4-WIFI6-datasheet.pdf
- **Demo Code (ZIP)**: https://files.waveshare.com/wiki/ESP32-P4-NANO/ESP32-P4-NANO_Demo.zip

### Espressif Resources
- **ESP-Hosted MCU GitHub**: https://github.com/espressif/esp-hosted-mcu
- **ESP-Hosted SDIO Documentation**: https://github.com/espressif/esp-hosted-mcu/blob/main/docs/sdio.md
- **ESP32-P4 Function EV Board Guide**: https://github.com/espressif/esp-hosted-mcu/blob/main/docs/esp32_p4_function_ev_board.md
- **ESP32-P4 GPIO Reference**: https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/gpio.html
- **ESP32-C6 GPIO Reference**: https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-reference/peripherals/gpio.html
- **SDMMC Host Driver (ESP32-P4)**: https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/sdmmc_host.html
- **ESP-IDF Kconfig Reference (ESP32-P4)**: https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/kconfig-reference.html

## Required Components

Add these dependencies to your project:

```bash
idf.py add-dependency "espressif/esp_wifi_remote"
idf.py add-dependency "espressif/esp_hosted"
```

Or create `main/idf_component.yml`:

```yaml
dependencies:
  espressif/esp_wifi_remote:
    version: "*"
  espressif/esp_hosted:
    version: "*"
```

## Menuconfig Settings

### ESP-Hosted Config
Path: `Component config → ESP-Hosted config`

- **Transport layer**: SDIO
- **Slave target** (via Wi-Fi Remote): ESP32-C6

### Hosted SDIO Configuration
Path: `Component config → ESP-Hosted config → Hosted SDIO Configuration`

Default GPIO Pin Mapping:

| Signal | GPIO |
|--------|------|
| CLK    | 18   |
| CMD    | 19   |
| D0     | 14   |
| D1     | 15   |
| D2     | 16   |
| D3     | 17   |
| Slave Reset | 54 |

### Wi-Fi Remote Config
Path: `Component config → Wi-Fi Remote`

- **Slave target**: esp32c6
- **WiFi-remote implementation**: ESP-HOSTED

## Troubleshooting

### Issue: MAC Address shows 00:00:00:00:00:00
This indicates the SDIO communication with the ESP32-C6 is not working.

**Solutions:**
1. Verify `esp_wifi_remote` and `esp_hosted` components are installed
2. Check SDIO GPIO pin configuration matches your board
3. Try 1-bit SDIO mode instead of 4-bit (known bug with 4-bit mode)
4. Ensure ESP32-C6 has ESP-Hosted slave firmware flashed

### Issue: "Not support read mac" error
Same as above - the ESP32-P4 cannot communicate with the ESP32-C6 co-processor.

### Issue: Disconnected, retrying...
1. Verify your WiFi network is **2.4GHz** (ESP32-C6 does not support 5GHz)
2. Check SSID and password are correct
3. Verify SDIO communication is working (check for esp_hosted initialization logs)

### Switching to 1-Bit SDIO Mode
In menuconfig: `Component config → ESP-Hosted config → Hosted SDIO Configuration → SDIO Bus Width → 1 Bit`

This is a workaround for a known bug affecting 4-bit SDIO stability.

## Flashing ESP32-C6 Slave Firmware

If the ESP32-C6 co-processor needs to be programmed:

1. Put C6 into download mode by pulling C6_IO9 low during power-on
2. Also put P4 into download mode
3. Flash firmware via C6_U0RXD and C6_U0TXD pins

The ESP-Hosted slave firmware for ESP32-C6 can be built from:
https://github.com/espressif/esp-hosted-mcu

## Sample Code

Basic WiFi station code works with `esp_wifi.h` as normal - the `esp_wifi_remote` component transparently routes calls to the ESP32-C6:

```c
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"

// Standard esp_wifi API calls work transparently
esp_wifi_init(&cfg);
esp_wifi_set_mode(WIFI_MODE_STA);
esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
esp_wifi_start();
esp_wifi_connect();
```

## Important Notes

- ESP32-C6 only supports **2.4GHz WiFi** - 5GHz networks will not work
- The ESP32-P4 recommended ESP-IDF version is **v5.3.1 or newer**
- External pull-up resistors are required on SDIO CMD and DATA lines (usually included on the board)
