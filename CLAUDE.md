# Zombie Infection (iOS Mach-O) → PS Vita

Port de Zombie Infection (iOS Mach-O) a PS Vita con loader de Mach-O propio.
Leer `PORTING_PLAN.md` antes de modificar el loader.

## Estructura

- `source/macho.c`: Carga, parseo Mach-O, mapeo de segmentos, rebase y resolución de imports.
- `source/darwin.c`: Capa de compatibilidad libc/libgcc/pthread/semáforos de Darwin.
- `source/objc.c` / `objc_rt.h`: Runtime ObjC (despacho a clases del juego + clases host) y Foundation.
- `source/uikit.c`: UIApplicationMain (nib emulado), run loop, NSTimer, EAGL, UIView, toques.
- `source/cxx.c`: libstdc++ del juego → libstdc++ del VitaSDK.
- `game_app` → symlink a la carpeta `.app` original del IPA.
- `ux0_data/zombieinfection/`: Datos extraídos y assets del juego en la consola.
- `PORTING_PLAN.md`: Plan de port y estado actual de fases.
- `port_progress.md`: Registro de progreso.

## Reglas

- ABI iOS ARM: softfp en ARMv6/v7 (floats en registros enteros), r9 temporal (preservar en callbacks).
- Nada del IPA original en el repositorio git (.gitignore anti-DMCA).
- Un bug por corrida de consola física, guiado por logs.
