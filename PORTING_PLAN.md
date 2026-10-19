# Zombie Infection (iOS) → PS Vita

Port de Zombie Infection (iOS, Mach-O arm) a PS Vita con loader de Mach-O propio.
Basado en la arquitectura probada de `ios-loader-boilerplate` (caso de estudio Inotia2-vita).

## Lo confirmado del binario (2026-10-18)

- Ejecutable: `ZombieInfection`, thin/fat **arm**.
- Cifrado FairPlay: **cryptid 0** (descifrado, listo para ejecución).
- Tamaño: 9706 KB.
- Bundle ID: `com.gameloft.ZombieInfection`.
- Paquete original: `Zombie Infection 1.0.0.ipa`.
- App bundle: `game_app` → `/Volumes/TOSHIBA EXT/PSVITA Develop/Zombie-Infection-vita/zombieinfection_extract/Payload/ZombieInfection.app`.
- Palabras de rebase (`rebase.bin`): **55401**.
- Imports únicos detectados: **1098** (GL: 614, Audio: 157, ObjC/CF: 411, POSIX/libc: 15).

## Reglas Críticas de ABI de iOS en PS Vita

1. **`r9` temporal**: En ARMv6/v7 iOS, `r9` es usado como temporal por el compilador y no se preserva. Compilar con `-ffixed-r9` y usar `call_guest` al llamar al juego.
2. **Convención SoftFP**: Floats en registros enteros (`r0..r3`). `vitaGL` vendorizada con `SOFTFP_ABI=1` obligatoria para no corromper la proyección en libgxm.
3. **Alineación de pila a 4 bytes**: El juego asume pila alineada a 4 bytes. Todo import/hook llamado desde el binario entra por `ml_align_call` (asm.S), que realinea a 8 bytes AAPCS copiando argumentos de stack.
4. **printf de Darwin**: Enteros y dobles de 64 bits viajan en palabras de 32 bits contiguas sin alinear; soportar `%@` de Objective-C (`darwin_printf.c`).

## Estructura en la Vita (`ux0:data/zombieinfection/`)

```
ux0:data/zombieinfection/
  ZombieInfection.app/   bundle del IPA limpio (ejecutable + recursos)
  rebase.bin         punteros absolutos reubicados (MACHO_SLIDE = 0x98000000)
  saves/             partidas guardadas (Documents/ de iOS)
  logs/              log_001.log .. log_999.log rotativos
```

## Arquitectura del Loader

| Archivo | Qué reemplaza / funcionalidad |
|---|---|
| `source/macho.c` / `asm.S` | Dyld Mach-O: carga, rebase, imports, thunks con trace, `call_guest`, `ml_align_call` |
| `source/darwin.c` | libc/libgcc/pthread/semáforos de Darwin con `pthread_detach` seguro contra SceUIDs |
| `source/darwin_printf.c` | Formateador variádico con soporte de dobles desalineados y `%@` |
| `source/objc.c` / `objc_rt.h` | Runtime ObjC 2.0 real: despacho a métodos del juego (class_ro_t) + clases host, caché (clase, SEL), Foundation (NSString/Array/Dictionary/Data/FileManager/FileHandle/Bundle/NotificationCenter...), `UIFont` FreeType |
| `source/uikit.c` | `UIApplicationMain` + MainWindow.nib emulado, run loop, NSTimer, EAGLContext/CAEAGLLayer, UIView, UITouch, UIAccelerometer, UIAlertView, MediaPlayer (cinemáticas saltadas) |
| `source/cxx.c` | libstdc++/libsupc++ del juego → libstdc++ del VitaSDK (ABI COW vieja, `m`→`j` en el mangling de size_t) |
| `source/gles.c` | Backend GLES1/2 sobre vitaGL vendorizada con FBO virtual escalado/rotado |
| `source/cgbi.c` | Soporte transparente para imágenes PNG CgBI de Apple |
| `source/coreaudio.c` | AudioSession (stubs) + OpenAL → openal-soft del VitaSDK (`alBufferDataStatic` emulado) |
| `source/input.c` | Touch frontal + botones físicos mapeados a la capa táctil |
| `source/patch.c` | Parches y hooks específicos de Zombie Infection |

## Fases

Leyenda: `[x]` hecho y verificado · `[~]` implementado y compilado, falta verificar en consola · `[ ]` pendiente.
Los GATE solo se marcan con una corrida en consola física (CLAUDE.md: un bug por corrida, guiado por logs).

