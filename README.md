# ESP32-P4 WiFi Project

A WiFi-enabled web server project for the **Waveshare ESP32-P4-WIFI6-M** development board using ESP-IDF.

## Features

- WiFi connectivity via ESP32-C6 co-processor (ESP-Hosted)
- Simple HTTP server with web interface
- JSON API endpoint

## Hardware Requirements

- **Board:** [Waveshare ESP32-P4-WIFI6-M](https://www.waveshare.com/wiki/ESP32-P4-WIFI6)
- **USB Cable:** USB-C for programming and power
- **WiFi Network:** 2.4GHz only (5GHz not supported by ESP32-C6)

## Software Requirements

- **ESP-IDF:** v5.3.1 or newer (v5.5.2 recommended)
- **Python:** 3.8+
- **Git**

---

## Setup Instructions

### 1. Install ESP-IDF

#### Windows

1. Download the [ESP-IDF Tools Installer](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/windows-setup.html)
2. Run the installer and select ESP-IDF v5.5.x
3. The installer will set up Python, Git, and all required tools

#### Linux/macOS

```bash
mkdir -p ~/esp
cd ~/esp
git clone -b v5.5.2 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32p4
source export.sh
```

### 2. Clone This Repository

```bash
git clone https://github.com/nalkema1/espidf-wifi.git
cd espidf-wifi
```

### 3. Activate ESP-IDF Environment

#### Windows
- Open **"ESP-IDF 5.5 CMD"** from the Start Menu
- Or run: `C:\Espressif\frameworks\esp-idf-v5.5.2\export.bat`

#### Linux/macOS
```bash
source ~/esp/esp-idf/export.sh
```

### 4. Set Target Chip

```bash
idf.py set-target esp32p4
```

### 5. Configure WiFi Credentials

```bash
idf.py menuconfig
```

Navigate to:
- **WiFi Configuration**
  - Set **WiFi SSID** to your network name
  - Set **WiFi Password** to your network password

> **Important:** Use a 2.4GHz network. The ESP32-C6 does not support 5GHz.

Press `S` to save, then `Q` to quit.

### 6. Build the Project

```bash
idf.py build
```

### 7. Flash to the Board

Connect the board via USB, then:

```bash
idf.py -p COMX flash monitor
```

Replace `COMX` with your serial port:
- **Windows:** `COM3`, `COM4`, etc. (check Device Manager)
- **Linux:** `/dev/ttyUSB0` or `/dev/ttyACM0`
- **macOS:** `/dev/cu.usbserial-*`

### 8. Access the Web Server

Once connected, the serial monitor will display:
```
I (xxxx) P4_WIFI: Got IP: 192.168.x.x
I (xxxx) HTTP_SERVER: Starting HTTP server on port 80
```

Open a browser and navigate to `http://<IP_ADDRESS>/`

---

## Project Structure

```
espidf-wifi/
├── CMakeLists.txt          # Top-level CMake configuration
├── main/
│   ├── CMakeLists.txt      # Main component CMake
│   ├── main.c              # Application entry point, WiFi init
│   ├── http_server.c       # HTTP server implementation
│   ├── http_server.h       # HTTP server header
│   ├── Kconfig.projbuild   # Menuconfig options (WiFi credentials)
│   └── idf_component.yml   # Component dependencies
├── README.md               # This file
└── WIFI_SETUP_GUIDE.md     # Detailed WiFi troubleshooting guide
```

---

## API Endpoints

| Endpoint       | Method | Description                    |
|----------------|--------|--------------------------------|
| `/`            | GET    | HTML status page               |
| `/api/status`  | GET    | JSON status: `{"status":"ok"}` |

---

## Menuconfig Options

### WiFi Configuration
Path: `WiFi Configuration`
- **WiFi SSID:** Your network name
- **WiFi Password:** Your network password

### ESP-Hosted Configuration
Path: `Component config → ESP-Hosted config`
- **Transport layer:** SDIO (default)
- **SDIO Bus Width:** 4-bit (or 1-bit if experiencing issues)

### WiFi Remote Configuration
Path: `Component config → Wi-Fi Remote`
- **Slave target:** esp32c6

---

## Troubleshooting

### MAC Address shows 00:00:00:00:00:00
The ESP-Hosted components are not properly configured. Ensure:
1. `esp_wifi_remote` and `esp_hosted` are in `idf_component.yml`
2. Run `idf.py fullclean` then rebuild

### "Version mismatch: Host > Co-proc"
The ESP32-C6 co-processor firmware is outdated. This warning can be ignored for basic functionality, but for best stability, update the C6 firmware. See [WIFI_SETUP_GUIDE.md](WIFI_SETUP_GUIDE.md).

### WiFi keeps disconnecting
1. Verify your network is 2.4GHz (not 5GHz)
2. Check SSID and password are correct
3. Try setting SDIO to 1-bit mode in menuconfig

### Build fails with missing components
Run:
```bash
idf.py fullclean
idf.py set-target esp32p4
idf.py build
```

---

## Useful Commands

| Command                     | Description                          |
|-----------------------------|--------------------------------------|
| `idf.py build`              | Build the project                    |
| `idf.py flash`              | Flash to the board                   |
| `idf.py monitor`            | Open serial monitor                  |
| `idf.py flash monitor`      | Flash and open monitor               |
| `idf.py menuconfig`         | Open configuration menu              |
| `idf.py fullclean`          | Clean all build artifacts            |
| `idf.py set-target esp32p4` | Set the target chip                  |

---

## References

- [Waveshare ESP32-P4-WIFI6 Wiki](https://www.waveshare.com/wiki/ESP32-P4-WIFI6)
- [ESP-IDF Programming Guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/)
- [ESP-Hosted GitHub](https://github.com/espressif/esp-hosted-mcu)
- [ESP32-P4 Datasheet](https://www.espressif.com/en/products/socs/esp32-p4)

---

## License

MIT License - feel free to use this code for your projects.
