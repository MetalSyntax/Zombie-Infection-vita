# Zombie Infection — Progreso del Port (iOS Mach-O)

Leyenda: `[x]` hecho y verificado · `[~]` implementado y compilado, falta verificar en consola · `[ ]` pendiente.
Los GATE solo se marcan con una corrida en la consola física.

- [x] **0. Repo y herramientas**: IPA extraído, binario analizado, `game_app` symlink, scaffolding creado.
- [x] **1. Loader mínimo** — GATE ✅ en consola (log_003).
  - [~] Carga de segmentos en memoria con `macho_load` (slide `0x98000000`).
  - [~] Aplicación de `rebase.bin` (cobertura verificada offline).
  - [~] Resolución de imports indispensables de libc, math, pthread y runtime (401/424).
  - [~] Ejecución de inicializadores globales con `macho_run_inits()`.
  - [~] Runtime ObjC + UIApplicationMain + run loop.
- [ ] **2. Render** — GATE: primer frame / pantalla de inicio visible en consola física.
  - [~] Inicialización de vitaGL y FBO virtual de presentación.
  - [x] Manejo de texturas PNG CgBI con `cgbi.c` (verificado; no lo usan las texturas del juego).
  - [~] Swap de buffers funcional a ~60 FPS sin splashscreen freeze.
- [ ] **3. Input** — GATE: controles funcionales in-game.
  - [~] Multitouch frontal de la Vita mapeado a las coordenadas lógicas del juego.
  - [~] Mapeo de botones físicos (D-Pad, Cross, Circle, Square, Triangle, Triggers) — sin calibrar.
- [ ] **4. Texto y fuentes** — GATE: diálogos y menús legibles.
  - [x] Análisis: texto in-game = fuentes bitmap de Irrlicht (`gll_font.pak`).
  - [~] FreeType sobre atlas de fuentes para UIKit (`sizeWithFont:`).
- [ ] **5. Audio y video** — GATE: música y efectos de sonido activos.
  - [~] OpenAL (openal-soft) + AudioSession — el juego no usa RemoteIO.
  - [x] Formato y decodificación de audio: ADPCM decodificado por el propio juego.
  - [ ] Cinemáticas `.m4v` (hoy se saltan).
- [ ] **6. Guardado y LiveArea** — GATE: progreso persistente y LiveArea completa.
  - [~] Persistencia de partidas en `ux0:data/zombieinfection/saves/`.
  - [x] LiveArea personalizada con `icon0.png`, `pic0.png`, `bg0.png`, `startup.png`.

---

## 2026-10-18 — Sesión 1: loader, runtime ObjC y backends (sin consola)

La Vita no respondía (FTP 1337 y 1338 cerrados en 192.168.3.15): todo se verificó compilando y
con chequeos offline contra el binario. **Ningún GATE se probó en consola.**

### Fase 1 — Loader

- **`macho.c` reescrito con la carga probada de Inotia2.** La copia del boilerplate tenía 4 bugs
  que habrían roto el arranque:
  1. `sceKernelAllocMemBlockForVM` no fija la dirección, pero el código escribía en
     `0x98000000 + vmaddr` → ahora `kuKernelAllocMemBlock` con `field_C = dirección` (TEXT, DATA
     y la arena de thunks justo antes de TEXT).
  2. Slots `INDIRECT_SYMBOL_LOCAL` de `__nl_symbol_ptr` sumaban el slide otra vez (rebase.bin ya
     los cubre: 244/244 verificado).
  3. `macho_run_inits` sumaba el slide a punteros ya rebaseados (366/366 en rebase.bin).
  4. Relocs externas pisaban el slot en vez de sumar el addend (rompía p. ej. vptr =
     `vtable for __class_type_info + 8`).
  También: `macho_hook` vuelve a soportar Thumb y entra por `ml_align_call`; los imports que el
  binario define él mismo (símbolos weak/coalesced) se resuelven contra el propio binario.
- **`hle.h`**: `BUNDLE_PATH` apuntaba a `game_app/`; en la consola el bundle está en
  `ZombieInfection.app/` (como lo deja `stage_data.sh`).
- **Imports** (424 indefinidos reales según `nm -u`; el reporte de 1098 contaba símbolos C++ que el
  binario define): faltaban 302 → quedan 23 como stubs con log.
  - `darwin.c`: ~60 funciones (stat/dirent/tm con layout Darwin convertido, printf/snprintf con
    el formateador Darwin, swprintf, strings, math, red offline, pthread cancel, uname...).
  - `cxx.c` (nuevo): 57 símbolos de libstdc++/libsupc++ → libstdc++ del VitaSDK (ABI COW vieja;
    solo cambia el mangling de size_t `m`→`j`). Todos verificados con `nm libstdc++.a`.
