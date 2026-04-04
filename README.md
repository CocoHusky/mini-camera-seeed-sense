# XIAO ESP32S3 Sense Continuous Audio + Periodic Photo Logger

This PlatformIO project targets the **Seeed Studio XIAO ESP32S3 Sense** and implements:
- continuous audio capture from the onboard PDM microphone,
- WAV chunk rotation every 60 seconds,
- one JPEG photo capture every 60 seconds,
- storage to microSD on the Sense expansion board.

> ⚠️ Important: continuous audio recording keeps the MCU active. You cannot run true continuous audio capture and deep sleep at the same time.

## Features

- **Board/framework**: `seeed_xiao_esp32s3` + Arduino (PlatformIO)
- **Audio**: 16 kHz, 16-bit, mono WAV (`/audio/*.wav`)
- **Photo**: JPEG capture (`/photo/*.jpg`)
- **Reliability**:
  - frequent file flushes for lower data-loss risk on sudden power loss,
  - file rotation every 60s,
  - retry handling for SD availability,
  - independent audio/photo tasks so one subsystem can continue if the other errors.
- **Power-minded behavior**:
  - Wi-Fi and Bluetooth disabled,
  - CPU clock reduced to a practical level,
  - camera initialized only during capture.

## Hardware required

- Seeed Studio XIAO ESP32S3 Sense
- Sense expansion board (camera + microSD)
- microSD card (FAT32)
- USB-C cable
- Optional battery pack for portable use

## Pin assumptions in this firmware

- microSD CS: **GPIO21**
- PDM mic clock: **GPIO42**
- PDM mic data: **GPIO41**
- Camera pins: configured for XIAO ESP32S3 Sense module in `src/main.cpp`

## Project structure

- `platformio.ini` – PlatformIO environment and dependencies
- `src/main.cpp` – firmware implementation (audio task + photo task + WAV helpers)
- `AGENTS.md` – agent-oriented project guidance

## Install and build

1. Install [PlatformIO](https://platformio.org/) (VS Code extension or CLI).
2. Clone/copy this project.
3. From project root, build:

```bash
pio run
```

4. Flash device:

```bash
pio run -t upload
```

5. Open serial monitor:

```bash
pio device monitor -b 115200
```

## Runtime behavior

- On boot, firmware attempts to mount SD and create:
  - `/audio`
  - `/photo`
- Audio task starts continuous sampling from PDM mic and writes WAV data.
- WAV files rotate every 60 seconds.
- Photo task captures one JPEG every 60 seconds.
- If RTC time is unavailable, filenames use a monotonic fallback format.

Example output layout:

```text
/audio/20260404_120001.wav
/audio/20260404_120101.wav
/photo/20260404_120100.jpg
/photo/20260404_120200.jpg
```

Fallback naming (no RTC time):

```text
/audio/M123456_1.wav
/photo/M184021_2.jpg
```

## Notes on transcription strategy

For this hardware class, a practical architecture is:
- **device** handles robust local capture/storage,
- **phone/PC/cloud** handles heavier speech-to-text.

This keeps firmware simpler and improves reliability for long recordings.

## Troubleshooting

- **SD mount errors**
  - Re-seat card, confirm FAT32 formatting, test another card.
- **Camera init fails intermittently**
  - Ensure stable 5V USB power, avoid weak USB ports/cables.
- **Unexpected resets or corrupted files**
  - Use quality power source and keep periodic flush enabled.
- **Large battery drain**
  - Expected with continuous audio + periodic camera use; size battery accordingly.

## Legal and privacy reminder

If you wear or carry this as a personal recorder, make sure you comply with local recording laws and consent expectations in your area.
