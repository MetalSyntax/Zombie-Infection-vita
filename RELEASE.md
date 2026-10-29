# Zombie Infection (PS Vita) — Release v1.1

Release **v1.1** of the native iOS **Mach-O loader** port of *Zombie Infection* (Gameloft) for the PlayStation Vita. This release introduces hardware-accelerated H.264 cutscene video playback, eliminates visual glitches in in-engine cutscenes, overhauls the physical control mappings with dedicated buttons for holstering, jumping, healing and reloading, fixes edge-case crashes, activates triple buffering and shader patcher caching, and provides cross-platform compilation tooling.

---

## 📦 Release Package Contents

1. **`zombieinfection.vpk`** (v01.10): PlayStation Vita application package containing the updated Mach-O loader, hardware video decoder integration, optimized vitaGL renderer, and complete physical input handling.
2. **`rebase.bin`**: Binary relocation table (**61,828** absolute 32-bit memory pointers) for the iOS ARM executable of *Zombie Infection* 1.0.0.

---

## 🚀 Key Improvements in v1.1

### 🎬 Hardware H.264 & AAC Video Player Subsystem
- **Hardware-accelerated cutscenes**: Implemented a complete MP4/ISO-BMFF demuxer paired with the PS Vita hardware video decoder (`SceVideodec` / `sceAvcdec`), rendering directly into vitaGL CDRAM memory textures with 16:9 letterboxing.
- **High-fidelity audio decoding**: Software AAC-LC decoding powered by FFmpeg (`libavcodec`, `swresample`, `libavutil`) streaming on a dedicated thread via `SceAudioOut`.
- **Integrated playback**: Cutscenes (`cinematic_02_lab.m4v`, `cinematic_03_fin.m4v`) trigger seamlessly via `MPMoviePlayerController` and can be skipped at any time by pressing **Cross (✖)**, **Circle (●)**, or **Start**.
- **Bundle path probe**: Automatic detection of video file extensions (`.m4v`, `.mp4`) when queries omit extensions.
- **Debug shortcut**: Press **L + R + Triangle** during gameplay to trigger video playback verification.

### 🎨 Visual Glitch Elimination & vitaGL Optimizations
- **No speedhack regressions**: Cleaned vitaGL compilation flags, removing `DRAW_SPEEDHACK` and `SAMPLERS_SPEEDHACK` which were causing polygon tearing, flickering and distorted textures during in-engine cutscenes.
- **Shader Patcher Cache (`patched_vprog_cache`)**: Added a 64-entry cache in `ffp.c` for GXM patched vertex programs, reusing compiled shaders in 0 ms and avoiding hundreds of redundant `sceGxmShaderPatcherCreateVertexProgram` calls per frame.
- **Compiler Null Safety**: Guarded `shark_compile_shader_extended` against failed allocations, preventing `memcpy(NULL)` crashes and providing automatic fallback to the last valid shader in cache.
- **Expanded Matrix Stack**: Raised `GENERIC_STACK_DEPTH` from 2 to 16 in `vendor/vitaGL/source/shared.h`, preventing matrix stack overflow in complex camera/lighting transformations.
- **Triple Buffering**: Enabled `vglUseTripleBuffering(GL_TRUE)` to eliminate VBlank waiting stalls.
- **Memory thresholds**: Reallocated memory pools with `vglInitWithCustomThreshold` reserving 32 MB RAM heap, 16 MB CDRAM, and 8 MB PHYCONT for smooth level transitions and video buffers.

### 🎮 Dedicated Controls Overhaul
- **D-Pad Down**: Dedicated to **Holster / unDraw weapon** (`{0x2c, 0x3c}` → `PlayerTest::unDrawWeapon` / Back).
- **Triangle (▲)**: Dedicated to **Jump** (`0x64`).
- **D-Pad Left**: Dedicated to **Heal / Use Medikit** (`0x68`).
- **D-Pad Right**: Dedicated to **Reload weapon** (`0x98` / `MultiElement::ReloadBullets`).
- **L Trigger**: Contextual **Dodge** (`0x60`), with automatic fallback to **Reload** (`0x98`) when dodge is inactive.
- **Start / Select**: Clean pause menu and game resume (`CLevel::SetResume`).
- **Select + D-Pad ↑/↓**: Real-time right-stick camera sensitivity adjustment (levels 1–10, saved to disk).
- **L + R (held)**: Virtual on-screen touch controls hidden at 0% opacity by default for an unobstructed screen; hold L+R to toggle visible.

### 🛡️ Crash Fixes & Disk I/O Optimization
- **`PlayerTest::isCarryingVaccineTank` hook**: Intercepted with `macho_hook` to add NULL pointer validation, fixing the crash when toggling pause/resume via Start/Select in menus or opening scenes.
- **Disk I/O latency resolved**: Silenced thousands of missing texture probes (`.tga`, `.bmp`, `.gif`) in `d_fopen` and disabled `fflush` on `DEBUG` lines, eliminating severe stuttering and micro-freezes caused by synchronous memory card writes.