- **Runtime ObjC real** (`objc.c` + `objc_rt.h`, el anterior era el de Inotia: un `strcmp` por
  selector sin despacho a clases del juego):
  - `objc_msgSend` / `_stret` / `Super2` en asm: buscan el IMP y hacen tail-call con registros y
    pila intactos; solo nuestro código entra por `ml_align_call`. Enlazados directo (sin thunk).
  - Clases del juego: se recorren sus `class_ro_t` compilados; clases host (NSObject, UIView, ...)
    creadas a demanda para cada `_OBJC_CLASS_$_X` importado, con tablas de métodos en C.
  - Caché (clase, SEL) con spinlock (el juego crea hilos).
  - Foundation: NSString (layout `__cfstring`), NSNumber, NSArray/NSSet (fast enumeration),
    NSDictionary, NSData, NSDate, NSBundle, NSLocale, NSUserDefaults, NSFileManager,
    NSFileHandle, NSNotificationCenter, NSThread, NSURL/NSError offline, CFBundle.
- **`uikit.c`** (nuevo): `UIApplicationMain` reproduce `MainWindow.nib` (UIWindow 320x480 +
  EAGLView `initWithCoder:`, outlets `window`/`glView`), manda `applicationDidFinishLaunching:`,
  `layoutSubviews` (crea el framebuffer) y corre el run loop: input → UITouch, acelerómetro,
  NSTimer (`drawView:`), callbacks diferidos (alertas, notificaciones de video).
- **`tools/annotate.py`**: soporta modo ARM (`ldr rX,[pc,#n]; ldr rY,[pc,rX]`, pc+8).

### Fase 2 — Render

- FBO/renderbuffers OES → objetos reales de vitaGL (render-to-texture del juego intacto). El
  renderbuffer marcado por `renderbufferStorage:fromDrawable:` se sustituye por `fbo_tex` al
  adjuntarlo como color; el depth del juego es un renderbuffer real.
- 53 funciones GLES1 nuevas mapeadas directo (loader y vitaGL comparten softfp), `glDrawElements`
  con conversión GL_FIXED, `glLightf`/`glPointParameterf` envueltos.
- CgBI: `cgbi_check.py` → idéntico a la referencia. Las texturas del juego las carga Irrlicht
  desde `data.pack`; UIImage/CGContextDrawImage solo lo usa GLLive.

### Fase 3 — Input

- Toques Vita → `UITouch` (began/moved/ended por id de dedo) en coordenadas portrait de EAGLView;
  `to_view()` es la inversa de la rotación de `gles_present` (si la imagen sale girada, se
  corrigen juntas).
- `patch_input()`: botones → toques sintéticos; stick izquierdo → joystick virtual (touch-down en
  el centro y arrastre); stick derecho → acelerómetro. **Posiciones sin calibrar.**

### Fase 4 — Texto

- El texto in-game es de Irrlicht; FreeType queda para `sizeWithFont:`/`drawAtPoint:` de UIKit.

### Fase 5 — Audio y video

- El juego usa OpenAL (no RemoteIO): mapeado a openal-soft del VitaSDK (`libopenal`,
  `SceAudioIn_stub`). `alBufferDataStatic` (extensión Apple pedida por `alcGetProcAddress`) se
  emula con `alBufferData`.
- Cinemáticas saltadas: `MPMoviePlayerController` postea preload y fin de reproducción.

### Fase 6 — Guardado y LiveArea

- Guardado: `NSSearchPathForDirectoriesInDomains(Documents)` → `saves/`; `darwin_path` colapsa
  `dir/../` y redirige `Documents/` → `saves/` (el juego abre `<bundle>/../Documents/...`).
  Test host: `sh tools/test_darwin_path.sh` → `darwin_path ok`.
- LiveArea: las de `extras/livearea/` eran placeholders ("iPhone OS"). `tools/make_livearea.py`
  genera `extras/livearea_game/` (ícono + splash del juego, PNG indexados; ignorada por git por
  ser contenido del IPA). CMake la prefiere si existe. Validada y empaquetada en el VPK.

### Verificación hecha

- `cmake --build build` sin errores ni warnings nuevos; VPK generado.
- Cobertura de imports 401/424; selectores del juego sin implementación: 54, todos setters de
  widgets UIKit / red / biblioteca de música (nil es correcto).
- `tools/test_darwin_path.sh`, `tools/cgbi_check.py`, `psvita-toolkit livearea --validate`: OK.

### Riesgos conocidos

- Nada de esto corrió en hardware. El primer arranque probablemente tenga crashes de runtime
  ObjC/UIKit: un bug por corrida, guiado por `logs/log_NNN.log`.
- Sin conteo de referencias: `release` no libera (fuga lenta en sesiones largas).
- Excepciones C++ del juego son SjLj y la libstdc++ de la Vita usa EHABI: un `throw` real termina
  el proceso.
- El render es a 320x480 escalado (borroso); subir la resolución del FBO es una mejora posterior.

### Siguiente corrida en consola (checklist)

1. `tools/stage_data.sh` → `python3 -I tools/deploy_data.py` → instalar `build/zombieinfection.vpk`.
2. Requisitos: `kubridge.skprx` en `ur0:tai/config.txt`, `libshacccg.suprx` (ShaRKBR33D).
3. Bajar logs (`python3 -I tools/deploy_data.py --logs`) y buscar, en orden: `rebase:`,
   `imports:`, `global constructors:`, `UIApplicationMain`, `NSTimer`, `EAGL drawable`,
   `objc: ... not implemented`.

## 2026-10-18 — Corrida 1 en consola (log_001 + psp2dmp)

