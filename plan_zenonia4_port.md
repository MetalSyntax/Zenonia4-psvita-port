# Plan de Port — Zenonia 4 (PS Vita)

## 0. Contexto y estrategia general

Zenonia 4 usa el **mismo motor propietario de Gamevil** que Zenonia 2 y Zenonia 3 ("Nexus2/Clet
engine"): mismo paquete JNI (`com.gamevil.nexus2.Natives`), misma familia de prefijos C++ internos
(`CGs*` = Gamevil System, `CMv*`, `CZn*` = Zenonia), resolución JNI 100% por convención de nombre (sin
`RegisterNatives`), mismo pipeline gráfico GLES1 de punto fijo. Esto se confirmó comparando
`nm -D libgameDSO.so` de Zenonia 4 contra la documentación de exports de Zenonia 3 — ver §2.

**Decisión de arquitectura: NO empezar desde cero ni desde el scaffold genérico que ya vive en este
repo.** Este repo (`Zenonia4-vita/`) tiene actualmente dos puntos de partida posibles y hay que elegir
uno:

1. `source/` + `extras/` — un template genérico de soloader vitasdk (estilo Rinnegatamante/Atamanenko:
   `so_util` como librería externa, `reimpl/*` para libc/pthread/mem/io/egl, `utils/glutil.h`). **No es
   específico del motor Gamevil** y está prácticamente vacío (`java.c` con tablas JNI en blanco,
   `main.c` con placeholders `// ... do some initialization`). No hay commits de trabajo real acá.
2. El linaje `Zenonia2-vita/loader/` → `Zenonia3-vita/loader/` — código específico del motor Nexus2/Clet,
   con ~13 bugs de motor ya diagnosticados y arreglados en hardware real, sistema de Android-UI overlay
   completo, mezclador de audio Tremor, hooks de runtime, parches binarios confirmados.

**Elegido: (2), forkeando `Zenonia3-vita/loader/` como base.** Es la opción que cumple el pedido
explícito de reutilizar código de un motor idéntico, y Zenonia 3 es el sibling más cercano en el tiempo
y en superficie de API (ver tabla de símbolos §2 — Zenonia 3 ya dividió `NativeInit()` en
`NativeInitDeviceInfo`/`NativeInitWithBufferSize`, igual que Zenonia 4; Zenonia 2 todavía tenía un único
`NativeInit()`). El template genérico de `source/`/`extras/` queda como referencia de patrones
alternativos (su enfoque de `reimpl/` modular es prolijo) pero no como base de código — recomendado
archivarlo o dejarlo sin tocar y marcarlo como no usado en el `CLAUDE.md` del proyecto.

`porting_tools/` (scripts heredados de Dungeon Hunter 2 / Prince of Persia Classic) son plantillas
genéricas de build/deploy, no del motor — ver "Adaptando `porting_tools/`" en `CLAUDE.md`; siguen
siendo válidos como base de `build.sh`/`manage_vita.py`/`decompile_all.sh` una vez reescritos con los
nombres correctos, pero el código del loader en sí debe venir de Zenonia3, no de estas plantillas.

Metodología: la misma que documenta `plan_zenonia3_port.md` para el salto Z2→Z3 — análisis estático de
símbolos primero (objdump/nm/jadx/Ghidra), tabla de diffs de ABI confirmados vs. hipótesis a verificar
en consola, un bug confirmado a la vez en hardware real (no parches especulativos en batch).

## 1. Qué ya está confirmado en este repo (sin escribir código todavía)

- Único binario nativo: `zenonia-4-1-2-3/lib/armeabi/libgameDSO.so` (armeabi puro, sin v7a), ~1.63 MB
  (vs. 1.28 MB de Zenonia 3 — más grande, más subsistemas, ver §2).
- Activity principal: `.SkeletonLauncher` (`AndroidManifest.xml`).
- Los formatos de asset propietarios `.zt1` (tablas de texto) y `.pzx` (recursos empaquetados) **NO son
  nuevos** — ya existen y se manejan sin problema en Zenonia 2 y Zenonia 3 (se sirven como bytes opacos
  vía `readAssets`/`fopen_hook`, el motor los parsea internamente; el loader nunca necesita entender el
  formato). El `CLAUDE.md` actual de este repo todavía los marca como "no asumir, investigar" — eso ya
  está resuelto por precedente directo, corregir esa sección cuando se actualice el `CLAUDE.md`.
- `extras/livearea/` ya tiene `icon0.png`, `bg0.png`, `pic0.png`, `startup.png`, `template.xml` listos
  para empaquetar (mismo mecanismo que `sce_sys/` en Z2/Z3) — no hace falta rehacer el arte de LiveArea.
- No hay `.obb`; los assets van dentro de `assets/` del APK mismo, igual que Z2/Z3 → mismo mecanismo de
  extracción a `ux0_data/zenonia-4/`.

## 2. Diff de símbolos exportados: Zenonia 3 vs Zenonia 4

Confirmado con `nm -D` sobre ambos `.so`. Métodos que **ya existían** en Zenonia 3 (reutilizar
directamente la lógica de `java.c`/`dynlib.c`/`main.c` de Z3, verificando firma exacta contra
`out_ghidra.c` de Z4, nunca asumir que es idéntica):

`JNI_OnLoad`, `NativeInitDeviceInfo`, `NativeInitWithBufferSize`, `NativeRender`, `NativeResize`,
`handleCletEvent`, `NativeDestroyClet`, `NativePauseClet`, `NativeResumeClet`, `SetCletStarted`.

Métodos **nuevos, sin precedente en Z2/Z3** — trabajo genuinamente nuevo, no hay código previo que
portar más que el patrón general de "stub JNI que no bloquea":

| Símbolo | Subsistema probable | Riesgo |
|---|---|---|
| `NativeHandleInAppBiiling`, `NativeResponseIAP`, `nativeBillingResult`, `nativePrepaid`, `nativePrepaidWithdraw` | In-App Billing (Google Play Billing) | Alto si el motor espera una respuesta síncrona/callback antes de continuar — confirmar con Ghidra antes de asumir no-op seguro |
| `isTapjoyReady`, `nativeTapjoyAward`, `SetTJReady` | Tapjoy (ads) | A diferencia de Z3 (`NativeHandleTapjoyOffer` estaba declarado en Java pero **sin símbolo real** en el `.so`, o sea nunca se llamaba), Z4 **sí** tiene el símbolo real → sí hay que implementar el stub, no se puede asumir "no aplica" como en Z3 |
| `NativeGetPublicKey` | Verificación de licencia (Google Play Licensing) | Debe investigarse qué hace el caller con el valor devuelto antes de decidir si un stub arbitrario alcanza o si hay que neutralizar la llamada completa |
| `IsNetworking`, `NativeNetTimeOut` | Red / Gamevil Live | Candidato a no-op "sin red" — confirmar que el motor tiene un camino offline gracioso |
| `getLang` | Selección de idioma | Mapear a uno de los locales presentes en `assets/` (`eng/chi/deu/fra/jpn/tai`, sin `kor` confirmado — ver §6) |
| `nativeGetSelectGameSlot` | UI de selección de slot de partida | Pantalla nueva de Android-UI-overlay, no existía en Z2/Z3 |
| `NativeAsyncTimerCallBack`, `NativeAsyncTimerCallBackTimeStemp` | Timer asíncrono del motor | Confirmar si es polling opcional o crítico para algún subsistema |
| `getGameState`, `getMenuState`, `getOpenMainUIState`, `getState`, `returnToMainMenu` | Estado de UI/menú | Posible reemplazo/extensión del mecanismo `g_ui_status`/`OnUIStatusChange` que ya existe en Z3 — mapear contra `androidui.c` |
| `NativeIsNexusOne`, `InitializeJNIGlobalRef` | Inicialización / detección de dispositivo | Probablemente no-ops directos, bajo riesgo |

`nm` también expone símbolos C++ desmangleables no vistos documentados antes en Z2/Z3
(`_Z10DrawNativeliiiiii10enumDrawOPih`, `CGcxBitmap::PutNativeImage`, `CGsGraphics::DrawNativeFillEllipse`)
— sugiere que el pipeline gráfico de Z4 tiene primitivas de dibujo adicionales (posiblemente para UI
nueva tipo minimapa/elipses). Confirmar si esto afecta a los shims GLES1 de `dynlib.c` o es
autocontenido dentro del `.so`.

## 3. Plan de reutilización de código, archivo por archivo

### Copiar casi verbatim desde `Zenonia3-vita/`

- `lib/falso_jni/` — FalsoJNI vendored, sin cambios.
- `lib/stb/` — stb_truetype, sin cambios.
- `loader/so_util.c`/`.h` — cargador ELF32, relocate/resolve/hooks. Motor y toolchain idénticos, cero
  razón para reescribir.
- `common/debugScreen.*` — consola de framebuffer para errores fatales.
- `loader/htmltext.c`/`.h` + `loader/htmlview.c`/`.h` — extractor HTML→texto y renderer de scroll. Z4
  tiene sus propios `live.html`, `live_auth_terms.html`, `circle_policy_h.html`, `circle_policy_v.html`
  además de un probable `about`/`help` — el parser tolera el markup `<tr><td>` visto en Z2/Z3, confirmar
  que el markup de estos HTML nuevos es compatible antes de asumirlo.
- `loader/font.c`/`.h` (backend GFA de fuentes) y `ksc5601_table.h` **solo si** Z4 requiere texto
  coreano — ver §6 (no confirmado todavía que haya locale `kor`).
- `loader/audio.c`/`.h` — mezclador Tremor/`sceAudioOut`. Confirmar tabla de IDs de sonido contra los
  assets reales de Z4 antes de asumir que el mapeo es igual.
- Patrón de `CMakeLists.txt`, `build.sh` (staging en `/tmp` por el espacio en el path), `clean_macos.sh`.

### Adaptar (misma estructura, contenido específico de Z4)

- `loader/dynlib.c` — tabla de imports base igual (mismo pipeline GLES1: `glTexSubImage2D` RGB565→RGBA,
  filtros forzados, `GL_FIXED`→float diferido), pero re-derivar la import list completa con
  `objdump -T | grep UND` sobre el `.so` de Z4 — no asumir que es el mismo set que Z3 (Z3 ya difirió de
  Z2 en `__android_log_print`).
- `loader/java.c` — tabla de métodos JNI: portar los ~10 métodos que ya existían en Z3 (verificando
  firma), agregar los ~15 métodos nuevos de §2 (billing, Tapjoy, licensing, slot, timer, estado UI).
- `loader/androidui.c` — el patrón de overlay (parsear `g_ui_status`, dibujar quads GL en las posiciones
  exactas que Java dibujaba) se reutiliza, pero **cada posición de píxel debe re-derivarse de las fuentes
  Java de Z4** (`Natives.java`/`ZenoniaUIControllerView.java` equivalentes vía jadx), nunca copiar
  coordenadas de Z3 a ciegas — además hay una pantalla nueva completa que agregar (selección de slot).
- `loader/main.c` — pipeline de arranque igual en estructura (load→relocate→resolve→patch→init→loop),
  pero el **orden exacto de las llamadas `NativeInitWithBufferSize`/`NativeInitDeviceInfo`/
  `handleCletEvent`** debe reconfirmarse leyendo `out_ghidra.c` de Z4 — Z3 encontró experimentalmente que
  invertir ese orden crashea (dependencia de heap custom `Gcx_MM_Init`); no hay garantía de que la firma
  de 4 argumentos de `handleCletEvent` sea igual en Z4.
- `manage_vita.py`, `porting_tools/build/*.sh` — mismos scripts, actualizar `VITA_IP`, nombres de VPK,
  paths (`zenonia-4-1-2-3` en vez de `zenonia3`/`dh2`).

### Trabajo genuinamente nuevo

- Stubs de In-App Billing, Tapjoy real (a diferencia de Z3), licensing (`NativeGetPublicKey`), timer
  asíncrono, red/Gamevil Live — todos siguiendo la guía de `jni_stubs.md` de la skill `psvita-porting`
  (cuándo un no-op simple alcanza y cuándo hace falta disparar un callback de "completado").
- Pantalla de selección de slot de partida en el sistema Android-UI-overlay.
- Confirmar/decidir soporte de idioma coreano (ver §6).

## 4. Bugs de motor con alta probabilidad de reaparecer (mismo motor, no del contenido del juego)

Extraídos de los ~13 bugs documentados en `port_progress.md` de Zenonia 2 y Zenonia 3. Presupuestar
tiempo de diagnóstico para estos aunque el síntoma inicial parezca distinto:

1. **Mutex/condvar "cero" al estilo Bionic**: `PTHREAD_MUTEX_INITIALIZER`/`PTHREAD_COND_INITIALIZER` en
   ceros es válido en Bionic pero VitaSDK lo desreferencia como puntero `NULL` real → crash dentro de
   `libstdc++` (allocator de nodos), no en código del juego. Requiere wrappers de
   `pthread_mutex_lock/unlock/destroy`/`pthread_cond_wait/broadcast` con inicialización on-demand. Es un
   bug del runtime C++ viejo, no de Zenonia específicamente — altísima probabilidad de reaparecer.
2. **`__cxa_atexit`/`__cxa_finalize`/`__aeabi_atexit` sin importar** → registrar como no-ops.
3. **Comparación de puntero de heap con signo** (`CMvLayerData::PreLoad` en Z2, puede estar en cualquier
   función del motor viejo): asume heap Android bajo/positivo, Vita usa heap alto que lee como negativo.
   No reusar el offset exacto — re-derivar con `vita-parse-core`+Ghidra sobre el binario específico de Z4.
4. **Método JNI `int` no registrado devuelve `-1`**, interpretado por el motor como valor "válido" en
   vez de "no implementado" — clase de bug más cara de diagnosticar (puede crashear varios saltos
   después de la llamada, ver el caso `CheckPhoneNumber`→`CMvApp::EvAppStart()` de Z3).
5. **`readAssets` con dos grafías reales** (`readAssets`/`readAssete`, typo confirmado en binario) y
   **dos consumidores JNI distintos** (acceso crudo a `ArrayObject` de Dalvik vs. `GetByteArrayElements`
   real) — hay que servir ambos formatos si ambos call sites existen en Z4.
6. **`struct stat` bionic vs. newlib** — necesario en cuanto el motor lea/escriba partidas guardadas.
7. Patrón de **hook de instancia (zerar un campo de estado en una instancia ya construida) preferido
   sobre parchear una función compartida** para ocultar UI — evita romper otros usuarios del mismo
   código compartido.

## 5. Fases de trabajo (adaptadas del `PORTING_GUIDE.md` de la skill, partiendo de código existente)

0. **Toolchain** — igual que Z2/Z3: vitasdk-softfp, kubridge, `libshacccg.suprx` en el device de prueba.
1. **Análisis estático / diff de ABI Z3 vs Z4** — completar la tabla de §2 con Ghidra
   (`decompile_all.sh` adaptado) + jadx del `classes.dex` de Z4; confirmar firmas exactas de los
   símbolos ya identificados antes de escribir una sola línea de `main.c`.
2. **Scaffold del repo** — copiar `loader/`, `common/`, `lib/` desde `Zenonia3-vita/`, renombrar
   referencias (`zenonia3`→`zenonia4`), adaptar `CMakeLists.txt`/`build.sh`; **no** partir de `source/`.
3. **Carga del `.so`** — `so_file_load`/`so_relocate`/`so_resolve` contra `libgameDSO.so` de Z4 con la
   import table heredada de Z3 como punto de partida; primer log limpio hasta el primer import faltante.
4. **Tabla JNI (FalsoJNI)** — portar métodos conocidos, agregar los ~15 nuevos de §2 como stubs seguros
   (confirmando caso por caso si un no-op alcanza), verificar que ninguno bloquea esperando un callback
   que el loader nunca dispara.
5. **Orden de init + gráficos** — vitaGL, confirmar el orden real de `NativeInitWithBufferSize`/
   `NativeInitDeviceInfo`/`handleCletEvent` contra Ghidra de Z4 (no asumir el de Z3), resolución interna
   (960×544 vs. resolución fija tipo teléfono — repetir el experimento de Z3 §Fase1).
6. **Assets** — reusar el patrón `fopen_hook`/`readAssets` de Z2/Z3 (`.zt1`/`.pzx` como bytes opacos ya
   resuelto), empaquetar `assets/` de Z4 en `ux0_data/zenonia-4/`.
7. **Android-UI overlay** — reconstruir logo/título/menú/about/help/**selección de slot** desde las
   coordenadas reales de las fuentes Java de Z4 (patrón de `androidui.c` reusado, contenido nuevo).
8. **Audio** — reusar `audio.c` (Tremor + mezclador), remapear tabla de IDs de sonido a los assets de Z4.
9. **Input** — reusar patrón de touch/hide-dpad-hook, re-verificar símbolos objetivo del hook en el
   binario de Z4 (pueden no llamarse igual).
10. **Debug en hardware real** — metodología `so-crash-triage`: log + `.psp2dmp` + `objdump`/Ghidra +
    jadx, un bug confirmado a la vez, sin parches especulativos en batch.
11. **Empaquetado VPK** — `extras/livearea/` ya tiene el arte listo; adaptar `sce_sys`/`vita_create_vpk`.
12. **Pase de regresión / backlog** — repetir el patrón de "Backlog" de Z2/Z3: anotar deuda técnica
    explícitamente en vez de dejarla implícita.

## 6. Preguntas abiertas a resolver temprano (antes de invertir tiempo en implementación)

- ¿Z4 necesita soporte de coreano? Hay botones `B_kr*.png`/`Text_kr*.png` en `assets/` pero no se
  encontró un directorio de locale `kor` explícito (sí hay `chi/deu/eng/fra/jpn/tai`) — confirmar si el
  coreano se sirve desde otro lado o si esos botones son vestigiales/deshabilitados en este build.
- ¿El sistema "Gamevil Live" (`live.html`, `live_auth_terms.html`, auth/circle policy) es necesario para
  jugar offline, o se puede neutralizar por completo (mostrar directo el menú principal)? Definir esto
  temprano evita construir UI para un flujo que se puede saltar.
- ¿`NativeGetPublicKey`/IAB bloquean el arranque si no hay respuesta de Google Play, o el motor tiene un
  camino "modo offline/sin compra" gracioso? Confirmar con Ghidra antes de decidir la forma del stub.
- Confirmar si los símbolos C++ de dibujo nuevos (`DrawNativeFillEllipse`, etc.) requieren shims GLES1
  adicionales o son autocontenidos.

## 7. Referencias

- `Zenonia2-vita/port_progress.md` y `Zenonia3-vita/port_progress.md` — bitácoras completas de bugs
  reales resueltos en consola física para este motor.
- `Zenonia3-vita/plan_zenonia3_port.md` — precedente exacto de este documento (diff de ABI Z2→Z3).
- `psvita-port-toolkit/PORTING_GUIDE.md` y la skill `psvita-porting`/`so-crash-triage` — metodología
  general (nota: escrita pensando en cocos2d-x; Zenonia no usa cocos2d-x, tratar como guía de fases, no
  de nombres de símbolos).
- `port_progress.md` (este repo) — bitácora específica de Zenonia 4, se actualiza a medida que se
  confirman cosas en código/hardware real.
