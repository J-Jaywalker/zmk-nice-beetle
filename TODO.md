# 🐞 zmk-nice!beetle TODO

A living and ever expanding list of improvements and ideas to improve Beedly.

---

## ✅ Core Tasks

- [X] Add Python-based workflow to automatically convert `.png` images into LVGL-compatible `.c` arrays  
  - [X] Integrate with a `Makefile`
  - [X] Output to same folder with `.gitignore` support
  - [X] Add requirements file for Python dependencies
- [X] Port the widgets to LVGL 9 / ZMK `main` (Zephyr 4.1)
  - [X] `make beedly` writes the firmware frame file (`beedly_frames.c`) directly, no copy-paste
- [ ] Verify the LVGL 9 port on hardware (layout, rotation, Luna animations, inverted colours)

---

## 🌠 Stretch Goals

- [ ] Replace current heart animation with an energy-style indicator  
  - [ ] Tie energy level to ZMK's shield battery percentage
  - [ ] Adjust animation speed or brightness based on charge level

- [ ] Add interactive Tamagotchi-like behavior to Beedly  
  - [X] `&beedly` behaviour: PET / FEED / SAVE menu on the left screen (BEEDLY layer), actions sent to the right half over the split link
  - [X] Hearts and food bars, XP and level on the right screen (over the art's mock HUD); PET hearts and bar-flash animations
  - [X] Lazy hunger / happiness decay from uptime (no timers)
  - [X] Store "stats" in persistent memory (right half, `beedly/pet` settings record)
  - [X] Simulator scenario (`make sim-test-pet`) - written, not yet run
  - [ ] Verify on hardware (menu, split delivery, saves across power cycles, deep-sleep save)
  - [ ] Redraw the art's HUD: the third bar shows XP under a "Zz" icon (there is no sleep stat)
  - [ ] SAVE closes the menu even if the send to the right half failed or was dropped (known issue, `behavior_beedly.c`). A BLE write-without-response is never acknowledged, so closing only on a failed send would not catch every case; the right half still saves on its own timer, on idle and before sleep
  - [ ] Combos without a `layers` list also fire on the BEEDLY layer (alphred_kb keymap); scope them to the typing layers
  - [ ] Track typing activity to simulate "feeding"
  - [ ] Idle timer to trigger "boredom" or "sleep"
  - [ ] Basic mood states (happy, hungry, tired, annoyed)

- [X] Beedly to replace Luna animations on the LHS (central) screen (`beedly_left.aseprite`, `make beedly_left`)

---

## 💡 Ideas (Backlog)

- [ ] Include `beedly.conf` for customization (thresholds, modes, etc.) once core behaviour is fleshed out
- [ ] Add more minigames and behaviours for Beedly
- [ ] Beedly for president.
---