**Resultado:** el loader funciona hasta los constructores globales: carga en dirección fija,
`rebase: 55401 words`, `imports: 7358 slots, 18 unimplemented`, runtime ObjC con 14 clases host,
y 68 de 366 `mod_init` ejecutados.

**Crash:** Data abort en `irr::CIrrFactory::CIrrFactory()` (PC `0x980207d0` = Mach-O `0x207d0`),
`str r0, [r3]` con `r3 = 0x00599a0c`: una dirección de `__DATA` **sin slide**.

**Causa raíz:** el ejecutable es non-PIE y no trae relocaciones locales (`nlocrel 0`); Irrlicht
está compilado con `-mdynamic-no-pic`, así que sus literal pools en `__text` guardan direcciones
absolutas (`ldr r3, =vtable` / `ldr r3, =g_singleton`). `macho_prep.py` solo escaneaba secciones
de datos. (Inotia2 era Thumb PIC y no tenía este problema.)

**Fix (`tools/macho_prep.py`):**
- Escaneo de literal pools de `__text`: cada `ldr Rt, [pc, #n]` (ARM y Thumb-1, según el bit
  Thumb del símbolo de la función). Si en las 8 instrucciones siguientes Rt se usa con pc
  (`add Rd, pc, Rt`, `ldr/str Rd, [pc, Rt]`; Thumb `add Rt, pc`) es un offset PIC y no se toca;
  si no, se rebasea cuando el valor apunta a un destino válido (mismo clasificador que los datos).
  → +5002 palabras (vtables/objetos 3644, cadenas 1043, símbolos 281, funciones 34).
- Tablas de `switch` non-PIC (`ldrCC pc, [pc, Rm, lsl #2]` + tabla inline de direcciones de
  case): 64 tablas, 1448 entradas.
- 2380 literales en rango rechazados quedan en el reporte (`__text-lit`); el muestreo muestra
  enteros (`0xFFFF`, offsets de struct ~0x1028, `5000`), no punteros.
- `rebase.bin`: 55401 → 61851 palabras. Los literales del crash (`0x207d8` vtable, `0x207dc`
  `0x599a0c`) entran; los offsets PIC de `main` no.

**Mejora:** `macho_run_inits` ya no loguea cada constructor (cada línea con flush cuesta ~9 ms en
ux0: 366 = ~3 s de arranque); el dump da PC/LR exactos.

**Desplegado:** `rebase.bin` y `eboot.bin` subidos por FTP (tamaños verificados en la consola).
Siguiente corrida: confirmar `global constructors: 366 done` y llegar a `UIApplicationMain`.

## 2026-10-18 — Fix del build con `psvita-toolkit build`

`make: No targets specified and no makefile found` en `vitaGL_vendored`: el toolkit copia el código
a un directorio temporal respetando `.gitignore`, y el patrón suelto `Makefile` (para artefactos de
CMake) excluía `vendor/vitaGL/Makefile`. Los patrones de CMake ahora están anclados a la raíz
(`/Makefile`, `/CMakeCache.txt`, ...). `psvita-toolkit build` genera el VPK (con la LiveArea del juego).

## 2026-10-18 — Corrida 2 en consola (log_002 + psp2dmp)

**Resultado:** `rebase: 61851 words`; el crash de `CIrrFactory` desapareció. Nuevo Data abort
dentro de los constructores, en un `__static_initialization_and_destruction_0` (Mach-O `0x2748`,
`strb r6, [r4, #0x40]`) con `r4 = 0x3059a368` = `0x0059a368 + 2 × 0x98000000`: **doble slide**.

**Causa:** `ldr r4, [pc, #0x78]` (`0x26f8`) carga un offset PIC cuyo `add r4, pc, r4` está
10 instrucciones después; la ventana fija de 8 instrucciones no lo vio, el literal se tomó como
absoluto y se rebaseó, y después el código le sumó `pc`.

**Fix (`tools/macho_prep.py`):** el uso de Rt se sigue hasta que se redefine (escritura de
data-processing/load o `ldm` que lo carga), hasta un fin de flujo (`bx Rm`, `ldm {..., pc}`) o el
fin de la función (máx. 64 instrucciones). Las palabras del literal pool se saltean al decodificar
(antes un literal `0x003fa0f8` tras un `bx r12` parecía `ldrh ..., [pc, r8]`). Lo mismo en Thumb.
Resultado: 23 literales pasan a PIC, todos con uso PIC verificado en el desensamblado (a 9–24
instrucciones); `rebase.bin` 61851 → 61828 palabras.

**Test:** `python3 -I tools/test_macho_prep.py` — casos de las corridas 1 y 2 más dos de borde.

**Desplegado:** `rebase.bin` subido por FTP (el eboot no cambió).

## 2026-10-18 — Corrida 3 en consola (log_003 + psp2dmp)

**Resultado (gran avance):** 366 constructores OK → `main` → `UIApplicationMain` (nib emulado) →
`EAGLContext initWithAPI:1` → `applicationDidFinishLaunching:` → NSTimer `drawView:` cada
1/60 s → framebuffer EAGL creado → `Application::Init` (Irrlicht) arranca. **Fase 1 GATE
cumplido: el log llega al bucle principal / inicialización del motor.**

