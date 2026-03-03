# Katux — A tiny system for serious (and slightly mischievous) tinkerers

Welcome to Katux: a small OS/GUI project aimed at microcontrollers and embedded displays. If you enjoy putting things together, watching pixels move, and telling people "it's for a project," you're in the right place.

This README was written by a real human who drinks too much coffee and refuses to believe an AI wrote it.

**Main features**
- Basic graphical interface (compositor, windows, cursor)
- Simple application handling (demos, settings, local browser)
- Input management (buttons, mouse emulation)
- Minimal kernel and scheduler to keep tasks behaving
- Support for hobby display drivers and board adapters (see `boards/` and `lib/`)

**Why Katux?**

Sometimes you want more than a blinking LED but less than a NASA-grade OS. Katux aims to be a readable, approachable base for experimenting with a graphical interface on microcontrollers.

**Essential tree**
- `Katux.ino`: Arduino-style entry point.
- `src/`: main source code.
  - `core/`: kernel, scheduler, event manager.
  - `graphics/`: compositor, renderer, window manager, themes.
  - `apps/`: example apps (demo, settings, ...).
  - `input/`: button handling and mouse emulation.
  - `system/`: BIOS, boot sequence, power management, soft keyboard.

**Prerequisites & building**

Open the project with the Arduino IDE or use PlatformIO inside VS Code if you prefer staying in that comfortable little bubble:

1. Connect a compatible board (e.g. an ESP-based board or another supported MCU).
2. Open `Katux.ino` in the Arduino IDE and select the matching board and port.
3. Click "Upload".

If you use PlatformIO, configure or choose an environment in `platformio.ini` (this workspace contains examples and helper scripts in other folders).

Note: exact settings (display resolution, pinout, drivers) depend on your hardware — check `boards/` and `lib/` for adapters and examples.

**Contributing**

Contributions are the nicest compliments a project can get. To contribute:
- Open an issue to discuss larger changes before coding.
- Send focused, well-tested pull requests (hardware-tested if possible).

Good practices: document hardware wiring, include a short test procedure, and avoid vague commit messages like "fixed stuff".

**License**

This repo may not include a dedicated `LICENSE` file for Katux. If you plan to reuse code publicly, add or verify the appropriate license first.

**Warnings**

- This project is intended for makers and hobbyists — expect to tinker.
- Performance and stability depend heavily on the chosen hardware.

If you read this and think "nice," mission accomplished. If you think "this needs work," even better — open an issue or a PR and let's improve it together.

— The human team (yes, really)
