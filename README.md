# AtomSpectra ESP32 Gateway

WiFi gateway for **KB Radar "Atom Spectra"** gamma spectrometer, built on ESP32-S3 with USB OTG Host.

Connects to the spectrometer via USB, acquires 8192-channel spectra in real time, and serves a web UI for visualization and export.

## Features

- **USB Host CDC** connection to Atom Spectra via CH343 USB-UART bridge
- **shproto binary protocol** (CRC-16 Modbus, escaped framing)
- **8192-channel spectrum** live display in browser
- **BecqMoni XML export** (FormatVersion 120920) — import directly into [BecqMoni](https://github.com/Am6er/BecqMoni)
- **InterSpec CSV export** — compatible with [InterSpec](https://sandialabs.github.io/InterSpec/)
- **Energy calibration** parsed from device (polynomial up to order 4)
- **Spectrum storage** on LittleFS (12.9 MB partition)
- **TCP bridge** on port 8234 — transparent serial-over-WiFi for BecqMoni/AtomSpectra PC software
- **WiFi captive portal** for initial network setup
- **SNTP** time synchronization for export timestamps
- **Web UI** with live spectrum chart, device stats, text command interface

## Hardware

| Component | Specification |
|---|---|
| MCU | ESP32-S3-N16R8 (16 MB Flash, 8 MB PSRAM) |
| USB | OTG Host mode, GPIO19/20 (Full-Speed 12 Mbps) |
| Spectrometer | KB Radar Atom Spectra (CH343 VID 0x1A86, PID 0x55D3) |
| Connection | USB-C OTG cable (host) to spectrometer USB port |

## Quick Start

1. Install [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/) v5.1+
2. Clone this repository
3. Build and flash:
   ```bash
   idf.py set-target esp32s3
   idf.py build
   idf.py -p COMx flash
   ```
4. Connect to WiFi AP **"AtomSpectra-Setup"**, enter your network credentials
5. Open `http://<device-ip>/` in browser
6. Connect spectrometer via USB-C OTG cable

## Export Formats

### BecqMoni XML (`/api/export.xml`)

Full spectrum with energy calibration, measurement times, pulse counts. Import via BecqMoni File > Open.

### InterSpec CSV (`/api/export.csv`)

Calibration coefficients header, metadata, 8192 channel counts. Open in InterSpec as CSV spectrum.

## Web API

| Endpoint | Method | Description |
|---|---|---|
| `/` | GET | Web UI |
| `/api/status` | GET | Device status JSON |
| `/api/spectrum.json` | GET | Live spectrum + stats |
| `/api/spectrum` | GET | Raw binary (32768 bytes) |
| `/api/export.xml` | GET | BecqMoni XML download |
| `/api/export.csv` | GET | InterSpec CSV download |
| `/api/command` | POST | Send text command to device |
| `/api/reset` | POST | Reset spectrum counters |
| `/api/save` | POST | Save spectrum to flash |
| `/api/list` | GET | List saved spectra |
| `/api/wifi/reset` | POST | Clear WiFi, reboot to setup |

## Project Structure

```
atomspectra/
  components/shproto/     shproto protocol (CRC-16 Modbus)
  main/
    atomspectra.h          project header, data types
    main.c                 entry point, SNTP
    usb_host_cdc.c         USB Host CDC-ACM driver
    wifi_manager.c         STA + AP captive portal
    web_server.c           HTTP API + exports
    tcp_bridge.c           transparent serial-over-WiFi bridge
    spectrum.c             spectrum processing + LittleFS
  web/
    index.html             main UI
    setup.html             WiFi captive portal page
  partitions.csv           custom partition table (LittleFS)
  sdkconfig.defaults       ESP32-S3 USB OTG config
```

## TCP Bridge (Port 8234)

Transparent serial-over-WiFi bridge. Connect BecqMoni or AtomSpectra PC software to `<device-ip>:8234` instead of a COM port. The bridge forwards raw bytes bidirectionally between the TCP client and the USB-connected spectrometer.

- One client at a time
- Data is also processed locally (Web UI works simultaneously)
- TCP_NODELAY enabled for low latency

## Protocol

The Atom Spectra communicates via **shproto** binary protocol over USB serial at 600000 baud:

- Framing: `START=0xFE`, `ESC=0xFD`, `FINISH=0xA5`
- CRC: CRC-16 Modbus (init 0xFFFF, poly 0xA001)
- Escape: `(~byte) & 0xFF`
- Commands: `0x01` histogram, `0x04` statistics, `0x03` text I/O

## License

Private repository. All rights reserved.