**Crash:** Data abort en `ASprite::PaintFrame` desde `GSSplash::draw`: el sprite no tiene datos.
Antes, todos los recursos fallaron: `fopen(ux0:data/zombieinfection/data.pack) -> 0`,
`ZI_Strings_EN.gt`, `main_menu.bsprite`.

**Causa:** Irrlicht hace `changeWorkingDirectoryTo(bundle)` → `chdir()` y abre los recursos con
rutas relativas (`addPakFileArchive("data.pack")`; `CFileStream::Open` = `getcwd() + nombre`).
`chdir` era un no-op y las rutas relativas caían en `DATA_PATH`.

**Fix (`darwin.c`):** cwd emulado. `chdir` lo valida y guarda; `getcwd` lo devuelve sin `/` final
(POSIX); `darwin_path` resuelve rutas relativas contra él (`./` incluido). Por defecto es el
bundle. Test: `sh tools/test_darwin_path.sh` (casos relativos agregados).

**Desplegado:** no — la Vita no respondía por FTP. Subir `build/eboot.bin` a `ux0:app/PSVZI0001/`.

## 2026-10-18 — Corrida 4 (log_004): gameplay alcanzado; FPS, gráficos e input

**Resultado:** el juego llega a las cinemáticas in-engine y al gameplay (tutorial). Problemas
reportados: errores gráficos y bajones de FPS en las cinemáticas del inicio; táctil y botones
físicos no funcionan bien (no se puede completar el tutorial de cámara).

**FPS:** 8187 de las 8213 líneas del log eran `NSTimer: every 0.00Xs -> -[EAGLView drawView:]`:
EAGLView recrea su timer en cada frame (compensa el tiempo de frame) y cada línea con flush
cuesta ~9 ms en ux0 → el log se comía medio frame. Ahora se loguean solo los 4 primeros. Los
timers invalidados se reciclan tras 2 s (antes: ~14 MB/hora de fuga).

**Gráficos:** el blit de presentación solo neutralizaba blend/alpha/scissor/depth. Ahora también
guarda, desactiva y restaura iluminación, fog, culling, stencil, clip plane, la 2.ª unidad de
textura, la matriz de textura y las máscaras de color/profundidad (las cinemáticas dejan fog e
iluminación activos). `GL_COLOR_MATERIAL` no se toca: vitaGL no lo reporta en `glIsEnabled`.
Pendiente de confirmar en consola si esa era la causa visual.

**Táctil — causas:**
1. El juego decide la orientación por la inclinación del acelerómetro (`Application::
   UpdateOrientationChanges`: `|s_currentTiltX| > 20` alterna 1↔3) y el acelerómetro venía del
   stick derecho: mover el stick (cámara) giraba 180° los controles. Ahora el dispositivo está
   quieto (0, 0, -1); la inclinación solo se usa para eso.
2. La orientación inicial del juego es 1 (no 3), pero `to_view()` y el blit asumían 3. Ahora
   ambos leen `CTouchPad::s_currentDeviceOrientation` en vivo: 3 → `(320-ly, lx)`, 1 →
   `(ly, 480-lx)` (inversas exactas de `CTouchPad::TransformCoordinatesByOrientation`), y la
   imagen rota 180° con 1. Los cambios de orientación se loguean.

**Botones físicos:** el HUD es reubicable (modo edición, posiciones guardadas), así que las
coordenadas fijas no servían. `patch_input()` ahora lee `CHudManager` en runtime
(`*c_pTls → +4 → +0x18 (Application) → +0xc8`) y toca el centro de la región táctil real
(`HudElement +0x24..+0x30`, `rect<float>`) del primer botón visible (`+8`), en el mismo orden que
`CHudManager::get*ButtonID()`:

| Vita | Acción | Offsets en CHudManager |
|---|---|---|
| X / R | ataque | 0x74, 0x1c, 0x84, 0x80 |
| □ | ataque secundario | 0x20 |
| ○ | usar / contextual | 0x24, 0x28, 0x30, 0x34 |
| △ | salto | 0x64 |
| L | esquivar | 0x60 |
| ↑ | girar | 0x78 |
| Start / Select | volver / pausa | 0x3c, 0x2c |
| Stick izq. | AnalogStick | 0x18 (centro + deflexión × radio) |
| Stick der. | cámara | arrastre desde (300,150), se levanta y repite cada 90 px |

Fuera del gameplay (HUD oculto) los botones no generan toques: menús por táctil.

**Desplegado:** `eboot.bin` subido (tamaño verificado).

## 2026-10-18 — Corrida 5 (log_005): texturas distorsionadas ocasionales + cámara

**Estado:** orientación estable (`game device orientation: 3`), FPS y resto de controles "decentes".