### ⚙️ Multiplatform Build System
- New `build.sh` script with automatic detection of `VITASDK` on macOS (`/Users/...`, `$HOME/vitasdk`) and Linux (`/home/...`).
- Portable multi-core parallel builds (`sysctl -n hw.ncpu` / `nproc`).
- Clean handling of `debug` and `release` build targets.

---

## 🎮 Complete Controls Reference

| PS Vita Input | Action | Behavior / Engine Mapping |
| :--- | :--- | :--- |
| **Front Touchscreen** | Touch UI | Full touchscreen input for menus and virtual buttons |
| **Left Analog Stick** | Movement | Drives game's HUD virtual analog stick (`0x18`) |
| **Right Analog Stick** | Camera look | Continuous touch-drag emulation with custom response curve |
| **Cross (✖) / R1** | Attack / Shoot | Primary attack (`0x74, 0x1c, 0x84, 0x80`) |
| **Square (◼)** | Secondary attack | Melee / secondary attack (`0x20`) |
| **Circle (●)** | Contextual action | Interact, pick up items, open doors (`0x24, 0x28, 0x30, 0x34`) |
| **Triangle (▲)** | Jump | Jump over obstacles (`0x64`) |
| **D-Pad Down (↓)** | **Holster weapon** | Guardar arma (`{0x2c, 0x3c}` → `PlayerTest::unDrawWeapon`) |
| **D-Pad Left (←)** | **Heal (Medikit)** | Usar botiquín de primeros auxilios (`0x68`) |
| **D-Pad Right (→)** | **Reload weapon** | Recargar munición (`0x98`) |
| **D-Pad Up (↑)** | Quick Turn | Giro rápido de 180° (`0x78`) |
| **L Trigger** | Dodge / Reload | Esquivar si hay esquive activo (`0x60`), o recargar arma (`0x98`) |
| **Start / Select** | Pause / Resume | Menú de pausa y reanudar partida |
| **Select + D-Pad ↑/↓** | Camera sensitivity | Ajuste de sensibilidad 1–10 (guardado en `camera_sens.txt`) |
| **L + R (held)** | Toggle HUD visibility | Alternar visibilidad de botones táctiles en pantalla (0% / 100%) |

---

## 🚀 Player Installation Guide

1. **PS Vita Requirements**:
   - Custom Firmware (**Enso 3.60 / 3.65** recommended).
   - `kubridge.skprx` installed under `*KERNEL` in `ur0:tai/config.txt`.
   - `libshacccg.suprx` present in `ur0:data/` (obtainable via **ShaRKBR33D**).

2. **Game Assets Setup**:
   - Extract `Payload/ZombieInfection.app/` from your decrypted iOS 1.0.0 `.ipa`.
   - Copy it to `ux0:data/zombieinfection/ZombieInfection.app/`.
   - Copy `rebase.bin` to `ux0:data/zombieinfection/rebase.bin`.
   - Required directory layout:
     ```text
     ux0:data/zombieinfection/
     ├── ZombieInfection.app/
     │   ├── ZombieInfection (original iOS binary)
     │   ├── data.pack, audio_*.pack, gll_font.pak, ...
     │   └── *.png, *.plist, cinematic_*.m4v, ...
     ├── rebase.bin
     ├── saves/ (created automatically on first launch)
     └── logs/  (created automatically, one log per run)
     ```

3. **Install the VPK**: Transfer and install `zombieinfection.vpk` (v01.10) using VitaShell.

---

## 📋 Changelog History

### v1.1.0 (2026-10-29)
- **Cutscene Video Player**: Added hardware H.264 video decoding (`SceVideodec`/`sceAvcdec`) and software AAC-LC decoding (`libavcodec`). Cutscenes `.m4v` play smoothly with audio and skip support.
- **Rendering & Cutscene Artifact Fix**: Removed `DRAW_SPEEDHACK` and `SAMPLERS_SPEEDHACK`, increased `GENERIC_STACK_DEPTH` to 16, and implemented 64-entry `patched_vprog_cache` in vitaGL.
- **Controls Refinement**: Reassigned D-Pad Down to weapon holstering, Triangle to Jump, D-Pad Left to Medikit heal, D-Pad Right to Reload, and L Trigger to contextual Dodge/Reload.
- **Crash Prevention**: Hooked `PlayerTest::isCarryingVaccineTank` with null safety.
- **Disk I/O Optimization**: Removed excessive logging in `d_fopen` and disabled `fflush` on debug lines.
- **Build System**: Added multiplatform `build.sh` supporting macOS and Linux. Bumped SFO version to `01.10`.

### v1.0.0-beta (2026-10-19)
- Initial native iOS Mach-O loader port reaching gameplay.
- 61,828 rebase words applied at `0x98000000`, 7,358 import slots resolved, 366 global constructors executed.
- GLES 1.x over vitaGL with `SOFTFP_ABI=1`, 960×544 presentation.
- OpenAL audio via openal-soft, FreeType font rendering, and analog stick camera look emulation.
