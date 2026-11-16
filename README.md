# NONOXP 3DS (AI5WIN engine port) — work in progress

## Build
Push this folder to a GitHub repository; the `build-3ds` workflow produces `nonoxp.3dsx` as an artifact.
Local PC check build: `gcc -O1 -w -Isrc -o ai5_headless src/*.c platform/headless/plat_headless.c`

## Run on a 3DS
1. Copy `nonoxp.3dsx` to `sdmc:/3ds/nonoxp/`.
2. Copy the game's `MES.ARC DATA.ARC BG.ARC MUSIC.ARC VOICE.ARC` (from your own disc) and `assets/font16.bin` to the same folder.
3. Start it from the Homebrew Launcher.

Controls: A / touch = confirm, B = cancel, D-pad = move, R (hold) = skip, START+SELECT = quit.
Problems are written to `sdmc:/3ds/nonoxp/log.txt`.

## State
No sound, no save/load, no backlog yet. Full-screen menus (save, settings) are not adapted to the small screen.
See docs/FORMATS.md for the reverse-engineering notes and what is verified vs. assumed.