**Texturas/geometría distorsionadas — causa encontrada en vitaGL:** `glVertexPointer` y
`glTexCoordPointer` no aceptan `GL_BYTE`, y `glColorPointer` no acepta `GL_FIXED` (los tres son
válidos en GLES1). Ante esos tipos vitaGL da `GL_INVALID_ENUM` y **conserva el puntero
anterior**: el draw usa los vértices/UV de la malla previa. Irrlicht elige el tipo por malla
(`S3DVertexComponentArrays`) y suele usar VBO (`forceCommitToVRAM`), donde la conversión vieja
(solo GL_FIXED de cliente) no actuaba.
- `gles.c` registra los 5 arrays (vértice, UV×2, color, normal) con su VBO y estado habilitado;
  los tipos no soportados se convierten a float en el draw leyendo el VBO por su puntero de CPU
  (en vitaGL el nombre del buffer es `vbo*` con `ptr` como primer campo; no se usa
  `glUnmapNamedBuffer`, que marcaría el buffer como no usado por la GPU). `glDrawElements` lee
  los índices del element buffer si hay uno.
- Bug propio corregido: tras el blit, los punteros se re-aplicaban sin su VBO original (un offset
  pasaba a leerse como memoria). Ahora cada array se re-aplica con su buffer y el blit usa
  `GL_ARRAY_BUFFER = 0`.
- El log anota cada combinación nueva de array/tipo (`GL array K: ... (converted)`) para
  confirmar en la próxima corrida.
- Revisado y descartado: tamaños mínimos de PVRTC en vitaGL (correctos), `glCopyTexImage2D`
  (`glReadPixels` espera a la GPU con `sceGxmFinish`), mips de `glTexImage2D` (se regeneran
  del nivel 0).
- Las barras laterales se limpian en negro (antes, con el color de clear del juego).

**Cámara (stick derecho) — modelo de Shadow Guardian:** un dedo apoyado y arrastrado de forma
continua por velocidad (antes se levantaba cada 90 px: "mecánico"), coordenadas que pueden salir
de la pantalla (el juego usa el delta), deadzone 0.15 + curva 0.35t + 0.65t², acumulador float,
ancla elegida fuera de los controles visibles del HUD, re-anclado solo a 3000 px. Sensibilidad
1..10 con **SELECT + ↑/↓** (default 5), guardada en `ux0:data/zombieinfection/camera_sens.txt`,
con barra OSD 1.5 s. SELECT ya no es "volver" (queda Start).

**Desplegado:** no — la Vita no respondía. Subir `build/eboot.bin`.

## 2026-10-19 — Corrida 6 (log_006): cinemáticas, pantalla completa, enfundar, HUD 1%

**Diagnóstico descartado:** el log de arrays muestra solo float/short/ubyte de cliente (sin VBO,
sin conversiones): la hipótesis de tipos de array no era la causa (la conversión queda como red
de seguridad).

**Causa de los errores gráficos en cinemáticas (vitaGL):** `glFramebufferRenderbuffer` **descarta
el depth buffer del framebuffer** en cada llamada (`mark_as_dirty` + `DEPTHBUFFER_MISSING`; los
renderbuffers de vitaGL no tienen memoria de profundidad propia). `LibEffects::Manager::
BeginRenderTargetGroup` (efectos de cinemática) re-adjunta el depth (`0x8D00`) en cada frame →
profundidad perdida a mitad del frame y re-asignada sin inicializar. Fix: solo se reenvía el
primer adjunto de depth/stencil de cada framebuffer (los siguientes no cambian qué memoria se usa);
`glDeleteFramebuffersOES` limpia el registro.

**Pantalla completa sin estirar:** al dibujar en el framebuffer de la ventana, la proyección se
pre-multiplica por S (eje de pantalla horizontal = Y del framebuffer portrait, × 816/960) y el
blit ocupa los 960 px. Resultado: el 3D gana campo de visión a los lados con proporciones
correctas, y el HUD/menús (480x320) quedan en su tamaño original centrados (mismo mapeo táctil que
antes). Scissor, `glClear` y `glCopyTex(Sub)Image2D` desde la ventana se transforman igual; los
render-to-texture no se tocan. La proyección del juego se restaura después de cada draw.

**Enfundar:** D-pad ↓ toca el botón "Back" del HUD (+0x2c/+0x3c), que el juego procesa en
`COnFootCombatControlHandler::EmulateBackButton` → `PlayerTest::unDrawWeapon()`.

**Controles táctiles al 1%:** `VirtualButton::draw` y `AnalogStick::draw` pasan alfa 255 fijo a
`ASprite::PaintFrame` (verificado: `PaintModule` lo usa como alfa de vértices). Se reescribe el
inmediato de `add rX, rX, #255` (0x53e60, 0x52158, 0x521b0; se valida el opcode original antes de
parchear) a 3 (~1%). **L + R** los muestra/oculta (mientras ambos están apretados no esquivan ni
atacan).

## 2026-10-27 — Corrida 7 (log_007): menú de pausa con Start y Select

**Problema:** Tras 17 minutos de juego in-game en hardware real, al pulsar Start o Select no se abría el menú de pausa / opciones / inventario.

