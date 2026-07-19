# Registro de Progreso del Port de Zenonia 4 (PS Vita)

Este juego usa el mismo motor que [`../Zenonia2-vita`](../Zenonia2-vita) y
[`../Zenonia3-vita`](../Zenonia3-vita) (Gamevil Nexus2/"Clet") — ambos proyectos ya tienen ports
funcionales en consola real y sus respectivos `port_progress.md` documentan en conjunto ~13+ bugs de
motor reales resueltos ahí, la mayoría con alta probabilidad de reaparecer acá por ser del runtime/motor
y no del contenido específico de cada juego (ver `plan_zenonia4_port.md` §4 para la lista consolidada).
Este documento solo registra lo que es **específico de Zenonia 4** — no repite la narrativa completa de
los otros dos.

Antes de tocar código, leer `plan_zenonia4_port.md` completo (diff de ABI Z3 vs. Z4, plan de
reutilización archivo por archivo, preguntas abiertas).

## Fase 0: Planificación — Completada (2026-07-16)

Sin escribir código todavía. Trabajo realizado:

- Confirmado por comparación de símbolos (`nm -D`) que `libgameDSO.so` de Zenonia 4 es el mismo motor
  Nexus2/Clet que Zenonia 2/3 (mismo paquete `com.gamevil.nexus2.Natives`, mismos prefijos internos
  `CGs*`/`CMv*`/`CZn*`, resolución JNI 100% por convención de nombre).
- Comparado el export table de Z4 (~35 símbolos `Java_com_gamevil_nexus2_Natives_*`) contra lo
  documentado para Z3: ~10 métodos ya existían en Z3 (reutilizables con verificación de firma), ~15 son
  nuevos (In-App Billing, Tapjoy real — a diferencia de Z3 donde el símbolo no existía en el binario—,
  licensing/`NativeGetPublicKey`, timer asíncrono, selección de slot de partida, estado de UI/menú
  extendido). Tabla completa en `plan_zenonia4_port.md` §2.
- Revisado el estado actual del repo: existen dos posibles puntos de partida (`source/`+`extras/`,
  scaffold genérico de soloader sin trabajo real; y el linaje `loader/` de Zenonia2→Zenonia3, específico
  del motor). **Decisión: forkear `Zenonia3-vita/loader/` como base** — ver justificación en
  `plan_zenonia4_port.md` §0.
- Descartada una preocupación del `CLAUDE.md` original de este repo: los formatos propietarios `.zt1`/
  `.pzx` **no son nuevos**, ya se manejan sin problema en Z2/Z3 como bytes opacos vía `readAssets`/
  `fopen_hook` — no requieren un parser propio en el loader.
- Confirmado que `extras/livearea/` ya tiene el arte de LiveArea listo (`icon0.png`, `bg0.png`,
  `pic0.png`, `startup.png`, `template.xml`) — no hace falta rehacer esa parte del empaquetado VPK.
- Preguntas abiertas identificadas para resolver en Fase 1 (análisis estático): soporte de coreano,
  si el sistema "Gamevil Live"/auth es necesario para jugar offline, si `NativeGetPublicKey`/IAB
  bloquean el arranque sin respuesta de Google Play. Ver `plan_zenonia4_port.md` §6.

## Fase 1: Análisis estático completo (jadx + Ghidra) — Completada (2026-07-16)

`porting_tools/build/decompile_all.sh` corrió sin adaptar paths (ya apuntaba a este repo) y generó
`decompiled/apk_jadx/` (jadx del APK completo, 4 clases con error de decompilación, no bloqueante) y
`decompiled/libgameDSO_armeabi/ghidra/out_ghidra.c` (7MB, Ghidra headless vía `devrvk/so-decompiler`).

Hallazgos clave:

- **`com/gamevil/nexus2/Natives.java` (jadx) es la fuente de verdad completa** de los ~35 métodos
  `native` (los que el loader debe resolver del `.so` y llamar) y de ~60 métodos "callback" que el
  motor invoca de vuelta hacia "Java" (los que hay que registrar en `java.c`/FalsoJNI). Confirmado con
  `strings libgameDSO.so` que casi todos los candidatos identificados en el plan realmente aparecen en
  el binario (o sea, sí se llaman en algún call site real), salvo un puñado (`finishApp`,
  `vibrateAndroid`, `hideShopButton`, `hidePurchaseComponent`, `showPurchaseComponent`,
  `HideLoadingDialog`, `showLoadingDialogIAB`, `hideLoadingDialogIAB`) que no matchearon — quedan sin
  registrar hasta que un log real muestre que hacen falta.
- **`isTapjoyReady`/`nativeTapjoyAward`/`SetTJReady` son métodos `native` (implementados DENTRO del
  `.so`)**, no callbacks que el loader deba implementar — corrige una hipótesis del plan original. El
  loader nunca los llama (no hay Tapjoy Java real), así que son día muerto sin trabajo extra.
- **Ghidra confirma el signature/orden de `NativeInitWithBufferSize`/`NativeInitDeviceInfo`**:
  `NativeInitWithBufferSize(w,h)` es quien dispara `startClet()` y pone `bCletStarted=1` (del que
  depende `handleCletEvent`) — se decidió mantener el mismo orden que Zenonia 3 (WithBufferSize antes
  que DeviceInfo) como precaución, sin encontrar evidencia que lo contradiga en este binario.
  `handleCletEvent` mantiene la firma de 4 ints confirmada en Zenonia 3.
- **Riesgo nuevo identificado, no resuelto todavía**: `NativeInitWithBufferSize` llama internamente a
  una función `getGL()` que hace una cadena de reflexión JNI genérica (FindClass/GetMethodID/
  CallObjectMethod sobre `PackageManager`/`PackageInfo`/`Signature`, probablemente un chequeo de firma
  de APK/anti-tamper) la PRIMERA vez que se llama. FalsoJNI es defensivo por diseño (`FindClass` nunca
  devuelve NULL, `CallObjectMethod` no encontrado devuelve default sin abortar), así que no se espera
  que crashee, pero no se implementó ningún soporte específico para esta cadena — es el primer
  candidato a revisar si el primer log real muestra un crash temprano dentro de `NativeInitWithBufferSize`.
- Confirmado (comparando `nm -D` de ambos `.so`): solo 8 símbolos de import son nuevos respecto a
  Zenonia 3 (`__assert2`, `__sF`, `dlopen`, `dlsym`, `dlclose`, `fprintf`, `strtok`,
  `pthread_key_delete`) sobre 95 totales — la tabla de imports de Zenonia 3 cubre el resto sin cambios.

## Fase 2: Scaffold del repo — Completada (2026-07-16)

Copiado el linaje completo de `Zenonia3-vita/loader/` (`so_util.c/.h`, `dynlib.c`, `java.c`, `main.c`,
`audio.c/.h`, `font.c/.h`, `htmltext.c/.h`, `htmlview.c/.h`, `androidui.c/.h`, `ksc5601_table.h`),
`common/debugScreen.*`, `lib/falso_jni/`, `lib/stb/`, `clean_macos.sh`, `manage_vita.py`, `font.ttf`.
`CMakeLists.txt` y `build.sh` escritos desde cero siguiendo la estructura de Zenonia 3 (title ID
`PSVZ00004`, VPK `zenonia_4.vpk`). Arte de LiveArea tomado de `extras/livearea/` (ya existía, específico
de Zenonia 4) en vez del de Zenonia 3.

El scaffold genérico que ya vivía en este repo (`source/`, `CMakeLists.txt` original apuntando a
`source/main.c`/`lib/so_util`/etc.) quedó reemplazado — no se llegó a usar ninguna línea de ese código.
Queda sin borrar por si se quiere revisar/rescatar algo puntual, pero no forma parte del proyecto activo.

## Fase 3: dynlib.c — Completada (2026-07-16)

Import list re-derivada con `objdump -T libgameDSO.so | grep UND` (95 símbolos) y comparada 1:1 contra
la tabla heredada de Zenonia 3: solo 8 símbolos nuevos (ver Fase 1). Agregados con shims simples
(`__assert2` loguea y aborta, `__sF` es un array de 3 `FILE*` inicializado en `dynlib_init()`,
`dlopen`/`dlsym`/`dlclose` devuelven "no soportado" ya que no se confirmó ninguna ruta de arranque que
dependa de que tengan éxito, `fprintf`/`strtok`/`pthread_key_delete` son libc estándar que solo hacía
falta declarar). `translate_path()` actualizado a `ux0:data/zenonia-4/assets/`.

## Fase 4: java.c (tabla JNI) — Completada (2026-07-16)

Agregados ~50 métodos nuevos (ids 47-96) sobre la base heredada de Zenonia 3 (que ya traía el puente GFA
de fuentes completo, `readAssets`/`readAssete`, `isAssetExist`, sonido, y los 4 getters de
teléfono/SIM/MAC/deviceID). Categorías nuevas: getters de dispositivo/red/versión (valores plausibles no
vacíos, mismo criterio que Zenonia 2/3), Gamevil Live (reporta "no logueado" consistentemente, sin
backend real), guardado/carga de partida (`isFileExist`/`saveFile`/`loadFile`/`deleteFile` contra
`ux0:data/zenonia-4/saves/*.sav`, **sin confirmar todavía** que el formato del `jbyteArray` que arma el
motor coincide con la convención "Dalvik ArrayObject" asumida), IAB/Tapjoy/analytics (no-ops explícitos,
no bloquean nada por diseño).

**Limitación conocida sin resolver:** `OnAsyncTimerSet(int,int)` y `OnAsyncTimerSet(int,int,int)` son dos
sobrecargas reales en Java, pero FalsoJNI resuelve `GetMethodID` solo por nombre (ignora firma) — ambas
colisionan en un único handler que asume 2 argumentos. Si el motor realmente usa la variante de 3
argumentos por este mismo camino, se va a desalinear el `va_list`. Sin confirmar en consola todavía.

## Fase 5: main.c — Completada (2026-07-16)

Pipeline de arranque igual al de Zenonia 3 (load→relocate→resolve→parche→init→loop). Cambios de
contenido:

- Variable `zenonia3_mod` renombrada a `zenonia4_mod` en todo el archivo (cosmético, sin efecto).
- **Parche binario de `CMvLayerData::PreLoad` mantenido con el offset de Zenonia 3 (`0x9a7b4`) pero con
  chequeo de bytes activo** (ya existía en el código heredado) — como se esperaba, el offset de Zenonia 3
  NO es el correcto para este binario (tamaños de `.so` distintos: 1.63MB vs 1.28MB), así que el patch se
  saltea con un log explícito en vez de aplicarse mal. Pendiente re-derivar el offset real de Zenonia 4
  con Ghidra/`vita-parse-core` en cuanto aparezca el síntoma (comparación de heap con signo) en un log
  real — no antes, siguiendo el criterio de "un bug confirmado a la vez".
- **Decisión de resolución interna documentada como no confirmada**: a diferencia de Zenonia 3 (400x240
  fijo, triple-confirmado), el Java decompilado de Zenonia 4 (`NexusGLActivity.java:73-235`) pisa
  `gameScreenWidth/Height` con la resolución REAL del dispositivo Android en `onCreate()`. Se decidió
  pasar 960x544 (nativo de Vita) consistentemente a `NativeInitDeviceInfo`/`NativeInitWithBufferSize`/
  `NativeResize` para evitar el bug de "cuadrante" que Zenonia 3 sufrió por pasar valores distintos a
  Init vs. Resize — sin validar en consola todavía.

## Fase 6: Assets para `ux0:data/zenonia-4` — Completada (2026-07-16)

`ux0_data/zenonia-4/assets` creado como symlink a `../../zenonia-4-1-2-3/assets` (287MB, se evita
duplicar en disco) — confirma la indicación del usuario de que esa carpeta es la que va en el "Data" del
Vita. `ux0_data/zenonia-4/sound/` poblado con symlinks a los 109 `.ogg` que ya vivían en
`zenonia-4-1-2-3/res/raw/s*.ogg` con la nomenclatura exacta (`sNNN.ogg`) que `audio.c` espera —
confirma que el pipeline de audio de Zenonia 3 es reutilizable sin cambios de convención. `saves/` y
`logs/` no se pre-crean: `sceIoMkdir`/`fopen` los crean solos en el primer uso.

## Fase 7: Primer build + VPK — Build completado, deploy pendiente (2026-07-16)

`./build.sh` compiló **sin errores en el primer intento** (solo warnings esperables, mismos que en
Zenonia 3) y generó `build/zenonia_4.vpk` (1.2MB). `manage_vita.py` actualizado (`LOCAL_VPK_PATH`,
`VITA_LOGS_DIR` -> zenonia-4; tenía restos de un template anterior, "popclassic", sin limpiar).

**Bloqueado en la prueba de hardware real durante esta sesión**: la consola en `192.168.3.15` no
respondió durante la sesión de trabajo — resuelto poco después: el usuario probó el VPK en consola real
por su cuenta y compartió el primer log (`logs/log_1784242833.txt`).

## Fase 7.1: Primer log real en consola — ¡ARRANCA! (2026-07-16)

**El loader llega al bucle principal sin crashear.** Secuencia completa observada en el log:

1. `so_file_load`/`so_relocate`/`so_resolve` OK (`text_base=0x98000000`, `num_dynsym=8716`).
2. Parche de `CMvLayerData::PreLoad` se saltea con el log esperado (offset de Zenonia 3 no coincide,
   ver Fase 5) — comportamiento correcto, no bloquea nada.
3. vitaGL, mezclador de audio, `androidui`/`splash`/`htmlview` todos reportan "no encontrado" para sus
   `.rgba`/html (esperado: esos assets Zenonia-4-específicos todavía no se generaron, Fase de Android-UI
   overlay diferida — ver plan). Fuente GFA (`font.ttf`) carga bien.
4. `NativeInitWithBufferSize(960,544)` corre, dispara `startClet()` y **la cadena de reflexión JNI de
   `getGL()` (el riesgo identificado en Fase 1) se confirma inofensiva en la práctica**: `GetMethodID`
   para `getPackageManager`/`getPackageName`/`getPackageInfo`/`hashCode` fallan con `[JNI ERR]` (no
   encontrados), `GetArrayLength`/`GetObjectArrayElement` sobre el objeto sentinel `0x42424242` fallan
   también, pero FalsoJNI degrada con seguridad en cada paso — cero crash. El motor toma la rama
   "firma no coincide" y llama a `updateDialogue()`, que ya estaba registrado como no-op.
5. Decenas de `readAssets`/`isAssetExist` sobre `.zt1`/`.pzx`/`.mpl` reales — **todos exitosos**, confirma
   que `.zt1`/`.pzx` se sirven sin problema como bytes opacos (correccion de Fase 1 al `CLAUDE.md`
   original, confirmada en consola real).
6. `NativeInitDeviceInfo(960,544)`, `NativeResize(960,544)` — viewport y ortho quedan en
   `left=-480 right=480 bottom=-272 top=272`, consistente con pasar 960x544 a los tres `Native*Init`
   (decisión de Fase 5).
7. `NativeResumeClet()`, `OnUIStatusChange: 104` (estado de UI nuevo, no visto en Zenonia 2/3 —
   pendiente de mapear contra las fuentes Java de Zenonia 4 cuando se aborde Android-UI overlay).
8. **`Iniciando Bucle Principal...` / `frame 1 alive, touch.reportNum=0 pad.buttons=0x00000000
   ui_status=104`** — el loop principal corre.

