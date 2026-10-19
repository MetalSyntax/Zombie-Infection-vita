# Zombie Infection (PS Vita) — Beta Release v1.0

First public **beta** of the native iOS **Mach-O loader** port of *Zombie Infection* (Gameloft) for the PlayStation Vita. The game boots, initializes its Irrlicht engine, reaches the in-engine intro and the tutorial/gameplay on real hardware.

---

## 📦 Release Package Contents

1. **`zombieinfection.vpk`**: PS Vita application package containing the native Mach-O loader, hardware-accelerated OpenGL ES via **vitaGL**, FreeType text rendering, and physical/touch controls integration.
2. **`rebase.bin`**: Binary relocation table (**61,828** absolute 32-bit memory pointers) generated specifically for the iOS ARM executable of *Zombie Infection* (version 1.0.0).

---

## 🛠️ What is `rebase.bin`?

The iOS executable is a non-PIE (`MH_EXECUTE`) image hard-linked to a low virtual base address, and App Store builds ship with `nlocrel = 0` (no local relocation records). The loader maps the image at `MACHO_SLIDE = 0x98000000`, so every absolute pointer in the data segments — plus the `-mdynamic-no-pic` literal pools and switch tables baked into `__text` — must be shifted by that constant.

`rebase.bin` is the compact lookup table telling the loader exactly which words to slide at startup. It is generated with the included [`tools/macho_prep.py`](tools/macho_prep.py):

```bash
python3 -I tools/macho_prep.py ux0_data/zombieinfection/ZombieInfection.app/ZombieInfection \
    ux0_data/zombieinfection/rebase.bin --report docs/macho_report.txt
```

(or use the automated `tools/stage_data.sh` pipeline).

File layout:
- `0x00` — magic `'RBS1'`
- `0x04` — word count (61,828)
- `0x08` — array of 32-bit `vmaddr` values to relocate

---

## 🚀 Player Installation Guide

1. **PS Vita Requirements**:
   - Custom Firmware (Enso 3.60 / 3.65 recommended).
   - `kubridge.skprx` active under `*KERNEL` in `ur0:tai/config.txt`.
   - `libshacccg.suprx` present in `ur0:data/` (obtainable via ShaRKBR33D).

2. **Game Assets Setup**:
   - Extract `Payload/ZombieInfection.app/` from your decrypted iOS 1.0.0 `.ipa`.
   - Copy it to `ux0:data/zombieinfection/ZombieInfection.app/`.
   - Copy `rebase.bin` to `ux0:data/zombieinfection/rebase.bin`.
   - Required layout:
     ```text
     ux0:data/zombieinfection/
     ├── ZombieInfection.app/
     │   ├── ZombieInfection (original iOS binary)
     │   ├── data.pack, audio_*.pack, gll_font.pak, ...
     │   └── *.png, *.plist, ...
     ├── rebase.bin
     ├── saves/ (created automatically on first launch)
     └── logs/  (created automatically, one log per run)
     ```

3. **Install the VPK**: transfer and install `zombieinfection.vpk` using VitaShell.

---

## 🎮 Controls

| PS Vita Input | Action | Status |
| :--- | :--- | :---: |
| **Front Touchscreen** | Full touch interface | ✅ Working |
| **Left Stick** | Movement (virtual analog stick) | ✅ Working |
| **Right Stick** | Camera look (touch-drag emulation) | ✅ Working |
| **Cross (✖) / R** | Attack | ✅ Working |
| **Square (◼)** | Secondary attack | ✅ Working |
| **Circle (●)** | Use / contextual action | ✅ Working |
| **Triangle (▲)** | Jump | ✅ Working |
| **D-Pad Up** | Rotate | ✅ Working |
| **D-Pad Down** | Holster / draw weapon | ✅ Working |
| **L Trigger** | Dodge | ✅ Working |
| **Start** | Back / Pause | ✅ Working |
| **L + R (held)** | Show / hide the on-screen touch controls | ✅ Working |
| **Select + D-Pad ↑/↓** | Camera sensitivity (1–10, saved) | ✅ Working |

---

## 📋 Changelog (v1.0-beta)

- **Loader**: fixed-base mapping via `kuKernelAllocMemBlock`; correct rebase of `-mdynamic-no-pic` literal pools and switch tables in `__text` (61,828 words); 366 global constructors run; 7,358 import slots resolved.
- **Runtime**: real Objective-C 2.0 dispatch into the game's classes plus host Foundation/UIKit; emulated `MainWindow.nib`, run loop, `NSTimer` and `EAGLContext`/`CAEAGLLayer`.
- **I/O**: emulated working directory (`chdir`/`getcwd`) so Irrlicht resolves relative asset paths from the bundle; `Documents/` redirected to `saves/`.
- **Graphics**: GLES 1.x → vitaGL with `SOFTFP_ABI=1`; full 960×544 presentation without stretching (3D gains FOV, HUD centered); per-array vertex/UV/color conversion and a corrected framebuffer depth-buffer re-attach to fix cutscene artifacts; native CgBI PNG decoding.
- **Audio**: OpenAL mapped to openal-soft (`alBufferDataStatic` emulated); AudioSession stubbed.
- **Input**: physical buttons and sticks drive the game's runtime HUD controls; right-stick camera model with dead zone, curve and selectable sensitivity.
- **LiveArea**: generated from the game's own icon and splash screen.

## ⚠️ Known Issues (beta)

- `.m4v` cutscenes are skipped.
- Occasional distorted geometry/textures during in-engine cutscenes (under investigation).
- Online/store and anti-piracy checks are bypassed by design.