**Causa raíz desensamblando el binario Mach-O:**
1. En `CHudManager`, el botón de pausa in-game es el `VirtualButton` almacenado en el offset `+0x38` (56 en decimal), que tiene `button_id == 6`.
2. Al tocarlo, emite `EvVirtualButton(id=6, state=0)`, el cual es recibido por `COnFootCombatControlHandler::onEvent` y llama a `CLevel::SetPause()`. Al frame siguiente, `GS3DStuff::update` detecta `CLevel::IsPaused()` y empuja la escena `GSIngamePause`.
3. Sin embargo, en `patch.c`, `hud_map` tenía `SCE_CTRL_START` mapeado erróneamente a `{0x3c, 0x2c}` (los botones de "Back" / enfundar arma de `CHudManager::getBackButtonID()`), en lugar de `{0x38}`.
4. Además, Select estaba consumido en exclusividad para calibrar sensibilidad de cámara (`SELECT + ↑/↓`).

**Fix (`source/patch.c`):**
1. Mapeo de `SCE_CTRL_START` corregido en `hud_map` al offset real del botón de pausa: `{0x38}`.
2. Pulsar / soltar Select de forma individual (tap sin flechas ↑/↓) ahora sintetiza la pulsación de `SCE_CTRL_START`, permitiendo abrir el menú tanto con Start como con Select. Si se mantiene pulsado Select junto con ↑/↓, se ajusta la sensibilidad de cámara normalmente sin pausar.
3. Al estar en pausa (`is_paused`), pulsar Start o soltar Select invoca directamente `CLevel::SetResume()`, permitiendo reanudar la partida sin necesidad de tocar la pantalla táctil.
4. Corrección de linkeo con vitasdk moderno en `CMakeLists.txt` agregando `-Wl,--no-warn-mismatch` para compatibilidad de ABI softfp con librerías del SDK.

## 2026-10-28 — Corrida 8 (log_010 + psp2core-1793145679): crash START/SELECT, controles, HUD 0% y reproductor de video

**Problema 1 (Crash en log_010):**
Al pulsar START / SELECT en menús o inicio de partida, la consola arrojaba un Data abort en `0x98032de8` (`PlayerTest::isCarryingVaccineTank`).

**Causa raíz diagnosticada con psvita-toolkit analyze:**
1. En `patch.c`, existía un fallback que llamaba a `CLevel::SetPause(level)` cuando `!hud`. Al estar en la pantalla de título o menús iniciales, `hud` era NULL pero `level` ya estaba instanciado, por lo que presionar Start marcaba `is_paused = 1`.
2. Al pulsar Start de nuevo (o intentar reanudar), se invocaba `CLevel::SetResume()`.
3. `CLevel::SetResume()` llama a `Map::removeFullScreenMap()`, el cual invoca `CHudManager::hideFullMap()`.
4. `CHudManager::hideFullMap()` llama a `CControlManager::GetPlayerObj()`, el cual devuelve NULL porque en los menús no existe ningún objeto `PlayerTest`.
5. Inmediatamente después, `CHudManager::hideFullMap()` llama a `PlayerTest::isCarryingVaccineTank(player)` sin validar si es NULL.
6. `PlayerTest::isCarryingVaccineTank` ejecuta `ldr r0, [r0, #0x7e0]`, produciendo Data abort por dereferenciar `0x0 + 0x7e0`.

**Fix del crash (`source/patch.c`):**
1. Eliminado el fallback erróneo `c_level_set_pause(level)` cuando `!hud`: fuera del gameplay el motor no debe ser pausado.
2. Hook implementado con `macho_hook` en `PlayerTest::isCarryingVaccineTank` (`__ZN10PlayerTest21isCarryingVaccineTankEv`): si `player == NULL`, retorna 0 de forma segura.

**Problema 2 (Mapeo de botones Triangle y L):**
1. **△ (Triangle):** reconfigurado en `hud_map` a `{0x2c, 0x3c, 0x64}` para enfundar las armas (`PlayerTest::unDrawWeapon`), con fallback a salto si no se tienen armas desenfundadas.
2. **L (Trigger izquierdo):** reconfigurado para recargar arma tocando el HUD de arma/munición en la esquina superior derecha (`MultiElement` en offset `+0x98`), complementado con invocación directa a `MultiElement::ReloadBullets()` en `patch_input`.

**Problema 3 (Opacidad de botones de HUD):**
`HUD_ALPHA_HIDDEN` se ajustó de 3 (~1%) a 0 (0% de opacidad). De este modo, los botones virtuales de pantalla (incluyendo botón táctil de menú y botiquín) quedan 100% invisibles en el gameplay para una interfaz limpia, conservando el atajo **L + R** para alternar visibilidad total.

**Problema 4 (Glitches gráficos en cinemáticas in-engine):**
Recompilación limpia de `vendor/vitaGL` con `GENERIC_STACK_DEPTH 16` (antes 2), permitiendo anidamiento profundo de matrices de textura y proyección en `LibEffects` y `wide_begin` sin desbordamiento de pila.

**Problema 5 (Reproducción de video .m4v):**
Implementado subsistema completo en `source/video.c` y enlazado en `source/uikit.c`:
1. Demuxer MP4/M4V ISO-BMFF para extraer las pistas H.264 y AAC de las cinemáticas (`cinematic_02_lab.m4v`, `cinematic_03_fin.m4v`).
2. Decodificación de video H.264 acelerada por hardware de PS Vita (`SceVideodec` / `sceAvcdec`) renderizada directo a memoria CDRAM de vitaGL en textura 16:9 letterboxed.
3. Decodificación de audio AAC mediante FFmpeg (`libavcodec` / `swresample` / `libavutil`) con streaming en hilo dedicado sobre `SceAudioOut`.
4. Salto de cinemática con Cross, Circle o Start.
5. `source/uikit.c` intercepta `MPMoviePlayerController initWithContentURL:` y `play`, reproduciendo el video antes de disparar la notificación `MPMoviePlayerPlaybackDidFinishNotification`.