- [x] **0. Repo y herramientas**: IPA extraído, binario analizado, `game_app` symlink, scaffolding creado. `annotate.py` soporta modo ARM (este binario es ARMv6 en modo ARM).
- [x] **1. Loader mínimo** — GATE ✅ en consola (log_003): `UIApplicationMain` → `applicationDidFinishLaunching:` → NSTimer `drawView:` → `Application::Init` de Irrlicht.
  - [~] Carga de segmentos en memoria con `macho_load` (slide `0x98000000`): bloques en dirección fija con `kuKernelAllocMemBlock` (lógica probada de Inotia2).
  - [~] Aplicación de `rebase.bin`: verificado offline que cubre `__mod_init_func` (366/366) y slots locales de `__nl_symbol_ptr` (244/244) → sin doble slide. Corrida 1: carga, rebase e imports OK en consola. Binario non-PIE con código `-mdynamic-no-pic`: `macho_prep.py` ahora rebasea literal pools de `__text` y tablas de switch (61851 palabras).
  - [~] Resolución de imports: 401/424 implementados; 23 stubs automáticos (CoreGraphics/UIGraphics de IGP/GLLive, personalidades de excepción).
  - [~] Ejecución de inicializadores globales con `macho_run_inits()` (ya no suma el slide dos veces).
  - [~] Runtime ObjC + `UIApplicationMain` → `applicationDidFinishLaunching:` → NSTimer → `-[EAGLView drawView:]`.
- [ ] **2. Render** — GATE: primer frame / pantalla de inicio visible en consola física.
  - [~] vitaGL + FBO virtual: FBO/renderbuffers OES reales (render-to-texture intacto); el renderbuffer del CAEAGLLayer se reemplaza por la textura que se presenta. 53 funciones GLES1 nuevas.
  - [x] CgBI con `cgbi.c`: idéntico píxel a píxel a la referencia (`tools/cgbi_check.py`). No aplica a las texturas del juego (Irrlicht las carga de `data.pack`); solo GLLive usa UIImage.
  - [~] Swap en `presentRenderbuffer:` (vsync de vitaGL marca el ritmo del NSTimer de 1/60 s).
- [ ] **3. Input** — GATE: controles funcionales in-game.
  - [~] Multitouch frontal → `UITouch` en coordenadas portrait de EAGLView (`to_view()` en uikit.c es la inversa de la rotación de `gles_present`).
  - [~] Botones físicos y stick izquierdo → toques sintéticos (`patch_input()`); posiciones sin calibrar. Stick derecho → acelerómetro.
- [ ] **4. Texto y fuentes** — GATE: diálogos y menús legibles.
  - [x] Análisis: el texto in-game es de Irrlicht (fuentes bitmap en `gll_font.pak`), no necesita FreeType.
  - [~] FreeType (`sa0:data/font/pvf/` o `font.ttf`) queda para `sizeWithFont:`/`drawAtPoint:` de UIKit.
- [ ] **5. Audio y video** — GATE: música y efectos de sonido activos.
  - [~] Audio por OpenAL (no RemoteIO): openal-soft del VitaSDK; AudioSession en stubs.
  - [x] Decodificación: la hace el propio juego (`AdpcmDecoder`, `audio_*.pack`); el loader solo recibe PCM en `alBufferData`.
  - [ ] Cinemáticas `.m4v`: hoy se saltan (se postean las notificaciones de preload/fin). Siguiente paso: SceAvPlayer.
- [ ] **6. Guardado y LiveArea** — GATE: progreso persistente y LiveArea completa.
  - [~] Persistencia en `ux0:data/zombieinfection/saves/`: `NSSearchPathForDirectoriesInDomains(Documents)` y `<bundle>/../Documents/` (normalizado en `darwin_path`, test en `tools/test_darwin_path.sh`).
  - [x] LiveArea desde el ícono y splash del juego (`tools/make_livearea.py` → `extras/livearea_game/`, fuera de git), PNG indexados validados con `psvita-toolkit livearea --validate` y empaquetados en el VPK.

## Iteración y Triage de Crashes

1. Staging de assets: `tools/stage_data.sh` (+ `python3 -I tools/make_livearea.py` para la LiveArea)
2. Despliegue por FTP: `python3 -I tools/deploy_data.py` (o `psvita-toolkit deploy --data`)
3. Si ocurre un crash:
   - Descargar logs: `python3 -I tools/deploy_data.py --logs`
   - Analizar volcado: `psvita-toolkit analyze logs/<dump>.psp2dmp --elf build/zombieinfection`
   - Si el PC está en `0x98xxxxxx`, restar `0x98000000` para ubicar la dirección en el Mach-O original con `tools/annotate.py`.
