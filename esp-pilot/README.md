# STM32 ADC Logger for ESP32-S3

This project turns an `ESP32-S3` into the STM32-side capture endpoint described in `stm_adc_pressure/docs/esp32_spi_link_technical_guide.md`.

It does four things:

1. Runs as SPI master and continuously drains the STM32 `3112-byte` ADC packets.
2. Validates packet headers and `FNV-1a` checksums before accepting data.
3. Records validated raw packets into a SPIFFS storage partition when recording is enabled.
4. Hosts a web page over Wi-Fi AP mode for start/stop recording, file listing, download, and deletion.

## Default behavior

- Wi-Fi AP SSID: `stm32-adc-logger`
- Wi-Fi AP password: `pilot1234`
- Web UI: `http://192.168.4.1`
- SPI mode: `0`
- SPI clock: `6 MHz`
- On-wire SPI transaction size: `3116 bytes`
- Recorded protocol packet size: `3112 bytes`
- Storage backend: internal flash `SPIFFS` partition on `16 MB` flash

## Default ESP32 pin mapping

- `RDY`: `GPIO10`
- `CS`: `GPIO7`
- `SCK`: `GPIO4`
- `MISO`: `GPIO5`
- `MOSI`: `GPIO6`

These defaults are only placeholders. Change them in `idf.py menuconfig` to match your board wiring.

## STM32 Wiring

With the current `stm_adc_pressure` defaults, connect the boards like this:

| ESP32-S3 | STM32F407 | Signal |
| --- | --- | --- |
| `GPIO10` | `PB5` | `RDY` |
| `GPIO7` | `PB12` | `CS/NSS` |
| `GPIO4` | `PB13` | `SCK` |
| `GPIO5` | `PB14` | `MISO` |
| `GPIO6` | `PB15` | `MOSI` |
| `GND` | `GND` | common ground |

Notes:

- The ESP32 acts as SPI master and the STM32 acts as SPI slave.
- Both sides must share `GND` and use `3.3V` logic levels.

## Build

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

## Notes

- The ESP32 keeps reading STM32 packets even when recording is off, so the STM32 link does not stall just because the web recorder is idle.
- Files are stored as raw binary packet streams plus a small `.json` metadata sidecar file.
- The default partition layout now targets `ESP32-S3 + 16 MB flash`, with a `2 MB` factory app partition and about `13.9 MiB` reserved for SPIFFS recording storage.
- STM32 packet headers expose the live per-channel sampling rate in `sample_rate_hz`; on the current STM32 firmware, `KEY1` cycles that value from `100 Hz` to `2000 Hz` in `100 Hz` steps.

## Decode Captures

Use `scripts/adc/decode_adc_capture.py` to turn a downloaded `.bin` capture into:

- a frame-by-frame CSV
- an SVG multi-channel waveform overview
- a JSON summary with packet and gap statistics

Example:

```bash
python3 scripts/adc/decode_adc_capture.py \
  "adc_raw_00000.bin" \
  --metadata "adc_raw_00000.json"
```

Optional:

- `--with-millivolts`: add `*_mv` columns to the CSV and use mV labels in the waveform
- `--no-svg`: export only CSV
- `--no-csv`: export only waveform and summary