## 2026-10-28 — Corrida 9 (log_012 + psp2core-1793197449): crash transición de zona / cinemáticas en vitaGL

**Problema (Crash en log_012 tras ~22 minutos de juego):**
Al cambiar de zona donde iniciaba una cinemática/escena, el juego crasheaba con Data abort exception (`0x30004`) en hilo `PSVZI0001`:
- `PC: 0xe000cd86` (`SceLibKernel` `memcpy + 0x16`).
- `LR: 0x81043b89` (`reload_ffp_shaders` en `vendor/vitaGL/source/ffp.c:947`).
- `R0: 0x862f8bc8`, `R1: 0x00000000` (puntero fuente NULL), `R2: 0x862fa223` (tamaño), `R5/R10: 0x201800ef` (`frag_shader_mask`).

**Causa raíz diagnosticada con psvita-toolkit analyze:**
1. `vendor/vitaGL/source/ffp.c` invocaba `shark_compile_shader_extended` tanto para shaders de vértices como de fragmentos, pero **no verificaba si el resultado `t` era NULL**.
2. Inmediatamente llamaba a `vgl_fast_memcpy((void *)ffp_fragment_program, (void *)t, size)`, produciendo la excepción fatal en `memcpy` al desreferenciar `R1 = 0x0`.
3. Además, en `source/gles.c`: `vglInitExtended(0, SCR_W, SCR_H, 8 * 1024 * 1024, ...)` reservaba únicamente 8 MB para el heap del sistema/newlib (`malloc`). Al transicionar de zona tras 22 min de partida continua y cargar nuevos modelos, bancos de audio y streaming de cinemáticas, el espacio libre para `malloc` y los allocators de `vitaShaRK` (`libshacccg`) se agotaba, causando que la compilación devolviera NULL.

**Fix implementado:**
1. **Robustez y control de errores en `vendor/vitaGL/source/ffp.c`:**
   - Se añadieron verificaciones `if (t)` tanto para `SHARK_VERTEX_SHADER` como para `SHARK_FRAGMENT_SHADER`.
   - En caso de fallo de compilación, se extraen e imprimen los diagnósticos del compilador con `shark_get_internal_compile_output()`.
   - Se previene la copia nula (`memcpy`) y el registro de programas corruptos con `sceGxmShaderPatcherRegisterProgram`.
   - Se realiza fallback automático al último shader válido registrado en la caché de RAM (`frag_shader_cache` / `vert_shader_cache`).
2. **Aumento de memoria para transiciones en `source/gles.c`:**
   - `ram_threshold` en `vglInitExtended` ampliado de 8 MB a 32 MB (`32 * 1024 * 1024`), cuatriplicando el heap de newlib para la carga fluida de assets, audio y cinemáticas entre zonas.
3. Recompilación limpia de `vendor/vitaGL` (`libvitaGL.a`) y regeneración de `eboot.bin` y `zombieinfection.vpk`.

## 2026-10-28 — Corrida 10 (log_013): resolución de videos .m4v, eliminación de glitches gráficos y caída de FPS

**Problemas reportados:**
1. Los videos `.m4v` no se reproducían.
2. Caída drástica de FPS en la nueva zona.
3. Glitches gráficos y geometría distorsionada en cinemáticas in-engine.

**Diagnóstico y causas raíz:**
1. **Detección de videos .m4v (`source/objc.c`):**
   - `CLevel::GetCinematicName()` pasa a `MoviePlayer initWithFile:andFlipped:` el nombre sin extensión (`cinematic_02_lab`).
   - `MoviePlayer` llama a `[NSBundle pathForResource:name ofType:nil inDirectory:@""]`.
   - Al ser `type == nil`, `bundle_path` buscaba únicamente la ruta exacta sin extensión (`.../cinematic_02_lab`), la cual no existe en disco (`cinematic_02_lab.m4v`).
   - Al devolver NULL, `MoviePlayer` abortaba la creación de `MPMoviePlayerController` y retornaba `nil`, impidiendo que el reproductor de video se activara.
2. **Glitches gráficos por flags de compilación en vitaGL (`CMakeLists.txt`):**
   - `DRAW_SPEEDHACK=2`: saltaba la copia a ring buffer de la GPU para draws grandes, leyendo directamente de la memoria cliente del CPU que el motor actualiza dinámicamente, produciendo desgarros y geometría distorsionada.
   - `SAMPLERS_SPEEDHACK=1`: omitía resolución rigurosa de texturas/samplers en shaders, produciendo distorsión de texturas.