**3 bugs reales encontrados y corregidos a partir de este log** (mismo criterio de "un bug confirmado a
la vez"):

- `GFA_GetTextSize` no estaba registrado en `java.c` (`[JNI ERR][GetStaticMethodID] ... "GFA_GetTextSize",
  "()F": not found`) — nuevo en Zenonia 4, no existía en la tabla heredada de Zenonia 3. Agregado como
  id 97 (`METHOD_TYPE_FLOAT`, primer uso de `MethodsFloat` en este archivo), devuelve `g_gfa_text_size`.
- **Bug real en `saveFile`/`loadFile`/`isFileExist`/`deleteFile`**: el motor ya pasa el nombre completo
  con su propia extensión (`option.sav`, `Save0.dat`, `SaveFlag.sav`, confirmado en el log vía
  `access_hook`), pero `zenonia_save_path()` le agregaba un `.sav` extra (`option.sav.sav`) — esos
  archivos nunca iban a poder abrirse para lectura/escritura. Corregido: usar el nombre tal cual llega.
- El directorio `ux0:data/zenonia-4/saves/` no se creaba nunca (`fopen` no crea directorios) — agregado
  `sceIoMkdir()` lazy (una sola vez) dentro de `zenonia_save_path()`, mismo patrón que `init_log()` en
  `main.c`.

**Pendientes/hallazgos menores sin resolver todavía (no bloquean el arranque):**

- `[AUDIO] no encontrado: ux0:data/zenonia-4/sound/s000.ogg` — el sonido no sonó en esta prueba. Muy
  probable que sea un problema de **deploy** (la carpeta `sound/` de `ux0_data/zenonia-4/` no se haya
  copiado a la consola junto con `assets/`), no de código — confirmar que `ux0:data/zenonia-4/sound/`
  existe en la consola con los `sNNN.ogg` antes de investigar el loader.
- `[JNI ERR][GetByteArrayElements] GetByteArrayElements(env, 0x0, 0x0): Could not find the array` —
  aparece dos veces (una antes de `GFA_Init`, otra después de los chequeos de `option.sav`/`Save*.dat`).
  No crashea, pero no se identificó todavía qué call site del `.so` la dispara ni si es señal de un
  problema real — investigar con Ghidra si reaparece de forma consistente.
- `OnUIStatusChange: 104` — número de estado de UI nuevo respecto a Zenonia 2/3, sin mapear todavía
  contra las fuentes Java de Zenonia 4 (relevante quzá para cuando se implemente el Android-UI overlay).

**Próximo paso real**: con este log tan limpio, el próximo ciclo debería enfocarse en (a) confirmar el
fix de deploy de `sound/`, (b) generar los assets de Android-UI (`androidui/*.rgba`,
`about.html`/`help_eng.html`) específicos de Zenonia 4 para que el overlay de menú/logo deje de estar en
blanco, y (c) seguir jugando manualmente en consola para encontrar el próximo crash real (personaje
nuevo, entrar a un mapa, etc.) siguiendo `so-crash-triage`.

## Fase 7.2: "Abre y se cierra solo" — causa real encontrada y corregida (2026-07-16)

Segundo log real (`logs/log_1784243728.txt`): el usuario reportó que el VPK abría y se cerraba solo, sin
crash visible, y señaló dos cosas para revisar — (1) los warnings de `androidui`/`ui_logo_gamevil` y (2)
que `zenonia-4-1-2-3/res/drawable` no tiene los mismos nombres de archivo que Zenonia 3 (ese título de
`androidui.c` es literalmente el de Zenonia 3, heredado sin adaptar). Revisión completa del log:

**Causa raíz del cierre (confirmada cruzando el log con `out_ghidra.c`, no es un crash):**

1. `getGL()` (dentro de `NativeInitWithBufferSize`, primera vez) llama `componentID(1)` →
   `isGreeting = 1` (siempre true) y `greetingHello = 0` (porque nuestro stub de `getCompany()` devuelve
   0, un valor que no matchea ninguna de las 4 ramas de mercado que reconoce `componentID()` en
   `out_ghidra.c`).
2. `getGL()` sigue con una cadena de reflexión JNI (`PackageManager`/`PackageInfo`/`Signature`) para leer
   la firma real del APK y le calcula `hashCode()`. **`hashCode` no estaba registrado** → FalsoJNI
   devuelve el default de un método `int` no encontrado (**-1**, mismo patrón que el bug de
   `isAssetExist` sin registrar documentado en Zenonia 2 — ver `psvita-porting/jni_stubs.md`).
3. Como `-1 != greetingHello(=0)` y `isGreeting` es true, el motor pone `nReturn = 1`.
4. `NativeResumeClet()` llama a `changeUI()`, que hace
   `if (nReturn == 1) changeUIStatus(0x68 /* UI_STATUS_EXIT = 104, confirmado en
   SkeletonUIControllerView.java */)`.
5. El bucle principal ve `g_ui_status == UI_STATUS_EXIT` en su primerísima iteración y corta (`break`) —
   de ahí "frame 1 alive ui_status=104" como única línea del loop antes de cerrarse solo, sin ningún
   `[FATAL]` ni crash real.

**Fix aplicado**: registrar `hashCode` en `java.c` (id 98) devolviendo **0** (el mismo valor por default
de `greetingHello`), para que la comparación sea `0 != 0` (false) y el motor nunca fuerce la salida. Es
un chequeo de firma de APK sin sentido en este entorno (no hay Dalvik/PackageManager real) — documentado
con la nota completa junto a `Zenonia_hashCode()`, incluyendo el riesgo de que `hashCode` se resuelve por
nombre (no por clase) en FalsoJNI, así que cualquier otro call site real que llame a `hashCode()`
esperando un valor con sentido recibiría este mismo 0 (no se detectó ninguno más en este binario).
Rebuildeado y VPK regenerado.

**Sobre el punto de `res/drawable` que señaló el usuario — confirmado, y con una explicación mejor de lo
esperado**: `zenonia-4-1-2-3/res/drawable` en efecto NO tiene los mismos nombres de Zenonia 3
(`ui_logo_gamevil.png`, `ui_menu_back0.png`, etc. no existen) — tiene nombres nuevos totalmente distintos
(`auth_bg.png`, `gamevil_live.png`, `buy_button.png`, `gold_150000.png`, etc.), que corresponden a las
pantallas NUEVAS de Zenonia 4 (login de Gamevil Live, tienda de IAP), no al logo/título/menú principal.
La razón: **el log muestra que Zenonia 4 carga `menu/TeamLogo.pzx`, `menu/ZeroGrade.pzx`,
`menu/Title_001..005.pzx` y `menu/logo.pzx` vía `readAssets` exitosamente** — son los gráficos de
logo/título, y a diferencia de Zenonia 2/3 (donde esas pantallas eran `ImageView`s de Android puro,
invisibles sin `androidui.c`), todo indica que Zenonia 4 los dibuja **nativamente** desde el propio
`.so` (via el pipeline `CGsGraphics`/`DrawNativeFillEllipse`/`PutNativeImage` confirmado en Fase 1) —
mismo patrón de assets `.pzx` que el resto del juego, no un overlay Java. Esto significa que
`androidui.c` (heredado literal de Zenonia 3, con sus nombres de archivo `.rgba` hardcodeados) muy
probablemente **no hace falta para el logo/título/menú principal de Zenonia 4** — sus warnings de
"no encontrado" son ruido esperado sobre archivos que no van a existir nunca con esos nombres, no un
problema real. Queda pendiente confirmar visualmente en consola (con el fix del punto anterior ya
aplicado, el juego debería llegar más lejos y mostrar si el logo/título se ve bien solo con el
renderizado nativo) antes de decidir si vale la pena adaptar `androidui.c` para las pantallas
Gamevil Live/IAP más adelante, usando los nombres reales de `res/drawable`.

**Próximo paso real**: reinstalar el VPK regenerado y volver a jugar — con el fix de `hashCode` debería
llegar mucho más lejos que "frame 1". Confirmar visualmente si el logo/título se ve correctamente (dado
que parecen dibujarse nativamente) antes de invertir tiempo en adaptar `androidui.c`.

## Backlog / deuda técnica conocida (heredada de Z2/Z3, aplica por ser el mismo motor)

- Shader de post-proceso experimental (Z2) da pantalla negra — no priorizar mejoras cosméticas antes de
  cerrar bugs funcionales, mismo criterio aplica acá.
- Texto inline multicolor en una misma línea (ABOUT/HELP de Z3) no soportado — si Z4 tiene el mismo
  patrón de UI, mismo límite aplica salvo que se invierta tiempo en extenderlo.
- Traducción KO/JA de HTML sin decoder EUC-KR/Shift-JIS confirmado en Z3 — relevante para la pregunta
  abierta de soporte de coreano en Z4 (§6 del plan).

## Fase 8: Assets de LiveArea reales — Completada (2026-07-16)

Reemplazados los placeholders genéricos:

- `sce_sys/icon0.png`: convertido desde `zenonia-4-1-2-3/res/drawable-xhdpi-v4/icon.png` (96×96 RGBA) a
  128×128 indexado de 8 bits (`sips` para el resize + `pngquant --force 256` para indexar) — formato
  exacto que exige LiveArea (ver `psvita-porting/references/vpk_packaging.md`, error `0x8010113D` si no
  es PNG indexado).
- `sce_sys/livearea/contents/bg0.png` (840×500) y `sce_sys/pic0.png`/`startup.png` (960×544 / 280×158):
  generados desde dos imágenes provistas por el usuario (Gemini), con resize "cover" + crop centrado
  (`sips -z` + `sips -c`) para no distorsionar el aspecto, y la misma cuantización a 8-bit indexado.

## Fase 9: "Juega pero muy lento" — causa real encontrada y corregida (2026-07-16)

Tercer log real (`logs/log_1784247532.txt`, 2965 líneas): el usuario reportó que ya se podía jugar pero
a una velocidad no jugable. Análisis por frecuencia de líneas normalizadas + comparación de rango de
líneas de log contra el contador de `frame N alive` (que solo loguea 1 de cada 120 frames) ubicó un pico
brutal: **912 líneas de log en una sola ventana de 120 frames**, exactamente en `ui_status=14`
(`UI_STATUS_FULLTOUCH`, juego real) durante la pantalla de selección/preview de personaje.

**Causa raíz confirmada**: la secuencia completa de ~11 archivos del personaje (`dynamic/
CharaterSelectUI.pzx`, `pc/000.pza`, `pc/head|armor|weapon|side_hand|effect/0/000.pzd` +
`pc/head|armor|weapon|side_hand|effect/000.pzf`) se repetía **idéntica, decenas de veces por segundo**
en el log — el motor vuelve a pedir el mismo set de archivos en cada actualización del preview (probable
animación idle del personaje), y **el loader no tenía ningún cache**: cada `readAssets` hacía una
lectura de disco real completa (`fopen`/`fstat`/`fread`/`fclose` contra `ux0:`) más 3-4 líneas de log con
`fflush` cada una. En Android esto es barato porque `AssetManager` puede mapear el asset directo del
APK; acá cada llamada repetía el trabajo completo contra el almacenamiento del Vita, mucho más lento.

**Fix aplicado en `java.c`**:

- Cache en memoria de los bytes crudos de cada asset leído (`g_asset_cache[]`, hasta 128 entradas, FIFO
  simple sin LRU real — el working set observado, ~11 archivos de un personaje, es chico). En cache hit,
  `Zenonia_readAssets` arma un `array_obj` nuevo copiando desde memoria, **sin tocar disco**. Importante:
  se cachean los bytes crudos, NO el `array_obj` con el header Dalvik-style ya armado — el motor libera
  (`free()`) ese puntero después de usarlo (`DeleteGlobalRef`, ver `FalsoJNI.c`), así que devolver el
  mismo puntero dos veces sería un use-after-free/double-free. Cada llamada arma un `array_obj` fresco.
  `Zenonia_isAssetExist` también consulta el cache para el tamaño, evitando un `stat()` extra en cache hit.
- Logging de `readAssets`/`isAssetExist` limitado a las primeras 200 líneas cada uno (mismo criterio ya
  usado en `dynlib.c` para los logs de GL) — con cache, la mayoría de las llamadas en juego real son hits
  repetidos sin valor de diagnóstico nuevo, y cada línea de log implica un `fflush()` bloqueante.
- No afecta el guardado de partida: `saveFile`/`loadFile` usan `zenonia_save_path()`, un código
  completamente separado de este cache (que solo cubre `assets/`, contenido estático de solo lectura).

Rebuildeado sin warnings nuevos, VPK regenerado. **Pendiente confirmar en consola** si esto resuelve la
lentitud reportada — si sigue lenta después de este fix, el próximo sospechoso a investigar sería la
conversión CPU de píxeles RGB565→RGBA8888 en `glTexImage2D_wrapper`/`glTexSubImage2D_wrapper`
(`dynlib.c`) para el framebuffer de software del motor, que corre sin cache posible (los píxeles cambian
cada frame por diseño).

## Fase 10: cuarto log — cache confirmado, pero sigue lenta + congelamiento nuevo (2026-07-16)

Cuarto log real (`logs/log_1784249393.txt`, 928 líneas): el usuario reportó que **sigue extremadamente
lenta** y que además **se congeló la pantalla al intentar guardar**.

**Confirmado que el cache de Fase 9 funciona**: de 200 llamadas a `readAssets`, 148 fueron cache hits
(74%) — ya no aparece el patrón de re-lectura idéntica y repetida de Fase 9. La lentitud remanente
reportada por el usuario probablemente ya no es por I/O de disco redundante; sospechosos pendientes de
confirmar: latencia real de disco en los cache *misses* (52 archivos únicos, primera lectura siempre
paga el costo real de `ux0:`), o costo de render/conversión de píxeles por frame (sin cache posible).

**El congelamiento es un problema DISTINTO y más serio, todavía sin diagnosticar con certeza** — el log
no tiene ninguna firma de crash (sin `[FATAL]`, sin corte abrupto tras un error): simplemente deja de
haber líneas nuevas a partir de la línea 928 (`[AUDIO] no encontrado: .../s095.ogg`), consistente con un
**hang real** (el proceso sigue vivo pero nunca vuelve a ejecutar código que loguee algo) más que con un
crash — y un hang no genera `.psp2dmp`, así que no hay volcado que cruzar con Ghidra como en un crash real.

Se investigaron y DESCARTARON (con evidencia del log, no por suposición) las hipótesis más obvias:

- **No es el guardado de partida real** (`Save0.dat`/`Save1.dat`/etc.): esos archivos solo aparecen con
  chequeos `access_hook` (probablemente `CGsSaveData` verificando qué slots existen), nunca llegan a
  `fopen_hook` en este log — o sea, el juego nunca llegó a escribir un save de personaje real antes de
  congelarse. Solo `option.sav`/`SaveFlag.sav` (datos chicos tipo configuración) pasaron por
  `isFileExist`/`loadFile`/`saveFile`, sin problema.
- **No parece relacionado a los cambios de esta sesión** (cache de assets): el último `readAssets` real
  quedó más de 350 líneas antes del corte, sin ninguna actividad de archivo cerca del punto de
  congelamiento.
- Los wrappers de `pthread_mutex`/`pthread_cond` (el bug de "mutex en cero estilo Bionic", muy probable
  de reaparecer según `plan_zenonia4_port.md` §4) ya estaban corregidos correctamente (heredados de
  Zenonia 3, con inicialización on-demand) — revisados y se ven bien, aunque no se puede descartar un
  100% un deadlock en un condvar que nunca recibe su `broadcast` sin más evidencia.
- El `.so` no importa `pthread_create`/`pthread_join` — no parece spawnear threads nativos propios, lo
  que descarta (parcialmente) un patrón de "esperar a un worker thread que nunca arranca".

**Diagnóstico pendiente**: sin una firma de crash ni un `.psp2dmp`, hace falta un log más detallado para
encontrar la última llamada JNI real antes de congelarse. Se generó un **VPK de diagnóstico** con
`ENABLE_VERBOSE_JNI_LOG=ON` (loguea CADA llamada JNI, no solo warnings/errores) en
`build/zenonia_4_debug_verbosejni.vpk` — mucho más lento en general (cada línea es un `fflush`
bloqueante), pero necesario una sola vez para capturar exactamente qué pasó justo antes del
congelamiento. **Pedir al usuario**: instalar ese VPK de diagnóstico, reproducir lo más parecido posible
la secuencia que llevó al "intentar guardar", y mandar el log resultante — con esas líneas finales
debería alcanzar para ubicar la función exacta en `out_ghidra.c` que se cuelga. Una vez identificado el
problema real, generar de nuevo el VPK normal (`build/zenonia_4.vpk`, sin verbose) con el fix.

## Fase 11: causa real de la lentitud (y probablemente del "congelamiento") — encontrada (2026-07-16)

Quinto log (`logs/log_1784251492.txt`, 8012 líneas, con el VPK de diagnóstico verbose de la Fase 10): el
usuario reportó que ahora tardaba mucho más en iniciar, ni siquiera llegó a ver el menú — esperado, dado
el aviso de que el log verbose hace todo mucho más lento (documentado en `CMakeLists.txt`: "con el nivel
0 el arranque tardaba minutos"). Pero el volumen de detalle permitió encontrar algo mucho más importante
que lo que se estaba buscando.

**Patrón encontrado**: cientos de ciclos idénticos de
`GetStaticMethodID("GFA_GetWordwrapPositionEx") → CallStaticIntMethodV → GetStaticMethodID("GFA_SetString") →
NewStringUTF(<mismo texto, un caracter menos cada vez>) → CallStaticVoidMethodV` — el motor estaba
armando un cuadro de diálogo largo (un aviso tipo "...from battery depletion. Please exit Zenonia 4
through normal means to avoid this critical error... (Main Menu>> Community>> Export data.)") y
consumiéndolo **UN CARÁCTER POR VEZ** en vez de una línea completa por vez. Con cientos de caracteres en
el mensaje y varias llamadas JNI + una `NewStringUTF` (alocación de string) por carácter, esto por sí
solo explica tanto la lentitud extrema como probablemente el "congelamiento" de la Fase 10 (una caja de
texto larga podía tardar minutos en terminar de armarse, indistinguible de un cuelgue real para el
usuario esperando).

**Causa**: `Zenonia_GFA_GetWordwrapPositionEx()` (`java.c`) delega en `gfa_break_text()` →
`gfa_font_break_text()` (métricas reales de fuente vía stb_truetype), que devolvía un corte de **1 solo
carácter** para este texto — significa que, con el `maxWidth`/tamaño de fuente en juego en este punto,
el ancho acumulado supera `maxWidth` apenas agregado el primer carácter. Causa raíz exacta (¿unidades de
`maxWidth` mal interpretadas? ¿`GFA_SetTextSize` con un valor pensado para otra escala?) **todavía no
confirmada con números reales** — el logger JNI genérico no imprime los valores de argumentos `float`.

**Fix aplicado (mitigación inmediata + diagnóstico para la causa exacta)**:

- Agregado un log de diagnóstico (una sola vez por sesión, disparado solo si se detecta el patrón
  sospechoso: corte de 0-2 caracteres para un texto de más de 10) que imprime `maxWidth`, `text_size`,
  `char_cnt` y si la fuente real (stb_truetype) está lista — la próxima vez que aparezca este cuadro de
  diálogo, el log va a tener los números reales para confirmar la causa exacta.
- **Piso de progreso mínimo** en `Zenonia_GFA_GetWordwrapPositionEx`: si el cálculo real devuelve menos
  de ~20 caracteres para un texto largo, se fuerza un mínimo razonable antes de seguir — garantiza que
  cualquier cuadro de texto termine de armarse en un puñado de llamadas en vez de cientos. Contrapartida
  conocida: mientras no se confirme y corrija la causa real, el wrap visual de textos largos puede
  quedar imperfecto (línea con más o menos texto del que entra visualmente) — priorizado no colgar el
  juego por sobre que el wrap se vea perfecto, dado que esto probablemente afecta CUALQUIER texto largo
  del juego (diálogos, descripciones de ítems), no solo este aviso puntual.

Rebuildeados `build/zenonia_4.vpk` (normal) y `build/zenonia_4_debug_verbosejni.vpk` (verbose,
actualizado con el mismo fix) sin warnings nuevos. **Próximo paso real**: probar de nuevo con el VPK
normal — debería ya no tardar tanto en los cuadros de texto largos. Si el log de diagnóstico llega a
aparecer de nuevo (poco probable ya con el piso puesto, pero posible en otro cuadro de texto con
parámetros distintos), esos números van a permitir corregir la causa real en vez de depender del piso.

## Fase 12: guardado corrompía ~96 MB — bug real confirmado y corregido (2026-07-16)

Sexto log (`logs/log_1784252535.txt`): el fix de wordwrap de la Fase 11 ayudó (juego llega a
`ui_status=14` mucho más rápido, sin los saltos enormes de líneas de log entre frames de las fases
anteriores), y el usuario logró crear un personaje y guardar — pero **la consola entera se congeló ~30
segundos** durante ese guardado.

**Causa confirmada en el log**: `[Java] saveFile: Save0.dat -> ux0:data/zenonia-4/saves/Save0.dat
(100866051 bytes)` — **~96 MB** para lo que debería ser un save de personaje de algunos KB. Exactamente
el riesgo que ya estaba anotado como pendiente de confirmar (ver Fase 4): `Zenonia_saveFile` asumía que
el motor arma el `jbyteArray` de datos con la misma convención "Dalvik ArrayObject" cruda que usa
`readAssets` (offset 8 = largo, offset 16 = datos) — **pero esa convención es específica de un consumidor
particular de `readAssets`, no universal**. El motor arma este array con una llamada JNI real
(`NewByteArray`), y `FalsoJNI.c` devuelve literalmente el puntero a su propio `JavaDynArray` (`{jarray
array; jsize len; FIELD_TYPE type;}`) como el `jbyteArray` — leer los offsets crudos leía basura como
"largo" (96 MB) y escribía esa cantidad de memoria adyacente a la estructura real, corrompiendo el save
y tardando ~30s en escribirlo a la tarjeta de memoria.

**Fix**: `Zenonia_saveFile` ahora lee el array como `JavaDynArray*` (`->array`, `->len`) en vez del
struct crudo. `Zenonia_loadFile` (que yo mismo había escrito por simetría con `readAssets`, sin
confirmar) se corrigió igual, devolviendo un `jbyteArray` armado con `jda_alloc()` (mismo mecanismo que
`zenonia_new_byte_array_str`) en vez del layout Dalvik crudo — consistente con la convención real
confirmada para este par de funciones.

**Acción pendiente para el usuario**: el `Save0.dat` que ya existe en la consola (`ux0:data/zenonia-4/
saves/Save0.dat`, ~96 MB) es basura del bug — conviene borrarlo o renombrarlo antes de probar de nuevo,
para que el próximo guardado se cree limpio con el fix ya aplicado.

## Fase 13: optimización de conversión de píxeles (RGB565→RGBA8888) (2026-07-16)

El usuario reportó que el juego "va a 1 frame por segundo, mejor que antes pero no la optimización
esperada" — con los bugs de wordwrap y de guardado ya corregidos, y sin saltos de log anómalos entre
frames de juego real en el log más reciente, el cuello de botella que queda ya no parece ser I/O ni
logging sino tiempo de render genuino por frame.

**Optimización aplicada** (sin esperar más logs, es una mejora de bajo riesgo y alto valor esperado):
`convert_rgb565_to_rgba8888()` en `dynlib.c` (sube el framebuffer de software del motor a una textura
GL, sin cache posible porque los píxeles cambian cada frame por diseño) hacía 3 divisiones por píxel
(`(p>>11 & 0x1F) * 255 / 31`, etc.) — a la resolución real de este port (960×544 ≈ 522k píxeles) son
~1.5 millones de divisiones por frame. Reemplazado por 2 tablas de lookup precalculadas (5 bits→8 bits,
6 bits→8 bits, 32 y 64 entradas) más un empaquetado directo a `uint32` en vez de 4 escrituras de byte
sueltas por píxel.

Rebuildeados ambos VPK sin warnings nuevos. **Pendiente confirmar en consola** si esto sube el
framerate de forma perceptible — si sigue muy lento, el próximo sospechoso sería el propio pipeline de
`glDrawFrame`/vitaGL a esta resolución, o el conversor de vértices `GL_FIXED`→float (`glDrawArrays_wrapper`),
que corre por cada `glDrawArrays` con vértices en punto fijo.

## Fase 14: séptimo log — confirmado el fix de guardado, más limpieza de performance (2026-07-16)

Séptimo log (`logs/log_1784253297.txt`): el usuario confirmó que el save ya no se corrompe y que el
sonido ya funciona (copió los archivos `.ogg` faltantes a la consola) — juego "un poco más rápido" tras
los fixes de wordwrap/save/conversión de píxeles, pero pidió revisar el log a fondo por más
optimizaciones, incluyendo evaluar sacar `androidui`.

**Confirmado con el diagnóstico de la Fase 11** (`[GFA][diag] wordwrap sospechoso: char_cnt=0 sub_len=11
maxWidth=0.00 text_size=18.00 font_ready=1`): al menos una causa real de `maxWidth=0` para un string
corto (probablemente un elemento de UI sin layout todavía calculado en ese momento) — el piso de
progreso ya puesto en Fase 11 maneja bien este caso (fuerza a consumir el string completo de una vez).
No se ve evidencia de que la causa "un carácter por vez" del mensaje largo de batería (Fase 11) sea la
misma — sigue sin confirmarse con números reales, pero ya no es crítico gracias al piso.

**Encontradas y corregidas 3 fuentes reales de overhead adicionales**:

1. **Logging de GL sin tope durante logo/título/menú** (`ui_status` 0-2): `zenonia_verbose_ui()` en
   `dynlib.c` era instrumentación puntual heredada de una investigación de Zenonia 3 (un bug de fade de
   alpha, nunca confirmado como aplicable a Zenonia 4) que dejaba **sin ningún tope** el log de
   `glColorPointer` mientras el usuario estuviera en esas pantallas — confirmado en este log: 48
   apariciones de la misma línea `Colors UByte` en una sola sesión, cada una con su `fflush` bloqueante,
   justo en las primeras pantallas que ve cualquier usuario. Removido el helper y sus dos usos
   (`glColorPointer_wrapper`, `glBlendFunc_wrapper`) — todo el logging de GL vuelve a estar topado a las
   primeras ~20 llamadas, sin excepciones por `ui_status`.
2. **Carga de assets Android-UI que nunca van a existir**: confirmado en DOS logs reales (Fase 9 y este)
   que ninguno de los ~16 `.rgba` + 2 `.html` que `androidui_load()` intenta cargar existe para Zenonia 4
   (el logo/título/menú se dibuja nativamente vía `assets/menu/*.pzx`, no vía overlay Android — ver Fase
   1/9). Se salteó la llamada a `androidui_load()` en `main.c` — mismo resultado funcional (las texturas
   quedan en 0 igual que si la carga fallara) pero sin los 18 `fopen()` fallidos + sus logs en cada
   arranque. **Deliberadamente NO se tocó** la lógica de hit-test táctil de menú
   (`androidui_menu_hit_test`/`androidui_backbtn_hit_test`/`androidui_reply_hit_test`) ni el dibujo por
   frame (`androidui_draw()`, que ya hacía early-return con texturas en 0): esa lógica traduce toques en
   botones dibujados nativamente a teclas sintéticas (BACK/UP/DOWN/OK) que probablemente sea necesaria
   para que la navegación táctil del menú funcione — remover eso sin confirmar primero habría arriesgado
   romper la navegación por toque a cambio de un beneficio de performance mínimo (son funciones baratas,
   disparadas solo en el toque inicial, no por frame).
3. Confirmado que el patrón de `ptc/000.ptc`..`ptc/033.ptc` (34 apariciones) son 34 archivos DISTINTOS
   (efectos de partículas de personaje), no una repetición del mismo archivo — no es un problema de
   cache, es carga legítima de una sola vez.

Rebuildeados ambos VPK sin warnings nuevos.

## Fase 15: ocultar cruceta/botones táctiles — hook apuntaba al lugar equivocado (2026-07-16)

El usuario pidió ocultar la cruceta y los botones de acción táctiles al jugar (feature ya heredada de
Zenonia 3 vía `HIDE_VIRTUAL_GAMEPAD`/`zenonia_install_hide_dpad_hook()`, activada por default). Revisando
los 7 logs acumulados, la línea de confirmación del hook (`"GVUIPlayerController construido en..."`)
**nunca apareció ni una sola vez**, pese a que el hook se reporta "instalado" en todos los arranques y el
usuario efectivamente ve la cruceta en pantalla — señal de que el hook nunca disparaba.

**Causa raíz encontrada con Ghidra**: a diferencia de Zenonia 3 (donde `GVUIPlayerController` se
construye directo durante el arranque normal del HUD), en Zenonia 4 este objeto se crea de forma
**perezosa** — recién la primera vez que se llama a `GVUIPlayerController::CallBack2(GVUIController*)`
(`out_ghidra.c:44634-44648`), que chequea un singleton (`GVUIMainSystem::pInstance`), construye todo si
hace falta, y termina llamando `ShowBtn(this, 10)` para volver visibles los botones. El offset del
contador de "objetos activos" (`this+0x19c`) que apaga el dibujo/touch es el mismo confirmado para
Zenonia 3 — el problema nunca fue el offset, fue el PUNTO donde se instalaba el hook (el constructor,
que nunca se ejecuta por su cuenta en este build).

**Fix**: `zenonia_install_hide_dpad_hook()` ahora hookea `_ZN20GVUIPlayerController9CallBack2EP14GVUIController`
en vez del constructor — el punto donde se garantiza que el objeto ya existe Y que se lo está por
mostrar. A diferencia del hook viejo (que se auto-desinstalaba tras el primer uso, asumiendo que un
constructor corre una sola vez), este se **reinstala a sí mismo** después de cada llamada, por si
`CallBack2` se invoca más de una vez en una sesión (por ejemplo, si es también el callback de "volver a
modo normal" del sistema de personalización de controles).

Rebuildeados ambos VPK sin warnings nuevos. **Pendiente confirmar en consola** que ahora sí aparezca el
log `[HideDpad] CallBack2 en...` y que la cruceta/botones desaparezcan de la pantalla.

## Fase 16: el fix de Fase 15 no alcanzó — la clase real es otra (2026-07-16)

El usuario reportó que los controles táctiles seguían visibles después del fix de Fase 15, y que el
framerate seguía sin pasar de ~5 FPS. Sin log nuevo todavía, se siguió investigando con Ghidra en base a
la evidencia ya conocida.

**`GVUIPlayerController` resultó ser un sistema de personalización OPCIONAL, no el HUD por defecto**:
`GVUIPlayerController::CallBack2` (el punto donde apuntaba el fix de Fase 15) se registra únicamente
como *callback de un botón de opciones* (`*(code**)(iVar3+0x48) = CallBack2+1`, confirmado en
`out_ghidra.c:44429/44448` — dos botones de un menú de configuración de controles), no algo que se
dispare durante el arranque o el gameplay normal. Esto explica por qué NI el hook original (constructor)
NI el de Fase 15 (`CallBack2`) resolvieron nada — todo ese subsistema (`GVUIMainSystem`/
`GVUIPlayerController`/`GVUIDirectionPad`/`illusiaDpad.pzx`) probablemente nunca se construye en una
partida normal.

**La clase real, encontrada buscando qué usa el prefijo específico de Zenonia** (`CZn`, no el genérico
`GVUI` de la librería de Gamevil): **`CZnTouchKeypad`** — un singleton (`CGsSingleton<CZnTouchKeypad>::
ms_pSingleton`) referenciado por decenas de call sites de gameplay real (`ChangeKeypadMode`, etc.) en
todo el código de estado del juego. Su `Draw()` (`out_ghidra.c:191091`) es simple y sin efectos
secundarios de estado (solo chequea si hay un diálogo tapando la pantalla y si hay touch disponible, y
si ambas condiciones se cumplen dibuja el patrón).

**Fix**: se agregó un segundo hook (independiente del de `GVUIPlayerController`, que se dejó instalado
por si algún día se usa el modo personalizar) sobre `_ZN14CZnTouchKeypad4DrawEv`, reemplazándolo por un
**no-op permanente** — nunca llama a la función original, así que no depende de en qué momento se
construye el singleton (a diferencia de los dos intentos anteriores, que fallaron justamente por
timing de construcción).

**Sobre el framerate (~5 FPS)**: sin más pistas de un log nuevo, se identificó una causa estructural
razonada desde la arquitectura ya confirmada del motor: este es un motor 2D viejo que renderiza TODO por
software a un framebuffer interno del tamaño exacto que se le pasa a `NativeInitDeviceInfo`/
`NativeInitWithBufferSize`, y sube ese framebuffer COMPLETO como textura en cada frame (de ahí el
optimizado de Fase 13, RGB565→RGBA8888). La decisión tomada en Fase 5 de usar 960×544 (nativo de Vita)
como resolución INTERNA significa que el motor hace todo ese trabajo de software-render + conversión de
píxeles sobre **~522 mil píxeles por frame** — muchos más de los que este motor persigue.

**Cambio aplicado**: `GAME_W`/`GAME_H` (resolución interna) bajado a **480×272** (mismo aspecto 16:9,
1/4 del área de 960×544), mientras `SCREEN_W`/`SCREEN_H` se mantienen en 960×544 (lo que ve el usuario —
el escalado final lo hace la GPU vía el quad de `NativeResize`, barato). Mismo patrón que Zenonia 3
(confirmado con resolución interna fija más baja que la pantalla real). **No confirmado todavía en
consola** si 480×272 alcanza para un framerate jugable o si hace falta bajar más — el próximo log real
debería mostrar si esto tuvo impacto real en el FPS.

Rebuildeados ambos VPK sin warnings nuevos.

## Fase 17: el framerate subió (18 FPS) pero apareció "zoom"/desborde — causa real y fix (2026-07-16)

El usuario confirmó que bajar la resolución interna (Fase 16, 480×272) subió el framerate a **18 FPS**
(de ~5), pero reportó un problema visual nuevo: todo se ve pero "desborda" — hay partes de la pantalla
que no se llegan a ver, como si hubiera un zoom. Pidió mantener el tamaño de pantalla completo con
render interno menor, sin ese zoom.

**Causa raíz confirmada con Ghidra** (`glResize`, `out_ghidra.c` ~3274): a diferencia de lo asumido en la
Fase 16 (que `GAME_W/H` — resolución interna — y `SCREEN_W/H` — tamaño de pantalla para el quad final —
eran conceptualmente independientes, como en Zenonia 3), en Zenonia 4 **`glResize` guarda el ancho/alto
que recibe de `NativeResize` en los MISMOS campos de `getDeviceInfo()` que otro código de UI usa como
"tamaño de pantalla" para posicionar elementos absolutos**. Con `GAME_W/H=480×272` (pasado a
`NativeInitDeviceInfo`) pero `SCREEN_W/H=960×544` (pasado a `NativeResize`), el motor terminaba
posicionando UI/HUD asumiendo un lienzo de 960×544 mientras solo renderizaba contenido real de 480×272 —
exactamente el "zoom"/desborde reportado. La separación limpia "renderizo chico, muestro grande" que
funcionaba en Zenonia 3 **no aplica igual en Zenonia 4** porque este build no trata esos dos tamaños como
conceptos independientes.

**Fix real**: en vez de pedirle al MOTOR que renderice chico y escalar después con la GPU (lo que
requeriría que el motor separe ambos conceptos, y ya se confirmó que no lo hace), la solución es
renderizar chico **a nivel de vitaGL** y dejar que el HARDWARE DE VIDEO de la Vita escale la imagen final
al mostrarla en el panel físico — el mismo patrón que usan muchos ports desde PSP (que renderizan nativo
a 480×272).

- `GAME_W`/`GAME_H`/`SCREEN_W`/`SCREEN_H` ahora son **el mismo valor** (480×272) — el motor arma toda su
  UI/HUD/geometría consistentemente para esa única resolución, sin mezclar dos tamaños distintos.
- `vglInitExtended()` en `gl_init()` (`main.c`) ahora crea el framebuffer de vitaGL a 480×272 en vez de
  forzar 960×544 — la Vita escala ese framebuffer más chico al panel físico automáticamente al
  mostrarlo, sin que el motor se entere de que la pantalla real es más grande.

Esto debería mantener la reducción de trabajo de CPU por pixel de la Fase 16 (1/4 del área de 960×544)
sin el bug de desalineación de UI, porque ahora el motor nunca ve una resolución distinta a la que
realmente se renderiza. Rebuildeados ambos VPK sin warnings nuevos. **Pendiente confirmar en consola**
que la UI ya no se vea con zoom/desborde y que el framerate se mantenga o mejore.

## Fase 18: el fix de Fase 17 empeoró el zoom — causa real: un `glViewport` hardcodeado (2026-07-16)

El usuario reportó que, después del fix de Fase 17 (igualar `GAME_W/H`/`SCREEN_W/H` a 480×272 y bajar la
resolución real de `vitaGL`), el zoom **empeoró** en vez de arreglarse, y pidió literalmente lo
contrario a lo que se había hecho.

**Causa real encontrada**: `glViewport_wrapper` en `dynlib.c` — escrito en una fase anterior, cuando
`GAME_W`/`SCREEN_W` siempre eran 960×544 — **hardcodeaba** `glViewport(0, 0, 960, 544)` sin importar qué
valores le pidiera el motor. Esto era inofensivo mientras todo coincidiera en 960×544, pero con `vitaGL`
ahora inicializado a una resolución real de 480×272 (Fase 17), forzar un viewport de 960×544 —**más
grande que el framebuffer real**— hacía que GL solo mostrara una fracción/esquina de la escena
completa: exactamente el "más zoom" reportado (empeoró respecto a Fase 16 porque en Fase 16 el
framebuffer real de `vitaGL` todavía era 960×544, así que el viewport forzado coincidía por
casualidad — el bug estaba "dormido" hasta que Fase 17 cambió la resolución real de `vitaGL`).

**Fix**: `glViewport_wrapper` ahora pasa tal cual los valores que pide el motor (`glViewport(x, y, width,
height)`) en vez de pisarlos — el motor ya pide el viewport correcto porque coincide con `GAME_W`/
`SCREEN_W` (ver `NativeResize`/`glResize`), no hacía falta forzar nada. No se encontraron otros
overrides hardcodeados de resolución en `dynlib.c` (`glOrthof_wrapper`/`glOrthox_wrapper` ya eran
pass-through puro, confirmado revisando el archivo completo).

Rebuildeados ambos VPK sin warnings nuevos. **Pendiente confirmar en consola** que esta vez sí se vea
la pantalla completa sin zoom/desborde, manteniendo el framerate más alto de la resolución interna
reducida (480×272).

## Fase 19: el fix de glViewport no alcanzó — el escalado implícito de vitaGL no funciona como se asumía (2026-07-16)

Noveno log (`logs/log_1784259928.txt`): confirmado que el fix de Fase 18 funcionó a nivel de GL puro —
`glViewport x=0 y=0 w=480 h=272` y `glOrthof left=-240 right=240 bottom=-136 top=136` ya son
perfectamente consistentes entre sí (viewport y mitades del ortho coinciden exactamente con
480×272). Pese a eso, el usuario reportó que **seguía viéndose con mucho zoom** (framerate se mantuvo
en 18 FPS, sin regresión).

Como el GL interno ya estaba confirmado consistente (sin desajuste de viewport/ortho), la única pieza
que quedaba sin verificar era la suposición central de las Fases 16/17: que **el hardware de video de
la Vita escala automáticamente un framebuffer de vitaGL no-nativo (480×272) al panel físico
(960×544) al mostrarlo**. Esta suposición resultó ser incorrecta (o al menos no se comporta como un
simple stretch-to-fit) — confirmado empíricamente en dos intentos distintos que produjeron zoom.

**Fix real: escalado explícito vía FBO, en vez de depender de un escalado implícito no confirmado**:

- `vitaGL` vuelve a inicializarse a la resolución REAL del panel (960×544, confiable, sin ambigüedad).
- Se creó un FBO interno (`create_game_fbo()` en `main.c`) con una textura de color de `GAME_W×GAME_H`
  (480×272) — el motor (y los overlays de splash/androidui) renderizan ahí en vez de al framebuffer
  real, dentro del loop principal (`glBindFramebuffer(GL_FRAMEBUFFER, g_game_fbo)` antes de
  `NativeRender`).
- Al final de cada frame, se vuelve al framebuffer real (`glBindFramebuffer(..., 0)`,
  `glViewport(0,0,960,544)`) y se hace **un solo blit con textura** (`draw_game_fbo_upscaled()`)
  estirando ese FBO de 480×272 a un quad que llena los 960×544 reales — un escalado GPU explícito y
  barato, ya no depende de ninguna suposición sobre cómo la Vita maneja resoluciones no-nativas.

Como la orientación vertical de una textura poblada por render-to-texture (en vez de cargada desde un
archivo) puede quedar invertida según la convención de UV usada, se documentó en el código el fix de
una línea (invertir el orden de `uvs`) por si el resultado aparece con la imagen dada vuelta en
consola — no se pudo confirmar la orientación sin acceso a hardware real.

Rebuildeados ambos VPK sin warnings nuevos. **Pendiente confirmar en consola** que este enfoque (blit
explícito) sí muestre la pantalla completa sin zoom mientras mantiene el framerate alto — y si la
imagen aparece verticalmente invertida, avisar para aplicar el fix de una línea ya documentado.

## Fase 20: el zoom era del MOTOR, no de ninguna capa de escalado — fix definitivo (2026-07-17)

Décimo log (`logs/log_1784260421.txt`): el usuario reportó que **seguía exactamente igual de zoomeado**
incluso con el blit explícito por FBO de la Fase 19, y aclaró algo clave: *"no es que veo las texturas
en baja resolución pero full pantalla sino que todo tiene zoom"* — es decir, no es un problema de nitidez
ni de que falte pantalla completa, es que el CONTENIDO del juego en sí se ve objetivamente más cerca/más
grande de lo que debería, en todas las pruebas.

**Esto fue la pista decisiva**: se probaron 3 mecanismos de escalado completamente distintos (Fase 17:
escalado implícito por hardware: Fase 18: paso directo de `glViewport`; Fase 19: blit explícito por FBO)
y los TRES dieron el mismo síntoma de zoom. El único factor que se mantuvo IGUAL en los tres intentos
fue `GAME_W`/`GAME_H` = 480×272 (lo que se le pasa a `NativeInitDeviceInfo`/`NativeInitWithBufferSize`/
`NativeResize`, o sea, lo que el motor CREE que mide su lienzo). Conclusión: el zoom nunca fue un
problema de escalado de pantalla — es que **bajar `GAME_W`/`GAME_H` le rompe al motor su propia
matemática interna de cámara/posicionamiento** (probablemente tratando esas dimensiones como el "mundo
visible" para su propio zoom/cámara 2D), completamente independiente de cómo se escale después el
resultado final.

**Fix definitivo**: separar por completo "lo que el motor cree que mide su lienzo" de "cuántos píxeles
se rasterizan de verdad":

- `GAME_W`/`GAME_H`/`SCREEN_W`/`SCREEN_H` vuelven a **960×544 fijo** — el motor arma toda su
  geometría/cámara/UI para ese tamaño, igual que en la Fase 5 original (sin zoom, confirmado).
- Nueva resolución `RENDER_W`/`RENDER_H` = 480×272 — el tamaño REAL del FBO interno donde se rasteriza
  todo (la ganancia de performance).
- `glViewport_wrapper` (`dynlib.c`) ya no deja pasar el viewport que pide el motor (960×544) — lo
  **fuerza siempre** a `RENDER_W`/`RENDER_H` (480×272), compartido con `main.c` vía `extern int
  g_render_w, g_render_h`. La clave: `glViewport` define cómo se mapea el rango NDC (-1..1) a píxeles
  reales, un concepto completamente INDEPENDIENTE de la proyección ortográfica (`glOrthof`) que arma el
  motor para su propio "mundo" de 960×544 — forzar un viewport más chico que el pedido hace que TODO el
  contenido se escale proporcionalmente al viewport real (sin clipping, sin zoom, sin que el motor se
  entere), simplemente rasterizando menos píxeles.

Rebuildeados ambos VPK sin warnings nuevos. **Pendiente confirmar en consola** — esta vez el motor NUNCA
ve una resolución distinta a 960×544 (cero riesgo de romper su matemática interna), mientras la
rasterización real ocurre a 480×272 (misma ganancia de performance que las fases anteriores).

## Fase 21: el viewport forzado tampoco dio framerate — el costo real es CPU, no GPU (2026-07-17)

Undécimo log (`logs/log_1784261306.txt`): el usuario confirmó algo muy revelador: **la resolución se ve
baja/correcta (sin zoom, tal como se buscaba con el fix de Fase 20), pero volvió a ~5 FPS** — "en teoría
debería ir más rápido como con el zoom pero no funciona así".

**Esto es la prueba definitiva de la arquitectura real del motor**: forzar el `glViewport` real a
480×272 (Fase 20) sí eliminó el zoom, pero **no dio ninguna mejora de framerate** — volvió exactamente
al mismo ~5 FPS que con render a 960×544 completo. Como el único intento que SÍ daba 18 FPS era el que
bajaba `GAME_W`/`GAME_H` directamente (rompiendo la cámara del motor), la conclusión ahora es
inequívoca: **el costo caro de este motor no está en lo que la GPU rasteriza para el quad final de
pantalla** (eso es barato sea cual sea el tamaño del viewport) — **está en el render por software que
el motor hace en CPU hacia su framebuffer interno** (la conversión RGB565→RGBA8888 de ~522k píxeles,
ver Fase 13) **antes de que exista ninguna llamada GL de por medio**. Ese framebuffer interno está
atado al tamaño que se le pasó a `NativeInitDeviceInfo` (`GAME_W`/`H`), no al `glViewport` — interceptar
el viewport de la GPU nunca iba a tocar ese costo, porque ocurre en una etapa anterior que no se puede
interceptar con trucos de GL.

**Conclusión práctica**: no hay forma de bajar el costo de render de este motor sin que el propio motor
crea que su lienzo es más chico — y ya se confirmó (Fases 16-20) que eso rompe su cámara. Las dos metas
(visual correcto + ganancia de rendimiento por resolución) son incompatibles para este build específico
del motor, al menos con las herramientas de interceptar GL disponibles desde el loader.

**Se descartó por completo el enfoque de resolución** (se sacó el FBO intermedio y el `glViewport`
forzado — no aportaban nada real, solo una pasada extra y riesgo). En su lugar, se probó una
optimización completamente distinta y de bajo riesgo, común en homebrew de Vita: **subir el reloj de
CPU/GPU/bus al máximo documentado** (`scePowerSetArmClockFrequency(444)`,
`scePowerSetBusClockFrequency(222)`, `scePowerSetGpuClockFrequency(222)`,
`scePowerSetGpuXbarClockFrequency(166)`, llamado al principio de `gl_init()`) — la Vita arranca con un
perfil de reloj más bajo por defecto, y subirlo no cambia nada de la lógica del motor ni del loader,
solo la velocidad real del hardware.

Rebuildeados ambos VPK sin warnings nuevos (vuelta a render directo simple, sin FBO). **Pendiente
confirmar en consola** si el boost de reloj da alguna mejora perceptible de framerate manteniendo
960×544 sin zoom — si no alcanza, el techo de rendimiento actual (~5 FPS a resolución completa) podría
ser un límite real de este motor en este hardware dados los fixes ya aplicados (wordwrap, guardado,
conversión de píxeles, logging), sin tocar el propio código del motor.

## Fase 22: nítido de nuevo, boost de reloj da poco — conversión de píxeles con NEON (2026-07-17)

Duodécimo log (`logs/log_1784263617.txt`): el usuario confirmó que la imagen volvió a verse nítida y sin
zoom (el revert de la Fase 21 funcionó), pero que el boost de reloj de CPU/GPU solo subió el framerate
hasta ~7 FPS en algunas partes — mejora real pero chica. Pidió alternativas.

Se le presentaron 4 caminos con sus trade-offs (vectorizar con NEON la conversión de píxeles ya
confirmada correcta; probar subir RGB565 directo a vitaGL sin convertir — mayor ganancia posible pero
con riesgo real, ya que hay un comentario en el código heredado que documenta que esto se probó en el
port de Zenonia 2 y dio colores incorrectos en consola real; investigar un parche binario al cálculo de
cámara/zoom del motor para poder bajar la resolución interna sin romper nada — mayor potencial pero
mucho más trabajo de Ghidra; o dejarlo como está). El usuario eligió la opción segura: **vectorizar con
NEON**.

**Implementado**: `convert_rgb565_to_rgba8888()` (`dynlib.c`) reescrita para procesar 8 píxeles por
iteración con intrínsecos NEON (`arm_neon.h`, ya usado por la librería `mathneon` que este proyecto
enlaza desde la Fase 2). Cambios:

- Se reemplazaron las tablas de lookup (5/6 bits → 8 bits) por **replicación de bits**
  (`(v<<3)|(v>>2)` para canales de 5 bits, `(v<<2)|(v>>4)` para el canal G de 6 bits) — el resultado es
  idéntico o como mucho ±1 respecto al `*255/N` exacto anterior (imperceptible), pero son solo
  shifts/ORs, que vectorizan directo con NEON. Una tabla de 64 entradas (el canal G) NO vectoriza bien
  con las instrucciones `vtbl` de ARMv7 (limitadas a 32 bytes por lookup), por eso se abandonó el
  enfoque de tablas en vez de intentar forzarlo.
- `vst4_u8` hace el *interleaving* final a `R,G,B,A` en memoria en una sola instrucción (en vez de un
  empaquetado manual a `uint32` por píxel).
- Cola escalar (para cuando el ancho×alto no es múltiplo de 8) con la misma fórmula de replicación de
  bits, para que no haya una costura visible entre el tramo vectorizado y el escalar.

Compiló sin flags especiales de NEON (el toolchain de vitasdk ya lo soporta por default para este
target). Rebuildeados ambos VPK sin warnings nuevos. **Pendiente confirmar en consola** la ganancia real
de framerate — si no alcanza, las otras 2 alternativas (RGB565 directo con riesgo conocido, o parche de
cámara) siguen disponibles para la próxima iteración.

Confirmado por el usuario: con gráficos bajados en las opciones del juego, el NEON de la Fase 22 llevó
el framerate a **7-8 FPS** (subiendo desde ~5-7 con el boost de reloj solo). Mejora real pero todavía
lejos de los 18 FPS que se vieron con la resolución interna baja (que rompía la cámara).

## Fase 23: investigado el parche de cámara — conclusión: NO hay una función única para parchear (2026-07-17)

A pedido del usuario, se investigó con Ghidra si existe un cálculo de cámara/zoom puntual que se pueda
parchear para bajar `GAME_W`/`GAME_H` sin el bug de zoom de las Fases 16-21. Se rastreó la cadena
completa: `GAME_W`/`H` → `initPlatformSize()` → `platformGetFramebufferWidth/Height()` →
`MC_grpGetDisplayInfo()` → `CGsGraphics::InitialScreen()` (que fija los campos `+0x4c`/`+0x50` que usa
toda la UI/gameplay del motor para posicionamiento).

**Conclusión (no lo que se esperaba encontrar): la cadena es completamente consistente, no hay ningún
desajuste entre lo que el motor "cree" y lo que renderiza.** El "zoom" no es un bug de cálculo — es una
consecuencia **de diseño** del motor: los sprites/tiles se dibujan a tamaño de píxel FIJO (no se
encontró ningún factor de escala tipo DPI que reajuste tamaños según resolución, solo posicionamiento en
píxeles absolutos vía `SetClipRect`/`SetPosition`/`SetSize`). Con una pantalla lógica más chica, entra
menos mundo del juego en la misma cantidad de píxeles de sprite — el "zoom" es el resultado esperado de
mostrar menos mundo, no un valor mal calculado en un solo lugar. Corregirlo de verdad requeriría
interceptar y reescalar CADA primitiva de dibujo del motor (cientos de call sites: tamaños, posiciones,
bounding boxes de cada sprite) — un supersampling manual sobre toda la lógica de render del juego, mucho
más grande y riesgoso que cualquier otro intento de esta sesión, sin garantía de resultado.

**Recomendación dada al usuario**: no vale la pena seguir por este camino. Quedan 2 alternativas reales
sin explorar: (1) aceptar ~7-8 FPS a pantalla completa como techo práctico actual, o (2) probar RGB565
directo a vitaGL sin conversión (riesgo conocido de colores incorrectos, documentado en el port de
Zenonia 2 — pendiente de decisión del usuario).

## Fase 24: infraestructura de prueba A/B para RGB565 directo + confirmación (peor de lo esperado) de que no hay parche de cámara viable (2026-07-17)

**Prueba A/B de RGB565 directo — infraestructura agregada, todavía sin probar en consola**:

- Nueva opción de CMake `RGB565_DIRECT_UPLOAD` (default OFF). Cuando está ON, define
  `ZENONIA_RGB565_DIRECT` y ambos wrappers de subida de textura en `dynlib.c`
  (`glTexImage2D_wrapper`/`glTexSubImage2D_wrapper`) saltan por completo
  `convert_rgb565_to_rgba8888()` (la conversión NEON de la Fase 22) y suben el framebuffer del motor tal
  cual en `GL_UNSIGNED_SHORT_5_6_5` — mayor ganancia de FPS potencial (cero trabajo de conversión en
  CPU) pero con el riesgo ya documentado en el port de Zenonia 2 (mismo motor): puede mostrar colores
  incorrectos, nunca confirmado en Zenonia 4 en hardware real.
- Nueva variable de CMake `VPK_OUTPUT_NAME` (default `zenonia_4`) que ahora controla el nombre del
  `.self`/`.vpk` generado, separada de `PROJECT_NAME` (que sigue siendo el nombre del target de
  compilación) — permite generar un VPK con nombre distinto sin pisar `build/zenonia_4.vpk`.
- `build.sh --rgb565-test` arma el build de prueba con `-DRGB565_DIRECT_UPLOAD=ON
  -DVPK_OUTPUT_NAME=zenonia_4_rgb565test`, produciendo `build/zenonia_4_rgb565test.vpk` (mismo
  `VITA_TITLEID`, así que instalar este VPK reemplaza al build normal en la consola — no quedan
  instalados en paralelo, es una comparación secuencial). El nombre de la app en LiveArea también se
  sufija automáticamente (`Zenonia 4 [RGB565 test]`) para no confundir cuál build está instalado.
- Verificado que ambos builds (`./build.sh` y `./build.sh --rgb565-test`) compilan sin errores ni
  warnings nuevos. **Pendiente**: instalar `zenonia_4_rgb565test.vpk` en consola y confirmar A/B real
  (framerate ganado vs. colores correctos/incorrectos) — sin esto, es solo la infraestructura.

**Investigación a fondo del parche de cámara/zoom (a pedido del usuario, para stress-testear la
conclusión pesimista de la Fase 23)**: se buscó específicamente si existe un punto de rasterización
único (o un número chico de ellos) por el que pase TODO el dibujo de sprites/tiles, aguas abajo de los
`SetPosition`/`SetSize`/`SetClipRect` de alto nivel — si existiera, escalar ahí (multiplicar x/y/w/h por
un factor) permitiría bajar la resolución real de rasterizado sin que el motor deje de creer que su
lienzo es 960×544 (o sea, sin romper la cámara).

**Resultado: la conclusión de la Fase 23 se confirma, y es peor de lo estimado ahí.** Sí existe una
función `DrawNative` (`out_ghidra.c:226057`) con soporte de escala ya incorporado en su propio código
(una rama que multiplica/divide por un parámetro de escala) — pero **no es el blitter universal**: solo
tiene 4 call sites en todo el binario (`CGsScreenEffMgr::DrawBackupScreen`, `CMvMap::DrawMiniMap`,
`CCircleShadow::Draw`, `CGcxBitmap::PutNativeImage`/`Blt`), casos especiales (minimapa, sombra circular,
efectos de pantalla), no el pipeline de sprites/tiles del gameplay normal. `CGcxBitmap::BltFast` (la
variante que uno esperaría para el hot path) es un stub muerto (`return 0`).

El pipeline real está repartido en **~20 primitivas independientes** de `CGsGraphics::` (`PutPixel` x2,
`LineDDAFixedPoint`, `ClippingLine_LiangBarsky`, `DrawLine`/`DrawRect`/`DrawFillRect`/`DrawRoundRect`/
`DrawEllipse`/`DrawFillEllipse`/`DrawFillRoundRect`/`DrawPolygon`/`DrawFillPolygon`/`DrawOutLineShade`/
`DrawScrollBar`/`DrawColorGradationRect`/`DrawNativeFillEllipse`, más 3 renderers de texto separados:
`DrawTextSystemFont`/`DrawTextBBF`/`DrawTextInRect`), cada una con su propia matemática de clipping/
transformación (Liang-Barsky, DDA de punto fijo, punto medio de elipse, etc.) — escalar correctamente
significaría re-derivar y parchear cada algoritmo por separado, no multiplicar un puñado de argumentos
x/y/w/h compartidos. Esto es estrictamente MÁS trabajo/riesgo que la estimación original de la Fase 23
("cientos de call sites, una función"), no menos.

**Conclusión práctica reafirmada**: no hay una vía de parche binario razonable para bajar la resolución
interna sin romper la cámara. Las únicas rutas de rendimiento que quedan son las ya evaluadas: aceptar
~7-8 FPS actuales, o medir el resultado real de RGB565 directo (infraestructura de esta fase) en
consola.

*(Nota: no se investigó todavía dónde vive el pipeline real de sprites/tiles del hot path — solo se
descartó que pase por `DrawNative`/`CGcxBitmap`. Sería una investigación separada y más grande si se
quiere retomar este camino en el futuro.)*

## Fase 25: RGB565 directo confirmado en consola real — sin errores gráficos, 11-14 FPS (2026-07-17)

Décimo tercer log (`logs/log_1784312977.txt`): el usuario probó `build/zenonia_4_rgb565test.vpk` en
consola real y confirmó **la prueba A/B fue un éxito**: framerate subió a ~14 FPS pico y 11-12 FPS
estable en gameplay real (desde ~7-8 FPS del build NEON), **sin ningún error de color visible** — el
riesgo conocido documentado en el port de Zenonia 2 (mismo motor) no se manifestó acá. Confirmado en el
log que corresponde efectivamente al build RGB565 directo (no aparece ninguna línea de
`convert_rgb565_to_rgba8888`), sesión estable por 1200+ frames, guardado de partida y sonido funcionando,
sin JNI errors nuevos (los `[JNI WARN]`/`[JNI ERR]` de `getPackageInfo`/`GET_SIGNATURES`/`signatures` son
la verificación de firma/licencia de Google Play ya esperada como no-op inofensivo, no algo nuevo de esta
prueba).

**Dos mejoras de bajo riesgo agregadas para seguir optimizando** (a pedido del usuario de llegar a
"100% jugable"):

1. **Medición real de FPS** (`loader/main.c`, dentro del checkpoint de cada 120 frames): hasta ahora
   TODOS los números de FPS de este documento (5, 7-8, 11-14, 18) eran impresión subjetiva del usuario —
   no había ninguna medición objetiva en el propio loader. Se agregó un cálculo real con
   `sceKernelGetProcessTimeWide()` (un timestamp cada 120 frames, costo insignificante) que ahora imprime
   `fps=%.1f` en la misma línea de log — permite medir el impacto de cada cambio futuro con un número
   real en vez de "se siente más rápido".
2. **Tope de logging en `OnVibrate`/`OnEvent`** (`loader/java.c`): a diferencia de `OnSoundPlay` (ya
   topado a 40 apariciones desde antes), estos dos callbacks JNI llamaban `game_log()` — que hace
   `fprintf` + `fflush()` bloqueante a disco — **sin ningún tope**, una vez por cada vibración/evento del
   motor. En combate real esto puede dispararse muchas veces por segundo (feedback háptico por golpe),
   cada una con I/O de disco sin cachear en pleno gameplay. Topado a 40 apariciones, mismo patrón que
   `OnSoundPlay`.

Ambos builds (`./build.sh` y `./build.sh --rgb565-test`) recompilados y verificados sin errores ni
warnings nuevos. **Pendiente confirmar en consola** si el contador de FPS real coincide con la impresión
subjetiva del usuario (11-14) y si el tope de logging de vibración/evento aporta alguna mejora perceptible
adicional.

**Decisión de arquitectura pendiente**: `RGB565_DIRECT_UPLOAD` sigue en OFF por default en
`CMakeLists.txt` (el build normal `./build.sh` sigue usando la conversión NEON) — con la prueba A/B ya
confirmada exitosa en consola, cambiar el default a ON (o directamente eliminar la ruta NEON como
fallback) es una decisión de arquitectura real que se dejó pendiente de confirmación explícita del
usuario antes de tocar el default del build "normal".

## Fase 26: RGB565 directo promovido a default — el usuario confirmó que quiere mantener el cambio (2026-07-17)

El usuario confirmó explícitamente ("paso la prueba por lo que quiero que este cambio se mantenga") que
quiere que RGB565 directo sea el comportamiento default, no solo un build de prueba paralelo.

- `CMakeLists.txt`: `RGB565_DIRECT_UPLOAD` ahora default **ON** (antes OFF). El build "normal"
  (`./build.sh`, sin flags) genera `build/zenonia_4.vpk` con RGB565 directo — el mismo build confirmado
  en consola con 11-14 FPS y sin errores de color.
- La ruta NEON de la Fase 22 **no se borró** — sigue completa en el código, ahora detrás del flag
  invertido: `./build.sh --neon-fallback` genera `build/zenonia_4_neonfallback.vpk`
  (`RGB565_DIRECT_UPLOAD=OFF`), por si algún asset/locale todavía no probado revela el mismo problema de
  color que sí se vio en el port de Zenonia 2. El nombre de la app en LiveArea de ese build de respaldo
  ahora lleva el sufijo `[NEON fallback]` (invertido respecto a la Fase 24, donde el sufijo marcaba el
  build de prueba en vez del de respaldo).
- El archivo `build/zenonia_4_rgb565test.vpk` de la Fase 24 se eliminó (build local, `build/` está en
  `.gitignore`) — quedó redundante porque `zenonia_4.vpk` ahora es exactamente ese mismo build.
- Verificado en `/tmp/zenonia4-build/CMakeCache.txt` tras una reconfiguración limpia (cache vieja
  borrada) que `./build.sh` cachea `RGB565_DIRECT_UPLOAD:BOOL=ON` y `./build.sh --neon-fallback` cachea
  `RGB565_DIRECT_UPLOAD:BOOL=OFF` — ambos builds compilan sin errores ni warnings nuevos.

## Fase 27: confirmado en consola con datos reales — ~9-10 FPS en gameplay (no 11-14), instrumentado el conteo de subidas de framebuffer por frame (2026-07-17)

Décimo cuarto log (`logs/log_1784340572.txt`): el usuario reinstaló correctamente esta vez (confirmado:
cero apariciones de la palabra `NEON` en el log — el build que corrió es efectivamente RGB565 directo) y
reportó que el framerate "sigue en 12 FPS máximos". Con el contador de FPS real de la Fase 25 ya en el
binario, esta es la primera medición objetiva de este build en consola real: `fps=9.5` (menú), `fps=6.2`
(un checkpoint justo después de un `loadFile: Save0.dat` — un pico de carga puntual, no gameplay
sostenido), `fps=10.3` (combate real). Es decir, el techo real medido es **~9-10 FPS sostenido**, no los
"11-14" que se habían estimado a ojo en la Fase 25 (esa sesión fue anterior a tener el contador real). El
número real es más bajo que la impresión subjetiva, pero sigue siendo una mejora clara sobre los ~5-7 FPS
del build NEON — el techo está más cerca de "jugable con esfuerzo" que de "100% jugable".

**Instrumentación agregada para guiar el próximo paso** (en vez de seguir adivinando): un comentario
antiguo junto a `convert_rgb565_to_rgba8888` (antes de esta sesión) ya sospechaba que "el motor sube el
framebuffer completo antes de CADA quad (varias veces por frame)" — si es cierto, cada subida completa
del framebuffer (960×544×2 bytes = ~1 MB en RGB565) tiene un costo real de transferencia a VRAM
independientemente de que ya no haya conversión de por medio, y si se repite varias veces por frame ese
costo se multiplica. Se agregó un contador global (`g_rgb565_uploads_this_frame` en `dynlib.c`,
incrementado en `glTexImage2D_wrapper`/`glTexSubImage2D_wrapper` cuando `format=GL_RGB`,
`type=GL_UNSIGNED_SHORT_5_6_5`) que el loop principal (`main.c`) acumula y resetea alrededor de cada
`NativeRender()`, promediado sobre los últimos 120 frames e impreso junto al FPS real:
`avg_rgb565_uploads_per_frame=%.1f`. Ambos builds (`./build.sh` y `./build.sh --neon-fallback`)
recompilados y verificados sin errores ni warnings nuevos.

**Pendiente confirmar en consola**: si `avg_rgb565_uploads_per_frame` sale bien por encima de 1.0, confirma
la sospecha vieja y da un objetivo concreto y de bajo riesgo (cachear/deduplicar subidas redundantes del
mismo framebuffer dentro de un mismo frame) antes de considerar cualquier vía más arriesgada (parches de
motor, threading).

## Fase 28: sospecha de "múltiples subidas por frame" DESCARTADA con datos reales — no queda ningún candidato de bajo riesgo pendiente (2026-07-17)

Décimo quinto log (`logs/log_1784344392.txt`): otra vez confirmado sin `NEON` en el log (build correcto).
Los 4 checkpoints con datos reales:

| frame | fps | avg_rgb565_uploads_per_frame | contexto |
|---|---|---|---|
| 121 | 9.6 | 1.0 | menú (ui_status=1) |
| 241 | 6.6 | 1.0 | justo después de `loadFile: Save0.dat` (segunda vez que este patrón exacto aparece — ver Fase 27, mismo síntoma en el log anterior) |
| 361 | 10.4 | 1.0 | combate real |
| 481 | 11.4 | 1.0 | combate real |

**`avg_rgb565_uploads_per_frame` sale exactamente 1.0 en los 4 checkpoints — la sospecha vieja queda
DESCARTADA con datos reales**: el motor sube el framebuffer completo UNA sola vez por `NativeRender()`,
no varias veces por quad como sugería el comentario heredado (nunca se había confirmado con números
reales hasta ahora, era una suposición). No hay ninguna subida redundante que deduplicar — esta vía de
optimización de bajo riesgo se cierra.

**Patrón confirmado dos veces**: la caída puntual a ~6-7 FPS coincide EXACTAMENTE, en ambos logs (Fase
27 y esta), con el checkpoint inmediatamente posterior a un `loadFile: Save0.dat` (la transición
menú→gameplay real, que dispara una carga de partida). Es una caída transitoria de una sola ventana de
120 frames, no un piso sostenido — el resto de los checkpoints en gameplay real da 9.6-11.4 FPS. No se
investigó la causa exacta (parseo del save, reconstrucción de estado interno del motor) porque es un
costo único por transición, de impacto mucho menor que el piso sostenido de gameplay.

**Balance real y honesto de esta ronda de optimización** (Fases 22-28, comparando con NEON como línea de
base):

- NEON (Fase 22): ~5-7 FPS.
- RGB565 directo (Fase 24-26): **~9-11 FPS sostenido en combate real**, confirmado en DOS sesiones de
  consola separadas con el mismo patrón. Mejora real y reproducible.
- El único otro sospechoso de bajo riesgo que se había planteado (subidas redundantes de framebuffer)
  queda descartado con esta fase.
- Los caminos que quedan (parche de motor sobre ~20 primitivas de dibujo distintas, Fase 24; threading,
  nunca explorado en profundidad) tienen un perfil de riesgo/esfuerzo notablemente más alto, ya
  documentado — no son la clase de cambio "de bajo riesgo" que se venía aplicando en esta sesión.

**Recomendación**: con el camino de bajo riesgo agotado, la decisión de si vale la pena invertir en el
parche de motor (alto esfuerzo/riesgo) o aceptar ~9-11 FPS como el techo práctico de esta ronda y mover el
foco a otra área del port queda pendiente de decisión explícita del usuario — no es una llamada que
convenga tomar unilateralmente dado el salto de riesgo.

**El usuario eligió explícitamente investigar el parche de motor.**

## Fase 29: la Fase 24 se equivocó — `DrawNative` SÍ es el pipeline real de sprites (despacho por vtable invisible a grep), instrumentado un contador de llamadas antes de tocar nada (2026-07-17)

La Fase 24 concluyó que `DrawNative`/`CGcxBitmap::Blt` tenían solo 4 call sites (casos especiales:
minimapa, sombra circular, efectos de pantalla) y que el pipeline real de sprites/personajes/tiles estaba
repartido en ~20 primitivas independientes de `CGsGraphics::`. Esa conclusión se basó en un **grep plano
de nombres mangleados**, que no ve despacho polimórfico por vtable — y resultó estar mal para la parte que
más importa (el dibujo de personajes/animaciones).

**Cadena real encontrada** (investigación a fondo con Ghidra sobre `out_ghidra.c`):

```
CMvCharacter::DrawCharacter(...)                     [out_ghidra.c:104602]
  -> CGxPZxAni::Draw(...)                            [out_ghidra.c:236348 -- 45 call sites reales de
                                                       gameplay: CMvCharacter::DrawCharacter,
                                                       CMvPlayer::DrawCharacter,
                                                       CMvCostume::DrawCostumeAll, etc.]
      -> puntero a funcion cargado del struct tipo-vtable del frame, this+0x10
         (invisible a un grep de nombres -- es un salto indirecto, no una llamada nombrada)
          -> CGxPZxBitmap::Blt(...)                  [out_ghidra.c:237389 -- wrapper delgado]
              -> CGcxBitmap::Blt(...)                [out_ghidra.c:243741]
                  -> CGcxBitmap::PutNativeImage(...) [out_ghidra.c:243721]
                      -> DrawNative(...)              [out_ghidra.c:226057]
```

`CGxPZxAni`/`CGxPZxBitmap` (prefijo `PZx` — coincide con el formato de asset propietario `.pzx` ya
documentado en `plan_zenonia4_port.md` §1/§2 como el empaquetado de recursos de Zenonia) son casi
seguro el renderer real de personajes/animaciones/costumes — no un caso especial. `CGcxBitmap::BltFast`
(la rama alternativa `+0x14`, "DrawFast") es un stub muerto (`return 0`), así que TODO el contenido real
del juego que pasa por esta jerarquía termina en `DrawNative`.

**Por qué esto es mucho más tratable que las ~20 primitivas de `CGsGraphics::`**: `DrawNative` YA TIENE
lógica de escala incorporada (rama `param_8 == 0x10`, un modo de `enumDrawOP`) que escala tanto el rect
destino como el paso de la fuente, aplicada DESPUÉS del clipping contra `GcxGetMainScreenBuffer()` — es
decir, el orden correcto "clipear primero, escalar después" ya lo resolvió el desarrollo original. Si esto
es de verdad el hot path, es UN punto de parche con la matemática ya hecha, no 20 algoritmos distintos
para re-derivar a mano.

**Antes de tocar nada — instrumentación de solo-conteo, mismo criterio de "medir antes de parchear" que
RGB565 directo**: se agregó un hook de solo-conteo (pass-through, llama siempre a la función original sin
cambiar su comportamiento) sobre `DrawNative` (confirmado el símbolo real con
`nm -D lib/libgameDSO.so`: `_Z10DrawNativeliiiiii10enumDrawOPih`, retorno `void` confirmado en
`out_ghidra.c`). Detalles:

- `g_drawnative_calls_this_frame` (`dynlib.c`) + acumulador/promedio por 120 frames en `main.c`
  (`avg_drawnative_calls_per_frame`), mismo patrón que `g_rgb565_uploads_this_frame` de la Fase 27.
- El hook usa el mismo patrón ya existente en el código (`so_unhook` → llamar directo → `hook_addr` de
  nuevo, igual que `GVUIPlayerController_callback2_hook`) en vez del macro `SO_CONTINUE` de
  `so_util.h` (nunca usado en este proyecto, y no soporta retorno `void` tal como está escrito).
- **Nueva opción de CMake `INSTRUMENT_DRAWNATIVE_CALLS`** (default OFF) — deliberadamente un build de
  diagnóstico APARTE (`./build.sh --drawnative-probe` → `build/zenonia_4_drawnative_probe.vpk`), NO el
  build normal: el ciclo unhook/rehook hace flush de i-cache en cada invocación, un costo real que se
  suma si `DrawNative` se llama muchas veces por frame — el FPS medido con este build activo no sirve
  para comparar contra los números de las Fases 27/28, solo importa `avg_drawnative_calls_per_frame`.

Los 3 builds (`./build.sh`, `--neon-fallback`, `--drawnative-probe`) compilan sin errores ni warnings
nuevos. Confirmado con `strings` sobre el binario compilado que el símbolo y los logs esperados están
presentes.

**Pendiente confirmar en consola**: instalar `build/zenonia_4_drawnative_probe.vpk` y jugar un rato —
`avg_drawnative_calls_per_frame` da el número real de llamadas por frame en gameplay real, el dato que
falta antes de siquiera considerar escribir el parche de escala en sí (que es la parte realmente riesgosa,
todavía no intentada).

## Fase 30: DrawNative NO es el hot path — la hipótesis de la Fase 29 se descarta con datos reales (2026-07-18)

Décimo sexto log (`logs/log_1784347376.txt`), build `zenonia_4_drawnative_probe.vpk` confirmado activo
(`[DrawNativeProbe] hook de conteo instalado en DrawNative (0x98151ca9)` en el arranque). 4 checkpoints:

| frame | fps | avg_rgb565_uploads_per_frame | avg_drawnative_calls_per_frame | contexto |
|---|---|---|---|---|
| 121 | 6.4 | 1.0 | **0.0** | ui_status=19 (transición) |
| 241 | 4.9 | 1.0 | **0.1** | justo después de `loadFile: Save0.dat` (tercera vez que aparece este patrón — Fases 27/28/30) |
| 361 | 7.2 | 1.0 | **1.0** | combate real |
| 481 | 7.2 | 1.0 | **1.0** | combate real |

**`avg_drawnative_calls_per_frame` sale ~0-1, no decenas/cientos — la hipótesis de la Fase 29 queda
DESCARTADA con datos reales.** Si `DrawNative` fuera de verdad el blitter de cada personaje/monstruo/tile
en pantalla, en combate real (varios sprites visibles) se esperarían muchas llamadas por frame, no ~1. La
inferencia de la Fase 29 ("`CGxPZxAni::Draw` tiene 45 call sites reales de gameplay, por lo tanto es
caliente") confundió **call sites estáticos en el código** con **frecuencia de ejecución real** — son 45
lugares DISTINTOS del código que puntualmente pueden llamar a esa función (costume, arma, efecto, etc.),
no evidencia de que se ejecute 45 veces por frame; y aun cuando se ejecuta, el despacho por vtable
(`this+0x10`) puede resolver a una implementación DISTINTA de `CGcxBitmap::Blt`/`DrawNative` en el caso
común (sin escala) — el nombre y el propio código de `DrawNative` sugieren que es específicamente la
variante de dibujo CON ESCALA (`param_8==0x10`), probablemente reservada para casos puntuales (algún
ícono de HUD, un efecto con zoom), no el camino de cada sprite normal 1:1.

**Conclusión práctica**: parchear `DrawNative` no tocaría el costo real del motor (se llama ~0-1 veces por
frame, insignificante frente a ~100ms/frame de presupuesto). El pipeline real de sprites/tiles del hot
path sigue sin ubicarse — van dos rondas de arqueología de Ghidra (Fase 24 y Fase 29) que apuntaron a
candidatos plausibles por análisis estático y ambos fallaron la verificación empírica en hardware real.
Esto es exactamente el patrón que la metodología de este proyecto está diseñada para atrapar (medir antes
de parchear) — pero también es una señal honesta de que seguir por este camino tiene rendimientos
decrecientes: cada ronda adicional de Ghidra sin instrumentar puede volver a apuntar a un candidato
plausible-pero-equivocado.

**Nota de contexto sobre el FPS de esta sesión**: 6.4-7.2 FPS, más bajo que los ~9-11 FPS de las Fases
27/28 — esperado y no comparable directamente, ya documentado en la Fase 29 (el hook de conteo agrega
overhead real por invocación); no se lo debe interpretar como una regresión del build RGB565 directo
normal. También se repite por tercera vez el mismo patrón de caída puntual asociada a `loadFile:
Save0.dat` (Fases 27/28/30) — confirmado consistente, sigue sin investigarse la causa exacta por ser un
costo transitorio de una sola transición, no del piso sostenido.

**Pendiente de decisión explícita del usuario**: con dos candidatos de "parche de motor" ya descartados
por datos reales (¿20 primitivas de `CGsGraphics::`? no ubicado como hot path real; `DrawNative`?
descartado por frecuencia), seguir por esta vía requeriría una instrumentación más amplia (contar varias
funciones candidatas a la vez en vez de apostar a una por ronda) — más trabajo, sin garantía. La
alternativa sigue siendo aceptar ~9-11 FPS sostenido (build RGB565 directo normal, sin este hook) como
techo práctico de esta ronda y mover el foco a otra área del port.

**El usuario eligió explícitamente seguir buscando el hot path real, esta vez con un enfoque más amplio en
vez de otra apuesta a un solo candidato.**

## Fase 31: triangulación con 3 candidatos simultáneos — se encontró el sibling correcto de `DrawNative` (2026-07-18)

Investigación de seguimiento (tercera ronda): en vez de apostar a un candidato por ronda, se buscó (1)
todos los callers de `GcxGetMainScreenBuffer()`, (2) qué se asigna de verdad en el slot de vtable de
`CGxPZxAni` que la Fase 30 probó que casi nunca resuelve a `CGcxBitmap::Blt`/`DrawNative`, y (3) si existe
un renderer de tiles/mapa separado del de personajes.

**Hallazgo clave: `CGcxBitmap` (el candidato de la Fase 29) era el sibling equivocado — `CGxPZxZero` es el
real.** `CGxPZxAni::Draw`/`DrawFast` despachan a través de un puntero a función por instancia hacia una
implementación `Blt`/`BltFast`. La Fase 29 investigó `CGcxBitmap::Blt`/`BltFast` y encontró que `BltFast`
era un stub muerto — pero existe una clase HERMANA, `CGxPZxZero`, cuyos `Blt`/`BltFast`
(`out_ghidra.c:247337-247385`) son ambas implementaciones reales (no stubs) que llaman a
`PutCompressImg(...)` — casi con certeza el tipo concreto real usado para sprites comprimidos con color-key
("Zero") en el formato `.pzx` ("PZx"). `PutCompressImg` (`out_ghidra.c:224597`) es el despachador real:
switchea por `enumDrawOP` (19 modos registrados en `CMvGraphics::InitialBlend`, incluyendo un modo
`ENLARGE` con multiplicación de escala incorporada — mismo tipo de semántica de escala que tenía
`DrawNative`) y llama a uno de ~80 punteros a función globales (`g_funcZeroCopy`, `g_funcZeroBlend`, etc.)
que terminan en la familia `DrawOP_<MODO>_Compress_16*` — blitters reales por píxel/fila, no stubs.

No se encontró un renderer de tiles/mapa separado (`CMvMap`/`CZnField`/`CZnMap`/`Tile`/`CGsTile` sin
resultado de una clase de tile dedicada) — plausible que los tiles de mapa sean simplemente otra instancia
de `CGxPZxAni`/`CGxPZxZero` (capas de mapa también almacenadas como animaciones `.pzx` normales).

**Instrumentación de 3 niveles a la vez** (en vez de una apuesta por ronda, para triangular en una sola
sesión de consola): símbolos confirmados con `nm -D` sobre `lib/libgameDSO.so` (el `.so` real que carga
este port) y firmas confirmadas leyendo el cuerpo decompilado en `out_ghidra.c` directamente (no solo el
comentario de firma que genera Ghidra):

- `CGxPZxAni::Draw` (`_ZN9CGxPZxAni4DrawEii10enumDrawOPil`, `0x00159b10`) — punto de entrada garantizado,
  dispara SIEMPRE antes de cualquier despacho por vtable.
- `CGxPZxZero::Blt` (`_ZN10CGxPZxZero3BltEiiiiii10enumDrawOPil`, `0x00161e10`) — si el conteo aca es ~igual
  al de `CGxPZxAni::Draw`, confirma que el slot de vtable resuelve a esta clase en el caso común (la
  pregunta que quedó abierta tras la Fase 30).
- `PutCompressImg` (`_Z14PutCompressImgiiiiPhPt10enumDrawOPil`, `0x001509f8`) — si este conteo es MUCHO
  mayor que `CGxPZxZero::Blt`, hay otros callers además de `CGxPZxZero`.

Nueva opción de CMake `INSTRUMENT_HOTPATH_CALLS` (default OFF, mismo caveat que
`INSTRUMENT_DRAWNATIVE_CALLS`: overhead real por invocación, FPS no comparable) → `./build.sh
--hotpath-probe` → `build/zenonia_4_hotpath_probe.vpk`. Log extendido con
`avg_cgxpzxani_draw_per_frame`/`avg_cgxpzxzero_blt_per_frame`/`avg_putcompressimg_per_frame` junto a los
contadores ya existentes. Los 4 builds (`./build.sh`, `--neon-fallback`, `--drawnative-probe`,
`--hotpath-probe`) compilan sin errores ni warnings nuevos; confirmado con `strings` sobre el binario
compilado que los 3 símbolos y sus logs están presentes.

**Pendiente confirmar en consola**: instalar `build/zenonia_4_hotpath_probe.vpk` y jugar un rato — los 3
contadores nuevos dan por primera vez datos comparativos de las 3 hipótesis en una sola sesión, en vez de
una apuesta a la vez.

## Fase 32: primer crash real de la sesión — triage con log + `.psp2dmp`, causa confirmada y guardia aplicada (2026-07-18)

El usuario reportó que el build `zenonia_4_hotpath_probe.vpk` (Fase 31) crasheó a los ~7 FPS, con log
(`logs/log_1784351571.txt`) Y `.psp2dmp`
(`logs/Zenonia4-psp2core-1784351639-0x0006962ec1-eboot.bin.psp2dmp`) — la primera vez en esta sesión que
hay un crash real que triagear, siguiendo la metodología de la skill `so-crash-triage` (log primero, sin
adivinar, cruzar `.psp2dmp` + `.so` real + Ghidra).

**Datos del log antes del corte**: 2 checkpoints reales con los 3 contadores nuevos de la Fase 31 ya
mostrando actividad real —

| frame | fps | avg_cgxpzxani_draw | avg_cgxpzxzero_blt | avg_putcompressimg |
|---|---|---|---|---|
| 121 | 6.1 | 0.7 | 1.9 | 1.6 |
| 241 | 6.3 | 2.8 | 6.7 | 9.4 |

(Primera confirmación real de que `CGxPZxZero::Blt`/`PutCompressImg` SÍ se llaman varias veces por frame
en combate, subiendo con el tiempo — a diferencia de `DrawNative`, que se había descartado en la Fase 30.)

**Triage del `.psp2dmp`** (`vita-parse-core` contra el `.elf` sin strip de esa misma build, localizado en
`~/vita-tools/vita-parse-core`, confirmado via `CMakeCache.txt` que coincidía en flags — `RGB565_DIRECT_UPLOAD=OFF`,
`INSTRUMENT_DRAWNATIVE_CALLS=ON`, `INSTRUMENT_HOTPATH_CALLS=ON` simultáneos, ver el bug de script más
abajo):

- **Data abort**. `PC=0x98150a28` → dentro de `PutCompressImg`, offset **+0x30**. `LR=0x98161e51` → dentro
  de `CGxPZxZero::Blt`, offset **+0x41** (el retorno inmediato de la llamada a `PutCompressImg` dentro de
  `Blt` — confirma la cadena real que la Fase 31 acababa de instrumentar).
- Desensamblado (`arm-vita-eabi-objdump -d -M force-thumb`) de `PutCompressImg+0x30`:
  `ldr r3, [r2, #16]` con `r2=fp=5` en el momento del abort — **no es `NULL`, pero tampoco un puntero
  válido**.
- Cruzando con `out_ghidra.c:224597` (`PutCompressImg`): el noveno parámetro (`long param_9` — un puntero
  OPCIONAL a un framebuffer alternativo; `0`/`NULL` significa "usar `GcxGetMainScreenBuffer()`") se
  dereferencia con **solo un chequeo `== 0`**:
  ```c
  if (param_9 == 0) { /* usa GcxGetMainScreenBuffer() */ }
  else { local_2c = *(int *)(param_9 + 0x10); }  // <- crash real, param_9=5
  ```
  El motor nunca contempla "no es NULL pero tampoco es un puntero real" — mismo patrón de bug ya conocido
  para esta clase de ports (ver sección de patrones de la skill `so-crash-triage`).

**Se descartó que la instrumentación de la Fase 31 haya introducido este bug**: se desensambló
`CGxPZxAni::Draw` (`0x159b10`) completo — el call site real hacia el despacho por vtable
(`159b76: blx r4`) arma exactamente los mismos argumentos que Ghidra ya mostraba en su pseudo-C (6
valores: `this`, x, y, `enumDrawOP`, y los 2 últimos parámetros de `Draw` reenviados tal cual). Esto
confirma que el código real del motor ya arma la cadena de llamadas ANTES de que nuestro hook de conteo
corra — nuestros hooks solo leen y reenvían fielmente lo que el motor ya decidió pasar, sin tocar ningún
valor. El `5` inválido ya viene así desde el motor real (o desde algún wrapper intermedio entre el
despacho de 6 args de `Draw` y la firma real de 10 args de `Blt`, no identificado todavía) — es un bug
preexistente del port, no algo introducido por la Fase 31.

**Bug de script encontrado de paso**: `INSTRUMENT_DRAWNATIVE_CALLS` e `INSTRUMENT_HOTPATH_CALLS`
terminaron ambos en `ON` simultáneamente en el build que crasheó — `build.sh` no fijaba explícitamente los
3 flags de diagnóstico en cada invocación, así que el valor de una invocación anterior (`--drawnative-probe`)
quedó pegado en el cache de CMake al construir `--hotpath-probe` después, en el mismo `BUILD_DIR`. **Fix**:
`build.sh` ahora fija los 3 flags (`RGB565_DIRECT_UPLOAD`/`INSTRUMENT_DRAWNATIVE_CALLS`/
`INSTRUMENT_HOTPATH_CALLS`) explícitamente en ON u OFF en cada rama, nunca dejando ninguno sin mencionar.
Confirmado con `CMakeCache.txt` tras reconstruir los 4 builds que ya no hay bleed entre invocaciones.

**Fix del crash — guardia defensiva, SIEMPRE activa (no detrás de ningún flag de diagnóstico)**: nuevo hook
permanente sobre `PutCompressImg` (`dynlib.c`) que sanea `param_9` antes de dejar pasar la llamada — si no
es `0` pero está por debajo de una dirección de heap plausible (`< 0x1000`, guardia estándar tipo "página
cero" contra punteros chicos inválidos), se lo fuerza a `0` (usa el framebuffer principal, exactamente lo
mismo que el motor ya hace para el caso `NULL` legítimo). A diferencia de los hooks de conteo de la Fase
31 (que siguen detrás de `INSTRUMENT_HOTPATH_CALLS`, sin motivo para estar siempre activos), esta guardia
se instala en **todos los builds, incluyendo el normal** — es un fix de estabilidad real de gameplay, no
un instrumento de diagnóstico. Se evaluó el costo del ciclo `so_unhook`/`hook_addr` en cada llamada (el
mismo patrón usado para los hooks de conteo) — dado que `PutCompressImg` se llama en un solo dígito hasta
~10/frame (confirmado en esta misma sesión, tabla arriba), el overhead de flush de i-cache por llamada es
está acotado y se considera aceptable frente al riesgo real de un crash reproducible en combate.

Los 4 builds recompilados y verificados sin errores ni warnings nuevos; confirmado con `strings` que el
log `[PutCompressImgGuard] hook instalado...` está presente incluso en `build/zenonia_4.vpk` (el build
normal, sin ningún flag de diagnóstico).

**Pendiente confirmar en consola**: que el crash ya no se repita con la guardia activa. Si aparece el log
`[PutCompressImgGuard] param_9=... invalido` en algún momento, confirma que el bug real se sigue
disparando (y que la guardia lo está atajando) — si nunca aparece pero el crash tampoco se repite en una
sesión larga, es evidencia más débil pero también válida.

## Fase 33: confirmado en consola — sin crash, sesión completa, FPS estable 7.7-11.8 (2026-07-18)

Décimo séptimo log (`logs/log_1784354381.txt`), build `zenonia_4.vpk` normal (confirmado: sin `NEON`,
guardia de la Fase 32 instalada — `[PutCompressImgGuard] hook instalado en PutCompressImg`). El usuario
confirmó que ya no hubo crash, y reportó picos subjetivos de 13-14 FPS. Datos reales del log: 8 checkpoints
entre `fps=7.7` y `fps=11.8` (promedio ~9.6), sesión llegando al frame 961+ y terminando de forma limpia
(`saveFile: option.sav`, no un corte abrupto) — mejor que la sesión de la Fase 32 (6.1-6.3 FPS, que tenía
el overhead extra de los 2 hooks de diagnóstico pegados por el bug de cache del `build.sh`, ya arreglado).
`[PutCompressImgGuard] param_9=... invalido` **no apareció** en este log — no se puede confirmar si el bug
original se disparó de nuevo y la guardia lo atajó silenciosamente, o si simplemente no se volvió a dar la
condición esta sesión; de cualquier forma, no hubo crash.

**Aclaración pedida por el usuario**: no se removió ninguna vibración para ganar velocidad. `OnVibrate`
fue no-op desde las primeras fases del port (la Vita física no tiene el tipo de vibración que este
callback pide) — sigue disparándose igual (17 veces en este log), lo único tocado fue el TOPE de logging
de esas llamadas (Fase 25, a 40 apariciones por sesión, para no bloquear en disco sin límite), no la
vibración en sí.

---

## Fase 34: dos experimentos nuevos agregados desde `optimization_ideas.md`, TODAVÍA SIN MEDIR en consola (2026-07-18)

Nueva sesión, en paralelo a la decisión pendiente de la Fase 33 (seguir con el hot path real vs. aceptar
~9-11 FPS). El usuario pidió intentar el punto 4 de `optimization_ideas.md` (reemplazo de memcpy/memset
por NEON) y uno de los puntos 8/9/10 (audio en hilo esclavo, deshabilitar VSync, o downsampling
entrelazado). Antes de escribir nada se revisó qué existe realmente hoy en `loader/` (no `source/`, que
sigue siendo boilerplate sin usar — ver nota al pie de este archivo):

- **Punto 8 (audio en hilo esclavo) ya estaba hecho, no es una idea nueva**: `loader/audio.c` ya corre
  `audio_thread()` en un thread Vita dedicado (prioridad `0x10000100`) que decodifica Tremor (OggVorbis
  de punto fijo) y mezcla; `audio_play()` (llamado desde el thread principal via `OnSoundPlay`) sólo toma
  un mutex, copia unos campos de `struct voice_t` y lo suelta — la decodificación/mezcla real nunca corre
  en el thread principal. No se tocó nada acá: reimplementar esto sería trabajo redundante.
- Se descartó el punto 10 (downsampling entrelazado) para esta ronda: requiere parchear el rasterizador
  de software del motor con offsets reales de Ghidra (mismo tipo de riesgo que la Fase 31/32), no algo que
  se pueda armar sin una nueva ronda de instrumentación en consola.
- Se implementaron el punto 4 y el punto 9, **los dos apagados por default** (mismo criterio que
  `RGB565_DIRECT_UPLOAD` antes de su confirmación en la Fase 25 — ver `feedback_measure_before_patch` en
  memoria: nada de esto se promueve a default sin datos reales de consola primero):

  - **`NEON_LIBC_INTERCEPT`** (`CMakeLists.txt`): nuevo `loader/neon_mem.c`/`neon_mem.h` con
    `neon_memcpy`/`neon_memset` (loop manual `vld1q_u8`/`vst1q_u8` de 64 bytes desenrollado a 16, remanente
    byte a byte). En `loader/dynlib.c`, las entradas `"memcpy"`/`"memset"` de `default_dynlib[]` ahora se
    redirigen a estas dos funciones cuando `ZENONIA_NEON_LIBC` está definido; sin el flag, comportamiento
    idéntico al actual (libc de vitasdk sin modificar). `memmove` no se tocó (necesita manejar
    solapamiento, fuera de lo pedido). Build separado: `./build.sh --neon-libc-test` →
    `build/zenonia_4_neonlibc_test.vpk`.
  - **`DISABLE_VSYNC`** (`CMakeLists.txt`): en `gl_init()` (`loader/main.c`), si `ZENONIA_DISABLE_VSYNC`
    está definido, se llama `vglWaitVblankStart(GL_FALSE)` una vez después de `vglInitExtended()`, para
    descartar si `vglSwapBuffers()` esperando el refresco de pantalla es un techo artificial del FPS
    medido hoy (~9-11). Build separado: `./build.sh --no-vsync-test` → `build/zenonia_4_novsync_test.vpk`.

**Actualización misma sesión: se consiguió acceso a un `VITASDK` real (`/Users/metalsyntax/vitasdk`) y se
compilaron los dos builds.** Confirmado, no sólo asumido:
- `vglWaitVblankStart(GLboolean enable)` **sí existe** en la vitaGL vendorizada
  (`arm-vita-eabi/include/vitaGL.h:1184`) — la advertencia anterior sobre "agregado de memoria, sin poder
  confirmar el header" queda resuelta, el link no dio `undefined reference`.
- Los 3 builds (`zenonia_4.vpk` normal, `zenonia_4_neonlibc_test.vpk`, `zenonia_4_novsync_test.vpk`)
  compilan y linkean limpio, sin warnings nuevos respecto al build normal (los warnings preexistentes de
  `so_util.c`/FalsoJNI/`dynlib.c` ya estaban antes de esta sesión, no vienen de `neon_mem.c` ni de los
  cambios de esta fase).
- Verificado con `arm-vita-eabi-nm` sobre el ELF crudo de cada build (no sólo que CMake aceptó el flag):
  `neon_memcpy`/`neon_memset` aparecen como símbolos `T` (texto, definidos) reales en el build
  `--neon-libc-test`; el string `[PERF] VSync wait deshabilitado (DISABLE_VSYNC)...` aparece en el ELF del
  build `--no-vsync-test` (confirma que la rama `#ifdef ZENONIA_DISABLE_VSYNC` se compiló de verdad, no que
  el define quedó sin efecto).
- De paso, se notó que el parámetro de `vglSwapBuffers()` (la llamada ya existente en el loop principal,
  sin tocar en esta fase) se llama `has_commondialog` en el header real, no algo relacionado a vsync/fps
  como se había asumido antes de tener el header a mano — no cambia nada de lo implementado, sólo una
  corrección de lectura del API real vs. lo asumido de memoria.

**Lo que SIGUE pendiente** (esto no cambió: compilar limpio no es lo mismo que medir): correr
`build/zenonia_4_neonlibc_test.vpk` y `build/zenonia_4_novsync_test.vpk` en consola real (o Vita3K) en el
mismo tramo de combate usado en las Fases 25/27/33, y comparar el FPS logueado contra el build normal antes
de considerar promover cualquiera de los dos a default — ver `feedback_measure_before_patch` en memoria.
`build.sh` ya fija los 5 flags de CMake (`RGB565_DIRECT_UPLOAD`, `INSTRUMENT_DRAWNATIVE_CALLS`,
`INSTRUMENT_HOTPATH_CALLS`, `NEON_LIBC_INTERCEPT`, `DISABLE_VSYNC`) de forma explícita en las 6 ramas
(normal + 5 modos), siguiendo el fix de cache-bleed de la Fase 32.

---

## Estado al cierre de esta sesión (2026-07-18) — para retomar

**Build actual**: `build/zenonia_4.vpk` (normal, **RGB565 directo, default desde Fase 26** — confirmado en
DOS sesiones de consola real: ~9-11 FPS sostenido en gameplay, caída transitoria a ~6-7 en la transición
menú→gameplay por `loadFile`, sin errores de color; **ahora incluye también la guardia de crash de la Fase
32 sobre `PutCompressImg`, siempre activa**), `build/zenonia_4_neonfallback.vpk` (build de respaldo con la
conversión NEON de la Fase 22), `build/zenonia_4_drawnative_probe.vpk` (diagnóstico sobre `DrawNative` —
**hipótesis descartada en la Fase 30**, código conservado por si se retoma) y
`build/zenonia_4_hotpath_probe.vpk` (diagnóstico con 2 hooks de conteo restantes:
`CGxPZxAni::Draw`/`CGxPZxZero::Blt` — el conteo de `PutCompressImg` ahora vive en el hook siempre-activo de
la Fase 32, ya no depende de este flag) — con todos los fixes de las Fases 1-32.
`build/zenonia_4_debug_verbosejni.vpk` (verbose JNI) ya no está en `build/` — se puede regenerar con
`cmake -DENABLE_VERBOSE_JNI_LOG=ON -DVPK_OUTPUT_NAME=zenonia_4_debug_verbosejni` si hace falta de nuevo,
`build/` está en `.gitignore` así que no se pierde nada versionado. El juego arranca, llega a gameplay
real, guarda partida correctamente, sonido funciona, cruceta/botones táctiles ocultos, sin zoom, a
pantalla completa 960×544. Ya tiene medición de FPS real (Fase 25), conteo de subidas de framebuffer por
frame (Fase 27, descartado en Fase 28), y ahora una guardia real de crash contra un bug confirmado con
`.psp2dmp` (Fase 32) para seguir avanzando con datos reales en vez de suposiciones.

**IMPORTANTE — `build.sh` cambió de comportamiento (Fase 32)**: los 3 flags de diagnóstico
(`RGB565_DIRECT_UPLOAD`/`INSTRUMENT_DRAWNATIVE_CALLS`/`INSTRUMENT_HOTPATH_CALLS`) ahora se fijan
explícitamente en cada invocación — antes, invocar `./build.sh --drawnative-probe` y después
`./build.sh --hotpath-probe` (mismo `BUILD_DIR`) dejaba ambos flags en `ON` a la vez sin querer (así
crasheó el build de la Fase 31/32). Si vas a instalar cualquiera de los builds de diagnóstico, reconstruilo
de nuevo con el `build.sh` actualizado antes de asumir que coincide con lo documentado en fases anteriores.

**Decisión pendiente para la próxima sesión**: (1) confirmar en consola que el crash de la Fase 32 ya no
se repite con la guardia activa — instalar `build/zenonia_4.vpk` (normal, ya con la guardia) y jugar hasta
llegar al menos al mismo punto donde crasheó antes; (2) si querés seguir con el hot path real, instalar
`build/zenonia_4_hotpath_probe.vpk` (reconstruido con el fix del cache-leak) para conseguir
`avg_cgxpzxani_draw_per_frame`/`avg_cgxpzxzero_blt_per_frame` — ya tenemos un dato parcial de la Fase 32
(0.7-2.8 y 1.9-6.7 respectivamente, subiendo con el tiempo) que sugiere que SÍ hay actividad real, pero el
log se cortó por el crash antes de un tercer checkpoint. Con esos números:
- Si siguen subiendo a decenas/frame: confirma que el motor SÍ dibuja muchos sprites por frame vía esta
  clase — recién ahí tendría sentido evaluar un parche de escala real sobre `PutCompressImg` (que ya tiene
  un modo `ENLARGE` con escala incorporada, mismo patrón que tenía `DrawNative`).
- Si se estabilizan bajo (como `DrawNative` en la Fase 30): confirma que el hot path real todavía no se
  ubicó — en ese punto la recomendación es aceptar ~9-11 FPS sostenido (build normal) como techo práctico y
  mover el foco a otra área del port, en vez de seguir invirtiendo en rondas adicionales de arqueología de
  Ghidra con retornos decrecientes.

El camino de bajar `GAME_W`/`GAME_H` directamente sigue descartado (Fase 23/24) independientemente de qué
se elija.

**Agregado en la Fase 34**: dos builds experimentales nuevos, cada uno apagado por default y aislado del
build normal — `./build.sh --neon-libc-test` (`NEON_LIBC_INTERCEPT`, redirige `memcpy`/`memset` a
`loader/neon_mem.c`) y `./build.sh --no-vsync-test` (`DISABLE_VSYNC`, salta la espera de vblank en
`gl_init()` vía `vglWaitVblankStart`, confirmado presente en la vitaGL vendorizada). Ambos ya se
compilaron y linkearon limpio en esta misma sesión (`VITASDK` real disponible en
`/Users/metalsyntax/vitasdk`), y se confirmó con `nm`/`strings` que el código de cada flag realmente
quedó en el binario -- lo único que falta es instalarlos y medir FPS en consola/Vita3K en el mismo tramo
de combate de las Fases 25/27/33. Ver el detalle completo en la Fase 34 arriba.

## Fase 35: primeros datos reales de consola para `--neon-libc-test` y `--no-vsync-test` — ninguno de los dos mejora el FPS medido (2026-07-18)

Dos logs nuevos del usuario: `log_1784423342.txt` (build `--neon-libc-test`) y `log_1784423510.txt` (build
`--no-vsync-test`). Ambos limpios — sin `FATAL`, sin crash, sin `[PutCompressImgGuard] param_9=...
invalido` — así que ninguno de los dos cambios introdujo una regresión de estabilidad.

**Números reales (`fps=` del log, mismo formato que las Fases 25/27/33)**:

| Build | Checkpoints | FPS prom. | Rango | Llegó a frame |
|---|---|---|---|---|
| Normal (Fase 33, referencia) | 8 | ~9.6 | 7.7–11.8 | 961+ |
| `--neon-libc-test` | 8 (121..961) | **9.54** | 7.8–11.4 | 961 (sesión completa) |
| `--no-vsync-test` | 5 (121..601) | **10.00** | 5.9–12.2 | ~601-700 (sesión más corta) |

**Lectura honesta, no la impresión subjetiva del usuario** ("NEON se sintió más estable y rápido que
VSync" — percepción real, pero el log no la confirma en FPS promedio):
- `--neon-libc-test` corrió la sesión completa (8 checkpoints hasta frame 961, igual que la referencia) y
  su FPS promedio (9.54) es prácticamente idéntico al build normal (9.6) — dentro del ruido, sin mejora
  medible. Es una comparación limpia (misma duración de sesión que la referencia).
- `--no-vsync-test` promedia más alto (10.00) pero con una sesión más corta (solo 5 checkpoints, no llegó
  a los 961 frames de la referencia) y más varianza (5.9 a 12.2) — muestra insuficiente para afirmar una
  mejora real. Consistente con lo anticipado en la Fase 34: a ~90-110ms de frametime,
  `sceDisplayWaitVblankStart()` probablemente ya retornaba casi inmediato, así que saltarse la espera no
  tenía mucho margen para ganar — esto es un resultado negativo real (descarta VSync como el techo
  artificial), no un fallo del experimento.
- La sensación de "más estable" con NEON es plausible aunque el promedio no se mueva: el log solo
  registra FPS promedio por ventana de 120 frames, no varianza de frame-time/stutter — reemplazar
  memcpy/memset por NEON reduciría el costo por-llamada de forma más pareja (menos tiempo total en esas
  funciones) sin necesariamente mover el promedio si el cuello de botella real sigue siendo otro (el
  render por software, ver hot path pendiente de las Fases 29-33).

**El usuario preguntó si conviene combinar ambos flags.** Son dos rutas de código independientes (tabla de
imports `memcpy`/`memset` en `dynlib.c` vs. una llamada a `vglWaitVblankStart` en `gl_init()`) sin ninguna
dependencia entre sí, así que combinarlas es seguro. Se agregó `./build.sh --neon-novsync-test`
(`NEON_LIBC_INTERCEPT=ON` + `DISABLE_VSYNC=ON` simultáneos) → `build/zenonia_4_neonnovsync_test.vpk`,
compilado y linkeado limpio en esta sesión, confirmado con `nm`/`strings` que ambas rutas de código
(`neon_memcpy`/`neon_memset` como símbolos reales, y el log `[PERF] VSync wait deshabilitado...`) están
presentes juntas en el mismo binario.

**Pendiente**: instalar `build/zenonia_4_neonnovsync_test.vpk` y jugar una sesión de la misma duración que
la referencia (hasta frame 961+ como mínimo, para que la comparación sea limpia) — dado que ninguno de los
dos flags mostró una mejora clara por separado, no hay razón fuerte para esperar que la combinación sí la
muestre, pero es información real y barata de conseguir. Si tampoco muestra mejora, la conclusión práctica
sería la misma que al cierre de la Fase 33: ~9-11 FPS es el techo mientras el render siga siendo por
software en CPU, y el camino con más potencial real sigue siendo terminar la investigación del hot path
(`CGxPZxAni::Draw`/`CGxPZxZero::Blt`/`PutCompressImg`, `build/zenonia_4_hotpath_probe.vpk`) en vez de
seguir micro-optimizando funciones de libc o el vsync.

## Fase 36: tres implementaciones nuevas de `optimization_ideas.md` antes de seguir midiendo — afinidad de CPU, dirty-rect upload, instrumentación de CGsGraphics:: (2026-07-18)

El usuario pidió explícitamente NO instalar `--neon-novsync-test` todavía, y en cambio implementar al
menos 3 ideas más (mapeadas o no) de `optimization_ideas.md` antes de la próxima ronda de medición. Se
evaluaron todas las pendientes y se descartaron varias por no poder confirmarse con confianza sin más
datos (ver razones abajo) — se implementaron las 3 que sí se pudieron verificar con seguridad:

**Descartadas para esta ronda (con motivo, no solo omitidas)**:
- **#1 GPU Hooking** y **#10 Checkerboard**: ambas requieren parchear directamente el rasterizador de
  software con offsets reales de Ghidra — mismo tipo de riesgo que la Fase 31/32, no algo para hacer sin
  una ronda de instrumentación en consola primero.
- **#3 PBO/DMA async**: vitaGL expone `glBindBuffer`/`glBufferData`/`glMapBuffer`, pero sin el código
  fuente de vitaGL (no vendorizado en este árbol, no encontrado en `~/vitasdk-src`) no se pudo confirmar
  que `glTexSubImage2D` realmente lea de un `GL_PIXEL_UNPACK_BUFFER` en vez de ignorarlo — implementar esto
  arriesgaba escribir un flag que compila y linkea pero no hace nada real (violaría el criterio de medir
  antes de patchear: no se puede medir un efecto cuya existencia ni siquiera está confirmada).
- **#5 Pre-descompresión de `.pzx`**: requiere reversear el formato propietario de compresión "Zero" del
  motor, alcance demasiado grande para esta ronda.
- **#7 JIT**: no realista de implementar con confianza en esta sesión.
- **Wordwrap** ("Lo que NO se ha hecho" #4): se leyó `NexusFont.java` decompilado
  (`decompiled/apk_jadx/sources/com/gamevil/nexus2/NexusFont.java`) para confirmar si `GFA_SetTextSize`
  usaba una escala EM distinta a la de `stbtt_ScaleForMappingEmToPixels` (la sospecha inicial) — pero el
  comentario ya existente en `loader/font.c` confirma que esa distinción ya se investigó y resolvió
  correctamente en una fase anterior. Sin un log real capturando la línea `[GFA][diag] wordwrap
  sospechoso: ...` (el diagnóstico existente solo dispara una vez por sesión si el patrón aparece, y no se
  compartió ninguno), no hay datos nuevos para derivar un fix con confianza — se deja como está.

**Implementadas (las 3 que sí se pudieron verificar con confianza)**:

1. **#6 Afinidad de CPU / desbloqueo de núcleos** (`loader/main.c`, `loader/audio.c`): el hilo principal
   (render loop) se fija a `SCE_KERNEL_CPU_MASK_USER_0` en `gl_init()` vía
   `sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), ...)`, y el hilo mezclador de audio (ya
   corría en su propio thread, ver Fase 34/idea #8) se fija a `SCE_KERNEL_CPU_MASK_USER_1` en
   `audio_init()` — para que no compitan por el mismo núcleo. Mismo criterio que el boost de reloj
   (`scePowerSetArmClockFrequency` etc.): optimización estándar de homebrew Vita, sin gate detrás de
   ningún flag de CMake, activa en el build normal.
2. **#2 Dirty-rect upload** (`loader/dynlib.c`, nuevo `upload_rgb565_dirty_rect()`): detrás de
   `DIRTY_RECT_UPLOAD` (default OFF, requiere `RGB565_DIRECT_UPLOAD=ON`), compara el framebuffer RGB565
   fila por fila contra una copia cacheada del frame anterior y sube solo el rango `[primera fila distinta,
   última fila distinta]` vía `glTexSubImage2D`, en vez de las 544 filas completas. Se había descartado
   esta idea en un primer análisis (razonando que "solo 1 upload/frame" ya confirmado en la Fase 27/28
   implicaba que no había nada que diffear) — error corregido: esa cifra solo dice CUÁNTAS veces se llama
   `glTexSubImage2D` por frame, no CUÁNTO de la imagen cambió; con el puntero de píxeles ya en la mano en
   el wrapper existente, el diffing es autocontenido y verificable (a diferencia de PBO, no depende de
   semántica no confirmada de vitaGL). Nuevo contador `avg_dirtyrect_pct_uploaded` en el log. Build de
   prueba: `./build.sh --dirtyrect-test` → `build/zenonia_4_dirtyrect_test.vpk`.
3. **"Lo que NO se ha hecho" #3, evaluar el pipeline de `CGsGraphics::`** (`loader/dynlib.c`, nuevos hooks
   de solo-conteo): a diferencia de `DrawNative` (Fase 29/30, resuelto por despacho de vtable invisible a
   `nm`), se confirmó con `nm -D` sobre `lib/libgameDSO.so` que 13 primitivas de `CGsGraphics::` (DrawLine,
   DrawFillRect, DrawRoundRect, DrawFillRoundRect, DrawEllipse ×2, DrawFillEllipse, ClearScreen,
   BeginScreen, FastClearNonClipping, GetFrameBufferPtr, PutPixel ×2) SÍ son símbolos exportados
   directamente hookeables — bajo riesgo de hookear la función equivocada. Se dejaron afuera a propósito
   las que reciben structs por valor (`DrawTextInRect` con `TGXRECT`, `DrawTextBBF`/`DrawTextSystemFont`)
   por no tener el layout real de esos tipos confirmado — hookear esas firmas arriesgaría desalinear el
   paso de argumentos ARM AAPCS. Contador único agregado (`avg_cgsgraphics_per_frame`, no uno por
   primitiva) detrás de `INSTRUMENT_CGSGRAPHICS_CALLS` (default OFF). Build de prueba:
   `./build.sh --cgsgraphics-probe` → `build/zenonia_4_cgsgraphics_probe.vpk`.

**Verificado en esta sesión (con `VITASDK` real disponible)**: los 3 builds nuevos
(`zenonia_4.vpk` normal con la afinidad de CPU ya activa, `zenonia_4_dirtyrect_test.vpk`,
`zenonia_4_cgsgraphics_probe.vpk`) compilan y linkean limpio, sin warnings nuevos respecto a los builds
previos. Confirmado con `nm`: los 13 hooks de `CGsGraphics_*_counter_hook` están presentes como símbolos
reales en el build `--cgsgraphics-probe`; los strings `[PERF] Afinidad de CPU...` y `[AUDIO] mezclador
iniciado ..., afinidad->nucleo 1...` están presentes en el build normal (`CMakeCache.txt` de
`--dirtyrect-test` confirma `DIRTY_RECT_UPLOAD:BOOL=ON`; `upload_rgb565_dirty_rect` no aparece como símbolo
propio porque el optimizador `-O3` de `Release` la inlineó en su único call site, comportamiento esperado,
no un indicio de que el código no se compiló).

**Nada de esto se midió todavía en consola real** — eso es justamente lo que el usuario pidió posponer.
Antes de instalar cualquiera de los 4 VPKs pendientes (`--neon-novsync-test` de la Fase 35, más los 2
nuevos de esta fase + el build normal con afinidad de CPU), confirmar que ninguno introduce una regresión
de estabilidad (buscar `FATAL`/crash en el log) y comparar `avg_dirtyrect_pct_uploaded`/
`avg_cgsgraphics_per_frame` contra la hipótesis de cada idea antes de sacar conclusiones de FPS.

## Fase 37: el crash de `--cgsgraphics-probe` NO es un bug de CGsGraphics — es el mismo crash de la Fase 32, con la causa raíz real encontrada (bug de caché de instrucciones en `so_util.c`) (2026-07-18)

El usuario reportó un crash en el build `--cgsgraphics-probe` (`log_1784426270.txt` +
`psp2core-1784351639-0x0006962ec1-eboot.bin.psp2dmp`) y pidió arreglarlo antes de sumarlo al build
combinado. Triage completo siguiendo la skill `so-crash-triage`:

**El log**: se corta justo después de `[Java] loadFile: SaveFlag.sav -> 4 bytes` — antes de que aparezca
siquiera el primer checkpoint `frame 1 alive` (o sea, durante la inicialización/pantalla de título, no en
gameplay real). El instalador de los 13 hooks de `CGsGraphics::` confirma `13/13 hooks de conteo
instalados` sin ningún error — el probe en sí se instaló bien.

**El `.psp2dmp`** (parseado con `vita-parse-core` contra el `.elf` de este mismo build,
`RGB565_DIRECT_UPLOAD=ON`/`INSTRUMENT_CGSGRAPHICS_CALLS=ON`/resto OFF confirmado en `CMakeCache.txt`):
Data abort, **`PC=0x98150a28`, `LR=0x98161e51`, `R2=0x5`** — offsets contra los símbolos reales del `.so`
(`nm -D`) confirman `PC` = `PutCompressImg+0x30` y `LR` = `CGxPZxZero::Blt+0x41`, **exactamente las mismas
direcciones y el mismo valor de registro (`param_9=5`) que el crash ya documentado en la Fase 32.** No es
un bug nuevo de `CGsGraphics::` — es el bug de la Fase 32 recurriendo, a pesar de que la guardia
(`PutCompressImgGuard`) estaba confirmada instalada en el log (`[PutCompressImgGuard] hook instalado en
PutCompressImg`). El log tampoco muestra NUNCA la línea `[PutCompressImgGuard] param_9=... invalido` en
toda la sesión — la guardia nunca vio pasar un `param_9` inválido por su propio código, pese a que el
dump prueba que la ruta insegura (`param_9=5` sin sanear) sí se ejecutó.

**Causa raíz real, no en `dynlib.c` sino en `loader/so_util.c`**: `so_unhook()` restaura los bytes
originales de una función Y flushea el caché de instrucciones después
(`ku_flush_caches`) — pero `hook_thumb()`/`hook_arm()` (las funciones que **reinstalan** el hook, llamadas
al final de cada invocación de un hook tipo "unhook → llamar original → rehook", el mismo patrón que usan
`PutCompressImg_hook`, los 3 contadores de la Fase 31 y los 13 nuevos de `CGsGraphics::`) escriben los
bytes del parche pero **nunca llaman a `ku_flush_caches` después**. Esto deja una ventana real: después de
que la guardia reinstala su propio hook al final de una invocación, el caché de instrucciones del CPU
puede seguir sirviendo los bytes ORIGINALES (los que `so_unhook()` acaba de flushear un instante antes)
en vez de los nuevos bytes de parche recién escritos — la CPU nunca se entera de que la memoria volvió a
cambiar. El resultado práctico: la guardia protege confiablemente la PRIMERA llamada a `PutCompressImg`
después de instalarse al arrancar, pero cualquier llamada POSTERIOR corre el riesgo real de ejecutar el
código original SIN PASAR por la guardia — hasta que algo más (por casualidad) flushee esa región de
memoria. Con `PutCompressImg` llamado varias veces por frame (confirmado en las Fases 31/32, 0.7-9.4
veces/frame subiendo con el tiempo), el bug es prácticamente garantizado a manifestarse tarde o temprano
en cualquier sesión donde el motor vuelva a pasar un `param_9` inválido — exactamente lo que pasó acá, y
lo que probablemente también le pasó a la sesión de la Fase 32 original. La sesión "limpia" de la Fase 33
no fue evidencia de que la guardia funcionara: fue simplemente que esa sesión en particular no volvió a
generar un `param_9` inválido después de la primera llamada protegida.

**Fix real, aplicado en la fuente del problema (`loader/so_util.c`), no en `dynlib.c`**: se agregó
`ku_flush_caches(addr, sizeof(patch_instr))` al final de `hook_thumb()` y `hook_arm()`, inmediatamente
después de escribir los bytes del parche — mismo flush que `so_unhook()` ya hacía para el camino inverso.
Esto arregla el problema de raíz para **todos** los hooks tipo "reinstalar en caliente" del proyecto
(la guardia de `PutCompressImg`, los 3 contadores de hot path de la Fase 31, y los 13 nuevos de
`CGsGraphics::`), no solo para este caso puntual — no hacía falta (ni convenía) parchear cada hook
individualmente cuando el bug real vivía en la función compartida que todos usan.

**Verificado en esta sesión**: `zenonia_4.vpk` (normal) y `zenonia_4_cgsgraphics_probe.vpk` se
recompilaron con el fix, ambos compilan y linkean limpio (mismos warnings preexistentes de siempre, uno
nuevo de `so_util.c` -- `'h' may be used uninitialized` -- ya estaba antes del fix, no lo introdujo).
**Pendiente confirmar en consola** que el crash ya no se repite con el fix — mismo criterio que la
Fase 33 (jugar hasta pasar el punto donde crasheó antes; si aparece `[PutCompressImgGuard] param_9=...
invalido` en el log, mejor evidencia todavía de que la guardia ahora sí actúa de forma confiable en
llamadas repetidas).

**Nota aparte, no relacionada al crash**: el log de esta sesión muestra
`[PERF] Afinidad de CPU: hilo principal (0x40010003) -> nucleo 0 (res=0x00070000)` — el valor de `res` no
es `0`, lo que según la documentación del SDK (`@return 0 if successful, otherwise the error code`)
indicaría que `sceKernelChangeThreadCpuAffinityMask` no tuvo éxito para el hilo principal (el mezclador de
audio no se pudo confirmar en este log porque el crash ocurrió antes de que `audio_init()` llegara a
loguear su propio `res`). No se investigó más a fondo esta sesión — no parece estar relacionado al crash
(la asignación de afinidad de CPU y el guard de `PutCompressImg` son mecanismos completamente
independientes), pero vale la pena confirmar en el próximo log si `0x00070000` es realmente un código de
error o si el comentario del header de VitaSDK no es exacto para esta función.

**Sobre combinar todo en un VPK (pedido del usuario)**: se agregó `./build.sh --all-perf-test` →
`build/zenonia_4_allperf_test.vpk`, combinando `NEON_LIBC_INTERCEPT` + `DISABLE_VSYNC` +
`DIRTY_RECT_UPLOAD` + `INSTRUMENT_HOTPATH_CALLS` (los tres candidatos reales de rendimiento más la
instrumentación de conteo de hot path, para seguir juntando datos de
`CGxPZxAni::Draw`/`CGxPZxZero::Blt`/`PutCompressImg` mientras tanto) — compilado y linkeado limpio,
confirmado con `nm`/`strings` que las 4 rutas de código conviven en el mismo binario.
`INSTRUMENT_CGSGRAPHICS_CALLS` se dejó OFF a propósito en este build, tal como pidió el usuario: arreglar
el crash primero (hecho arriba) y confirmarlo en consola antes de sumarlo a la mezcla. **Advertencia real
para la interpretación de este build**: el FPS que se mida con `--all-perf-test` NO es comparable al build
normal — `INSTRUMENT_HOTPATH_CALLS` agrega el mismo overhead real por llamada que ya tenía
`--hotpath-probe` en solitario (mismo caveat de siempre). Lo único comparable de este build son los 3
contadores de llamadas (no dependen del FPS) y una confirmación cualitativa de que los 3 flags de
rendimiento no interactúan mal entre sí ni crashean juntos.

## Fase 38: segundo crash de `--cgsgraphics-probe` — esta vez SÍ era un bug real mío (retorno de 64 bits mal forwardeado), más datos reales muy buenos de `--all-perf-test` (2026-07-18)

**Segundo crash reportado**, mismo build (`--cgsgraphics-probe`, ya con el fix de caché de la Fase 37),
log `log_1784427800.txt` + `psp2core-1784426284-...psp2dmp`. Esta vez **NO** es el bug de la Fase 32:

- **Prefetch abort** (no data abort), `PC=0x98195e80` (no resuelve a ningún símbolo real cercano —
  ejecución saltó a memoria inválida), `LR=0x9809b3c7` = `CGsGraphics::EndAlpha+0x1f`. `R10`/`R12`
  = `0xdeadbeef` (patrón clásico de memoria envenenada/sin inicializar).
- Leyendo `CGsGraphics::EndAlpha`/`BeginAlpha` en `out_ghidra.c`: ambas leen/escriben un campo de
  contexto en `this+0xc` y lo pasan a `MC_grpSetContext` — si ese campo tiene basura, `MC_grpSetContext`
  termina saltando a una dirección inválida (coincide exactamente con el Prefetch abort observado).
- **Causa real, confirmada contra `out_ghidra.c` función por función para las 13 primitivas
  instrumentadas**: `CGsGraphics::BeginScreen`, `ClearScreen`, `DrawFillEllipse` y
  `FastClearNonClipping` **devuelven `undefined8`** (8 bytes reales, par de registros `r0:r1` en ARM
  AAPCS) — a diferencia de las otras 9 primitivas (confirmadas `void` una por una). Mis 13 hooks
  originales (Fase 36) declaraban y forwardeaban el retorno como `int` (solo `r0`) para las 13 por
  igual, razonando que "forwardear de más siempre es inofensivo" — cierto para funciones `void`, pero
  **falso para estas 4**: al declarar mi propio hook como retornando `int`, el compilador nunca
  garpolaba `r1` al volver — quedaba con lo que fuera que mi hook hubiera dejado ahí (basura de
  stack/registro, compatible con el patrón `0xdeadbeef` observado). Como `BeginScreen`/`ClearScreen`
  corren una sola vez al arrancar cada pantalla de render y su `r1` alimenta el estado que
  `BeginAlpha`/`EndAlpha` leen después, la mitad alta corrompida se propagaba directo a el bug real.
- **Fix**: los 4 hooks (`CGsGraphics_BeginScreen_counter_hook`, `..._ClearScreen_...`,
  `..._DrawFillEllipse_...`, `..._FastClearNonClipping_...`, `loader/dynlib.c`) ahora declaran y
  forwardean `long long` (64 bits reales, `r0:r1`) en vez de `int` — mismo patrón de llamada-a-través
  que los otros 9, pero con el tipo de retorno correcto. Las otras 9 (confirmadas `void`) no se
  tocaron. Recompilado (`--cgsgraphics-probe`), limpio, sin warnings nuevos.
- **Pendiente confirmar en consola** que el crash no se repite con este segundo fix.

**`--all-perf-test` — el usuario confirmó "no hizo nada mejorable", y el log real coincide, pero trajo
un hallazgo mucho más importante que el FPS**: `log_1784427866.txt`, sin crash, sesión completa hasta
frame 361+ (terminó porque el usuario cerró la sesión, no por un corte).

| frame | fps | avg_cgxpzxani_draw | avg_cgxpzxzero_blt / avg_putcompressimg | avg_dirtyrect_pct_uploaded |
|---|---|---|---|---|
| 121 | 9.2 | 1.3 | 14.7 | 27.2% |
| 241 | 8.1 | 32.5 | **445.1** | 87.2% |
| 361 | 9.4 | 35.5 | **546.6** | 98.0% |

- **FPS promedio ~8.9** — plano contra la referencia (~9.6), tal como predijo cada flag por separado
  (Fase 35), y encima con el overhead real de `INSTRUMENT_HOTPATH_CALLS` sumado (ahora con un flush de
  caché real en cada reinstalación de hook, Fase 37 — un costo que antes NO se estaba pagando por el bug,
  así que este build en particular paga más overhead de instrumentación que builds anteriores con el
  mismo flag). Consistente con la sospecha del usuario ("no hizo nada mejorable"): ninguno de los 3 flags
  de rendimiento reales mostró ganancia individualmente, así que la combinación tampoco tenía motivo para
  mostrarla.
- **El hallazgo real y bueno**: `avg_cgxpzxzero_blt_per_frame`/`avg_putcompressimg_per_frame` treparon de
  14.7 a **445-547 llamadas por frame** entre el frame 121 y el 361 — muchísimo más alto que los datos
  parciales de la Fase 31/32 (0.7-9.4/frame). Esto es la primera confirmación real y contundente de que
  el hot path (`CGxPZxZero::Blt`→`PutCompressImg`) SÍ es el pipeline real de sprites en combate, llamado
  cientos de veces por frame — el candidato con más potencial real de la lista completa de
  `optimization_ideas.md` (el parche de escala sobre `PutCompressImg`, que ya tiene un modo `ENLARGE`
  nativo, idea original #1) pasa de "hipótesis sin confirmar" a "confirmado con números reales, vale la
  pena intentarlo". También notable: `avg_cgxpzxani_draw_per_frame` (35.5) es ~15x menor que
  `avg_cgxpzxzero_blt_per_frame` (546.6) — confirma la sospecha de la Fase 31 de que hay OTROS callers de
  `Blt`/`PutCompressImg` además de `CGxPZxAni::Draw` (probablemente el renderizado de fondo/tiles), sin
  identificar todavía cuáles.
- **`avg_dirtyrect_pct_uploaded` sube de 27.2% a 98.0%** en la misma ventana — confirma exactamente la
  advertencia de la Fase 36: en combate real (acción sostenida), el motor redibuja prácticamente toda la
  pantalla cada frame, así que el diffing de dirty-rect converge a "casi sin ahorro" justo cuando más
  importaría. El 27% del frame 121 probablemente corresponde a un momento más tranquilo (menú/transición).
  Esto no invalida la idea (sigue siendo correcta y segura), pero sugiere que su beneficio real es
  marginal en el peor caso (combate), no el "ahorro dramático" que planteaba la idea original.

## Fase 39: tercer crash de `--cgsgraphics-probe` — causa raíz REAL encontrada en `hook_thumb()` (bug de alineación de 2 bytes, vendorizado, no de la Fase 36) (2026-07-18)

Tercer crash reportado (`log_1784428695.txt` + `Zenonia4-psp2core-1784428714-...psp2dmp`), mismo build,
**exactamente el mismo crash byte por byte** que la Fase 38 (`PC=0x98195e80`, `LR=0x9809b3c7`,
`R10`/`R12`=`0xdeadbeef`, mismo stack) — pese al fix de retorno de 64 bits ya aplicado. Esto por sí solo
ya era una señal fuerte de que ese fix, aunque correcto y necesario, no era la causa de ESTE crash.

**Triage más profundo, desensamblado real (no solo pseudo-C) con `arm-vita-eabi-objdump -M force-thumb`**:
- `PC=0x98195e80` no cae en ninguna función real — cae dentro de la sección **`.got`** (Global Offset
  Table, confirmado con `objdump -h`: `.got` va de `0x195044` a `0x196000`). Ejecutar instrucciones desde
  ahí es la firma clásica de un salto a través de un puntero corrompido/no inicializado.
- `LR=CGsGraphics::EndAlpha+0x1f` resultó ser un dato **residual, no relevante** — el desensamblado real
  de `EndAlpha`/`MC_grpSetContext` confirma que son código trivial (una tabla de `if/store`, sin llamadas
  indirectas ni saltos calculados) que no puede producir un Prefetch abort por sí solo.
- La pista real: `R9=0x9809bf77` resultó ser **la dirección de `CGsGraphics::DrawFillRect` con el bit de
  Thumb puesto** — uno de los 13 hooks instrumentados. Desensamblando `DrawFillRect` real: su prólogo es
  `push {r4,r5,r6,r7,lr}` y su epílogo `pop {r4,r5,r6,r7,pc}` — si el prólogo NUNCA se ejecuta pero el
  epílogo SÍ, el `pop` final saca del stack lo que sea que haya ahí (no lo que el prólogo debería haber
  guardado) hacia `r4-r7` y `pc` — coincide EXACTO con `R10`/`R12=0xdeadbeef` (memoria de stack sin
  inicializar) y con `PC` terminando en una dirección "plausible pero inválida" (una lectura de stack, no
  un valor realmente al azar).
- **Causa raíz confirmada, en `loader/so_util.c` (código vendorizado, no algo escrito para Zenonia 4)**:
  `hook_thumb()` verifica si la dirección del target no cae en un límite de 4 bytes
  (`if (addr & 2)`) y, si no, escribe un NOP de 2 bytes en la ENTRADA de la función para "correrla" 2
  bytes antes de instalar el trampolín de 8 bytes — pero **nunca guardaba los 2 bytes ORIGINALES que ese
  NOP reemplaza**. `so_unhook()` solo restauraba los 8 bytes del trampolín (a partir de la dirección ya
  corrida +2), nunca esos 2 bytes iniciales — quedaban reemplazados por el NOP **para siempre**, desde la
  primera vez que la función se hookeaba. Confirmado con aritmética directa sobre las 13 direcciones
  hookeadas: exactamente **3 de las 13 caen en `addr & 2 != 0`** — `DrawFillRect` (`0x9bf76`),
  `DrawEllipse` de 7 argumentos (`0x9c17e`), y `PutPixel` de 3 argumentos (`0x9b972`) — las otras 10 caen
  en un límite de 4 bytes y nunca dispararon este camino de código. Ninguno de los hooks preexistentes
  (`PutCompressImg`, `CGxPZxAni::Draw`, `CGxPZxZero::Blt`, `DrawNative`, `GVUIPlayerController::CallBack2`,
  `CZnTouchKeypad::Draw`) cae en este caso — por eso el bug nunca se manifestó antes de la Fase 36, no
  porque no existiera.

**Fix real, en `loader/so_util.h`/`so_util.c`**: se agregaron 2 campos a `so_hook`
(`uint16_t align_nop_orig; int has_align_nop;`). `hook_thumb()` ahora guarda los 2 bytes reales ANTES de
escribir el NOP de alineación (y flushea esa escritura, mismo criterio de la Fase 37). `so_unhook()` ahora
restaura esos 2 bytes también cuando corresponde, además de los 8 bytes del trampolín. `hook_arm()` fija
`has_align_nop=0` explícitamente (ARM siempre cae en un límite de 4 bytes, este caso no le aplica).
Recompilado (`--cgsgraphics-probe` y el build normal), ambos limpios, sin warnings nuevos.

**Nota de proceso**: este fue el tercer intento de fix para lo que parecían crashes "del mismo lugar" —
los primeros dos (icache flush de la Fase 37, retorno de 64 bits de la Fase 38) eran bugs REALES y ya
confirmados por su propia lógica, pero ninguno de los dos era la causa de ESTE crash en particular — el
hecho de que el crash reprodujera *byte por byte* después de cada intento fue la señal de que había que
seguir buscando en vez de asumir que el fix anterior "no alcanzó". Desensamblar el código real (no solo
leer el pseudo-C de Ghidra) fue lo que finalmente lo destapó — el pseudo-C de `EndAlpha` no tenía forma de
mostrar este bug porque el bug no está en `EndAlpha` en absoluto, está en la infraestructura de hooking.

**Pendiente confirmar en consola** que el crash no se repite con este tercer fix.

## Fase 40: el fix de la Fase 39 era necesario pero NO suficiente — cuarto crash, causa raíz completa encontrada (el hook se "olvida" de la desalineación en cada reinstalación) (2026-07-18)

Cuarto crash reportado (`log_1784429583.txt` + `Zenonia4-psp2core-1784429607-...psp2dmp`), esta vez el
usuario confirmó que llegó bastante más lejos: se ve el primer `frame 1 alive` en el log (nunca antes
visto en los 3 crashes previos) y un `[Java] OnUIStatusChange: 1` (transición real hacia el menú) — o
sea, **el fix de la Fase 39 sí funcionó** para el crash original, pero el juego crasheó de nuevo más
adelante, al intentar entrar al menú.

**Triage**: `PC=0x0` (NULL esta vez, no `.got` como en la Fase 38/39), `LR=0x9809b3c7` — la MISMA dirección
de retorno (`CGsGraphics::EndAlpha+0x1f`) que en el crash anterior, y `R9=0x9809bf79` sigue en el vecindario
de `CGsGraphics::DrawFillRect` (offset +3 esta vez, no +1). Mismo tipo de bug (llamar a través de un
puntero corrompido) pero con un valor de basura DISTINTO (`0x0` en vez de una dirección de `.got`) —
consistente con "el mismo patrón de corrupción de stack, pero con contenido de stack diferente porque esta
vez se llegó más lejos en el juego antes de disparar el bug".

**La causa real (completa) del bug de alineación de la Fase 39**: el fix de esa fase agregó los campos
para guardar/restaurar los 2 bytes del NOP de alineación — pero **el patrón de reinstalación que usan
LOS 18 HOOKS DEL ARCHIVO** (el contador de la Fase 31/38, la guardia de `PutCompressImg`, los 13 de
`CGsGraphics::`, incluso el hook de `GVUIPlayerController::CallBack2`) reconstruye la dirección a
re-hookear con `hook.addr | (hook.thumb_addr ? 1 : 0)` — usando `hook.addr`, que **para un target
desalineado ya no es la dirección original** (es la dirección YA CORRIDA +2 bytes por el fix de la Fase
39). Consecuencia: en la primera instalación, `hook_thumb()` detecta correctamente la desalineación y
guarda/restaura bien los 2 bytes del NOP — pero en la SEGUNDA instalación (el `rehook` de cada ciclo
`unhook → llamar original → rehook`), la dirección que se le pasa a `hook_thumb()` ya está corrida +2, así
que la nueva verificación de alineación (`addr & 2`) da **falso** esta vez (+2 desplaza la paridad) —
`has_align_nop` se pone en `0` para ese hook desde la segunda instalación en adelante, y el rango de 2
bytes que contiene la primera instrucción real de la función (el prólogo `push {...}`) deja de
restaurarse. El resultado práctico: la función parece funcionar bien el PRIMER par de invocaciones (donde
el fix de la Fase 39 sí actuaba), pero a partir de la tercera invocación en adelante el prólogo real vuelve
a faltar en el camino de "llamar al original" — la corrupción de stack simplemente tardaba más en
manifestarse como crash esta vez (después de llegar al menú) en vez de en la primerísima llamada.

**Fix completo, en `loader/dynlib.c`**: los 18 sitios de reinstalación (`hook_addr(X.addr | (X.thumb_addr
? 1 : 0), ...)`) ahora usan `hook_addr(X.thumb_addr ? X.thumb_addr : X.addr, ...)` — es decir, reinstalar
siempre en la dirección ORIGINAL verdadera (`thumb_addr`, ya con el bit de Thumb puesto) para hooks Thumb,
o en `addr` sin cambios para hooks ARM (`thumb_addr == 0` en ese caso, ver `hook_arm()`). Esto hace que
`hook_thumb()` reevalúe la desalineación de forma CONSISTENTE en cada reinstalación (no solo la primera),
así que el fix de la Fase 39 (guardar/restaurar los 2 bytes del NOP) se aplica en todas las invocaciones,
no solo en la primera. Aplicado con una sustitución uniforme a los 18 sitios (contador de hot path,
guardia de `PutCompressImg`, hook de dpad, los 13 de `CGsGraphics::`) — ninguno de los hooks previos a la
Fase 36 tenía targets desalineados, así que este bug nunca les afectó en la práctica, pero el fix es
correcto y general para cualquier hook futuro que sí caiga en un target desalineado.

Recompilado (`--cgsgraphics-probe` y el build normal), ambos limpios, sin warnings nuevos.

**Pendiente confirmar en consola** que el crash no se repite con este cuarto fix — esta vez con más
confianza que las rondas anteriores: el problema no era "faltaba guardar unos bytes" (Fase 39, real pero
incompleto) sino "el mecanismo de reinstalación nunca le daba a esa lógica la oportunidad de actuar más de
una vez" — un bug de una clase completamente distinta (gestión de estado entre invocaciones, no
codificación de instrucciones), encontrado recién al notar que el crash llegó MÁS LEJOS que antes en vez
de repetirse idéntico — la señal de que el fix anterior sí tuvo efecto real, solo que incompleto.

**Bugs/limitaciones conocidas sin resolver, heredadas de fases anteriores**:
- `GetByteArrayElements(env, 0x0, 0x0)` aparece dos veces por sesión sin causar problema visible — nunca
  se identificó el call site exacto (ver Fase 7.2).
- `OnAsyncTimerSet(int,int)` vs `OnAsyncTimerSet(int,int,int)`: FalsoJNI no puede distinguir sobrecargas
  por firma, el handler asume 2 argumentos (ver Fase 4) — sin confirmar si el motor usa la variante de 3
  en algún punto.
- Android-UI overlay (`androidui.c`) para las pantallas de Zenonia 3 heredadas (logo/menú con arte
  propio) sigue sin adaptarse a los assets reales de Zenonia 4 — actualmente deshabilitado
  (`androidui_load()` no se llama, ver Fase 9) porque esas pantallas se dibujan nativamente en Zenonia 4
  vía `.pzx`. Pendiente confirmar si hace falta para las pantallas NUEVAS (Gamevil Live login, tienda
  IAP) que sí podrían necesitar overlay Android real.
- Wordwrap (`GFA_GetWordwrapPositionEx`) usa un piso de progreso mínimo en vez de la fórmula exacta (ver
  Fase 11) — el wrap visual de textos largos puede quedar imperfecto (una línea con algo más o menos
  texto del ideal) hasta confirmar la causa exacta de `maxWidth`/escala con números reales.
