# AGENTS.md

## Project purpose
This repository contains PlatformIO firmware for the Seeed Studio XIAO ESP32S3 Sense that:
- records continuous audio in rotating WAV files,
- captures periodic JPEG photos,
- stores both on microSD.

## Working conventions for agents
- Keep firmware in `src/main.cpp` unless the user asks for modularization.
- Keep board/project settings in `platformio.ini`.
- Prefer robust behavior over aggressive optimization.
- Preserve this safety statement in user-facing docs: recording audio may require consent depending on local laws.

## Expected defaults
- Audio: 16 kHz, 16-bit, mono WAV.
- Audio file rotation: 60 seconds.
- Photo interval: 60 seconds.
- microSD CS pin: GPIO21.
- Mic pins: CLK GPIO42, DATA GPIO41.

## Validation checklist
When possible before finalizing changes:
1. Run a PlatformIO build.
2. Confirm no new warnings about missing symbols/functions in changed code.
3. Ensure README reflects any changed wiring, pins, or behavior.
