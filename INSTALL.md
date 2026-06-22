# Installation Guide

## Prerequisites

- **ESP-IDF v5.1+** — [installation guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/)
- **ESP32-S3-N16R8** board (16 MB Flash, 8 MB PSRAM required for spectrum buffer)
- **USB-C OTG cable** (host-side) for connecting to Atom Spectra
- KB Radar **Atom Spectra** gamma spectrometer

## Hardware Setup

The ESP32-S3 USB OTG Host connects to the spectrometer's USB port:

```
[ESP32-S3 USB OTG] ---USB-C cable---> [Atom Spectra USB port]
```

**Important:** Use the ESP32-S3 native USB port (GPIO19/20), not the UART/programming port. The S3-N16R8 DevKitC-1 has two USB-C ports — use the one labeled "USB" (not "UART").

## Build

```bash
# Clone
git clone https://github.com/VibeEngineering-LLC/atomspectra-esp32.git
cd atomspectra-esp32

# Set target
idf.py set-target esp32s3

# Build
idf.py build
```

### Build Dependencies

These are fetched automatically by ESP-IDF component manager (`idf_component.yml`):

- `espressif/usb_host_cdc_acm` — USB Host CDC-ACM driver
- `joltwallet/littlefs` — LittleFS filesystem

## Flash

```bash
# Flash via UART port (programming port)
idf.py -p COMx flash

# Monitor boot log
idf.py -p COMx monitor
```

Replace `COMx` with your serial port (check Device Manager for CH340/CP2102/FTDI).

## First Boot — WiFi Setup

1. Device starts in AP mode: **"AtomSpectra-Setup"** (open, no password)
2. Connect phone/laptop to this AP
3. Browser opens captive portal automatically (or navigate to `http://192.168.4.1/`)
4. Select your WiFi network from scan results
5. Enter password, click **Connect**
6. Device reboots and connects to your WiFi in STA mode
7. Find device IP in router DHCP table or serial monitor output

## Usage

1. Open `http://<device-ip>/` in browser
2. Connect Atom Spectra to ESP32-S3 USB OTG port
3. Spectrum appears automatically (updates every second)
4. Use buttons:
   - **Reset** — clear spectrum counters on device
   - **Save** — save current spectrum to flash
   - **Export XML** — download BecqMoni-compatible file
   - **Export CSV** — download InterSpec-compatible file
   - **Info** — request device info (sends `-inf` command)
   - **WiFi Reset** — clear WiFi config, reboot to setup AP

## Changing WiFi

Two methods:
1. **From Web UI:** Click the orange **WiFi Reset** button → device reboots into AP mode
2. **Via serial:** Erase NVS: `idf.py -p COMx erase-otadata` then reflash

## Partition Table

```
nvs,      data, nvs,     0x9000,   0x6000
phy_init, data, phy,     0xf000,   0x1000
factory,  app,  factory, 0x10000,  3M
storage,  data, littlefs,,         12944K
```

- **3 MB** for firmware
- **12.9 MB** LittleFS for saved spectra (~400 spectra at 32 KB each)

## Troubleshooting

### USB device not detected
- Check USB cable supports data (not charge-only)
- Verify correct USB port on ESP32-S3 (OTG, not UART)
- Check serial monitor for `USB device connected` message
- CH343 VID:PID should be `1A86:55D3`

### No spectrum data
- Send `-inf` command via Web UI to request device info
- Check serial monitor for shproto packet activity
- Verify baud rate 600000

### WiFi won't connect
- Verify SSID/password via captive portal
- Check 2.4 GHz band (ESP32 doesn't support 5 GHz)
- Use WiFi Reset button to reconfigure