3. **Caída de FPS por saturación de I/O a disco (`source/darwin.c`, `source/log.c`, `source/uikit.c`):**
   - Al cargar la nueva zona, el motor sondea miles de variantes de archivos ausentes (`.tga`, `.bmp`, `.gif`, `.pvr`).
   - `d_fopen` llamaba a `l_debug` en cada intento fallido y `log.c` ejecutaba `fflush(log_file)` de forma síncrona a la tarjeta de memoria en cada línea, congelando la CPU.
   - `app_set_orientation` registraba a 60 Hz el estado de la barra de orientación en el log.
4. **Reserva de memoria para SceAvcdec (`source/gles.c`):**
   - `vglInitExtended` dejaba 0 bytes de umbral en CDRAM y PHYCONT.
   - Migrado a `vglInitWithCustomThreshold` reservando 16 MB de CDRAM y 8 MB de PHYCONT para los buffers del decodificador de video por hardware (`alloc_phycont`).

**Fix implementado:**
1. `bundle_path` en `source/objc.c` sondea automáticamente extensiones comunes (`.m4v`, `.mp4`, `.png`, `.tga`, `.pvr`) si `type` es nulo o el archivo base no existe.
2. Eliminados `DRAW_SPEEDHACK=2` y `SAMPLERS_SPEEDHACK=1` de `CMakeLists.txt`; recompilación limpia de `vendor/vitaGL`.
3. En `source/darwin.c`: `d_fopen` solo registra aperturas de archivo exitosas (`f != NULL`).
4. En `source/log.c`: `fflush` deshabilitado para mensajes `DEBUG`.
5. En `source/uikit.c`: `app_set_orientation` solo registra cuando la orientación cambia.
6. En `source/gles.c`: `vglInitWithCustomThreshold` asigna 32 MB RAM, 16 MB CDRAM y 8 MB PHYCONT.
7. Build generado limpio: `eboot.bin` y `zombieinfection.vpk`.

## 2026-10-28 — Corrida 11 (log_014): corrección de caída a 6 FPS (parcheo de vertex shaders) y clarificación de cinemáticas

**Diagnóstico de los 6 FPS y estado de videos:**
1. **Causa real de la caída a 6 FPS en la nueva zona (`vendor/vitaGL/source/ffp.c`):**
   - En la nueva zona se activa la segunda unidad de textura (`GL_TEXTURE1`, `mask.num_textures == 2`).
   - En `ffp.c`, la configuración compacta estática de atributos solo se usaba cuando `mask.num_textures == 1 && mask.lights_num == 0`.
   - Al ser `mask.num_textures == 2`, caía en la rama `else` (modo no inmediato), la cual llamaba incondicionalmente a `patch_vertex_program` (`sceGxmShaderPatcherCreateVertexProgram`) en **cada draw call** debido a que `ffp_dirty_vert_attr` se ensuciaba con `glVertexPointer`.
   - Invocar al patcher de shaders de GXM cientos de veces por frame destruía el rendimiento, bajando a 6 FPS.
2. **Doble buffering vs Triple buffering (`source/gles.c`):**
   - Con `vglUseTripleBuffering(GL_FALSE)`, cualquier frame que tardara >16.6 ms bloqueaba la CPU y GPU esperando el VSync.
3. **Cinemática de inicio ("intro video"):**
   - El IPA de iOS 1.0.0 contiene únicamente dos videos `.m4v`: `cinematic_02_lab.m4v` (final del laboratorio) y `cinematic_03_fin.m4v` (final del juego).
   - La introducción del juego (bote llegando al muelle, diálogo de Shaw y su compañera) es una **cinemática 3D in-engine renderizada en tiempo real** por el motor (`ScriptManager::isInCinematic()`), no un archivo de video pre-renderizado. Al correr a 6 FPS por el cuello de botella de shaders, parecía una reproducción fallida.

**Fix implementado:**
1. **Caché de Vertex Programs parcheados (`vendor/vitaGL/source/ffp.c`):**
   - Se implementó `patched_vprog_cache` (64 entradas) que guarda en memoria los programas ya parcheados para la tupla `(id, attrs, streams, num_params, index_type)`.
   - Si los atributos y el shader no han cambiado su estructura (mismo formato, componentes y strides), se reutiliza el programa ya compilado en 0 ms sin invocar `sceGxmShaderPatcherCreateVertexProgram`.
   - Se normalizó el empaquetado de atributos en la rama no inmediata para que `streamIndex`, `regIndex`, y `indexSource` sean 100% deterministas.
2. **Triple Buffering activado (`source/gles.c`):**
   - Se activó `vglUseTripleBuffering(GL_TRUE)` eliminando los bloqueos por VBlank en swaps de pantalla.
3. **Atajo de depuración para probar el reproductor de video (`source/patch.c`):**
   - Se agregó el combo de botones `L1 + R1 + TRIÁNGULO` para invocar directamente `video_play("ux0:data/zombieinfection/ZombieInfection.app/cinematic_02_lab.m4v")` en cualquier momento.
4. Build verificado con `psvita-toolkit build`: `eboot.bin` y `zombieinfection.vpk` generados correctamente.

> [!WARNING]
> **Issue Abierto**: El usuario reporta que persiste la caída a ~6 FPS en zonas específicas. Queda registrado como issue prioritario para investigar perfilado de CPU/GPU, conversión de vertex arrays de cliente (`prepare_draw`), conteo de draw calls y temporizadores en `uikit.c`.
