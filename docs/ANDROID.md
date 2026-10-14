# Android: build, despliegue y depuración

## Requisitos

- Android SDK en `%LOCALAPPDATA%\Android\Sdk` con platform 35, build-tools 35+, NDK `28.2.13676358`, CMake `3.22.1`.
- JDK 21 de Android Studio: `C:\Program Files\Android\Android Studio\jbr` (el script lo usa si no hay `JAVA_HOME`).
- Un móvil Android 11 o posterior (arm64, Vulkan) con depuración inalámbrica emparejada o por USB (`adb devices`).
  Los de pruebas son un Galaxy Z Fold5 (Adreno 740) y un Xiaomi Mi 11X (Adreno 650).

> Hay dos `adb` en el PATH (SDK y scrcpy). Si tienen versiones distintas se matan el servidor ADB entre sí.
> Los scripts usan siempre `Sdk\platform-tools\adb.exe`.

## Proyecto Gradle

- `apps/android/` es el proyecto Gradle (AGP 8.13.2, Gradle 8.14.3). Ábrelo en Android Studio si quieres
  depurar con el IDE, pero el flujo normal es por script.
- El módulo `app` compila el **CMake raíz del repo** (`externalNativeBuild`); la librería nativa es
  `libvanilladroid.so`, cargada por GameActivity vía `android.app.lib_name`.
- Solo `arm64-v8a`. STL `c++_shared` (lo exige el prefab de GameActivity).

## Variantes

| Variante | Uso | Notas |
|---|---|---|
| `debug` | Depurar crashes con el IDE | `-O0`, **nunca** para medir rendimiento |
| `release` | Jugar y **medir** | `-O2`, firmada con la clave debug (uso personal), `profileable` para AGI/Perfetto/simpleperf |
| `profile` | Perfilar CPU con Tracy | `release` + cliente Tracy bajo demanda (puerto 8086). Coste casi nulo sin conexión, pero las cifras finales se toman en `release` |
| `dist` | **La que se publica** (fase 22, ADR-0005) | `release` endurecida (`VD_DIST`: sin ImGui, galería, interruptores `VD_*` ni extras de desarrollo en el intent), `applicationId` `com.cer0labs.vanilladroid` («VanillaDroid»; las demás son `dev.cer.vanilladroid`, «VanillaDroid Dev»: conviven en el móvil), firmada con la clave de release si están `VD_KEYSTORE`, `VD_KEYSTORE_PASSWORD`, `VD_KEY_ALIAS` y `VD_KEY_PASSWORD` (si no, con la de depuración). `-PvdVersion=X.Y.Z` fija la versión |

## Flujo diario

```powershell
.\scripts\android.ps1 -Variant release -Install -Run -Logcat   # compilar + instalar + lanzar + log
.\scripts\android.ps1 -Reverse                                  # túnel al vMaNGOS del PC
.\scripts\android.ps1 -Logcat                                   # solo log
```

## Actualizar el móvil a la última versión

En el móvil hay cuatro cosas, y cada una se actualiza por su lado:

| Qué | Dónde está en el móvil | Cuándo cambia | Cómo se actualiza |
|---|---|---|---|
| **APK** (código: `libvanilladroid.so` y `MainActivity`, y desde la fase 22 `assets/app.vdpk`: nuestros textos, arte y fuente) | la app `dev.cer.vanilladroid` (`app.vdpk` se copia al arrancar a su almacenamiento interno) | con cualquier cambio en `engine/`, `apps/android/`, `shaders/`, `locale/` o `art/` | compilar e instalar (paso 3); `android.ps1` hornea antes `app.vdpk` con el baker del PC |
| **Packs** (`*.vdpk`, los datos del juego horneados) | `/sdcard/Android/data/dev.cer.vanilladroid/files/packs/` | cuando cambian el baker, un formato o `locale/` (lo dice el commit: «models.vdpk v8», «solo cambia ui.vdpk») | hornear en el PC (paso 2) y subir (paso 4) |
| `client.cfg` (reino, cuenta, idioma) | `/sdcard/Android/data/dev.cer.vanilladroid/files/client.cfg` | cuando lo cambias tú | `adb push` (paso 5) |
| Ajustes (`settings.cfg`), `WTF/` (Config.wtf, macros, chat) y caché de pipelines | almacenamiento interno, `/data/user/0/dev.cer.vanilladroid/files/` | los escribe el juego | no se tocan: `adb install -r` los conserva |

> **Nunca desinstales la app para actualizarla.** Android borraría con ella los packs (4,4 GB que habría que volver a
> subir) y los ajustes. `adb install -r` reemplaza el código y conserva todo lo demás.

### Chuleta: lo normal son dos órdenes

```powershell
# Desde la carpeta del repositorio.
# Solo si hay más de un dispositivo conectado (ver el paso 1): el serial del móvil, copiado tal cual de "adb devices".
$env:ANDROID_SERIAL = '<serial>'
.\scripts\android.ps1 -Variant release -Install   # compila (solo lo que cambió) e instala el APK
.\scripts\push-data.ps1                           # sube solo los packs que cambiaron (compara el MD5)
```

Desde la fase 22 hay otra forma de subir los packs, la que usará VanillaDroid Setup:
`.\build\windows\tools\setup\RelWithDebInfo\vdsetup.exe --app-id dev.cer.vanilladroid --packs packs adb push`
(compara el SHA-256 con `sha256sum` en el móvil y copia a `.part` antes de renombrar). Ninguna de las dos sube
`app.vdpk`: va dentro del APK.

Medido con el Fold5 por Wi-Fi: con el APK ya compilado, instalar tarda unos 25 s (unos 50 s si hay que recompilar
algún fichero de C++). Subir 88 packs (1,05 GB) tardó 2,2 min, a unos 18 MB/s. El juego entero son 96 packs y 4,4 GB: unos 5 min desde cero.

### Paso a paso, con lo que hacen los scripts por debajo

Los pasos usan estas variables de PowerShell:

```powershell
# Desde la carpeta del repositorio.
$adb   = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"    # siempre este adb, nunca el de scrcpy
$baker = ".\build\windows\tools\baker\RelWithDebInfo\vd_baker.exe"
$dir   = "/sdcard/Android/data/dev.cer.vanilladroid/files"
```

**1. Conectar el móvil por ADB inalámbrico**

1. El móvil y el PC tienen que estar en la misma Wi-Fi. En el móvil: Ajustes → Opciones de desarrollador →
   **Depuración inalámbrica**, activada.
2. Solo la primera vez, o si se borró la vinculación: en esa pantalla, «Vincular dispositivo con código de vinculación».
   Después, en el PC: `& $adb pair <IP>:<puerto de ese diálogo>`, y escribes el código de 6 cifras.
3. Normalmente el PC se conecta solo (por mDNS: de ahí seriales como `adb-<número de serie>-<id>._adb-tls-connect._tcp`).
   Si no aparece: `& $adb connect <IP>:<puerto>`. Usa la IP y el puerto que salen arriba en «Depuración
   inalámbrica», no los de vincular; el puerto cambia cada vez que se activa la opción.
4. Comprueba con `& $adb devices -l`: el móvil (el Fold5 es `model:SM_F946B`) tiene que decir `device` (no
   `unauthorized` ni `offline`). Por cable USB su serial es el número de serie, y la subida de packs va más rápida.
5. Si hay más de un dispositivo conectado (por ejemplo el Mi 11X, `model:M2012K11AG`), elige el destino antes de
   seguir: `$env:ANDROID_SERIAL = '<serial>'`. adb lo respeta, y por tanto también todos los scripts. A veces el
   Fold5 sale repetido con ` (2)` en el nombre; vale cualquiera de los que digan `device`, copiado tal cual,
   con espacios y paréntesis.

**2. Re-hornear los packs (solo si cambiaron los datos)**

El APK y los packs son independientes: un cambio en el motor no obliga a re-hornear nada.

| Si el cambio toca… | Haz esto |
|---|---|
| solo `engine/`, `apps/` o `shaders/` | nada en este paso: basta con el APK (paso 3) |
| `locale/*/*.lang` (textos propios) | `& $baker bake --out packs --only ui` (los textos van dentro de `ui.vdpk`) |
| el cliente de otro idioma (fase 21) | `& $baker bake --out packs --only locale` (`db.esES.vdpk`, `textures.esES.vdpk`, `sound.esES.vdpk`) y `--only ui`; en el móvil, `locale = esES` en `client.cfg` |
| `tools/baker/` o un formato de `engine/src/pack/` | el `--only` de ese pack (lo nombra el commit) |
| no lo tienes claro | `& $baker bake --out packs` (todo: las texturas salen de la caché `packs/.cache` y el resto se rehace en varios minutos) |

Si cambió el código del baker, compílalo antes: `cmake --build --preset windows-release --target vd_baker` (en un PC
nuevo, primero `cmake --preset windows`). Si al móvil le falta un pack o le llega uno anticuado, el log lo avisa con
la orden que lo arregla, por ejemplo `UI pack ...: bake it with: vd_baker bake --out packs --only ui`.

**3. Compilar e instalar el APK**

`.\scripts\android.ps1 -Variant release -Install` hace esto:

```powershell
$env:JAVA_HOME = 'C:\Program Files\Android\Android Studio\jbr'        # si no estaba definido
Push-Location apps\android; .\gradlew.bat assembleRelease; Pop-Location   # C++ (CMake del repo, arm64) + APK
& $adb install -r apps\android\app\build\outputs\apk\release\app-release.apk   # -r: reemplaza y conserva los datos
```

- Gradle solo recompila lo que cambió (el C++ va por ninja, de forma incremental). El APK pesa unos 13 MB.
- `release` sirve para jugar y para medir; `profile` es para Tracy; `debug` no sirve nunca para medir (tabla de
  variantes, arriba).
- Para ver qué versión hay instalada: `& $adb shell dumpsys package dev.cer.vanilladroid | Select-String lastUpdateTime`.

**4. Subir los packs**

`.\scripts\push-data.ps1` hace esto con cada `packs\*.vdpk` (aquí, con `ui.vdpk` como ejemplo):

```powershell
& $adb shell mkdir -p $dir/packs
(Get-FileHash -Algorithm MD5 packs\ui.vdpk).Hash.ToLower()   # el MD5 en el PC...
& $adb shell md5sum $dir/packs/ui.vdpk                       # ...y en el móvil: si coinciden, se salta
& $adb push packs\ui.vdpk $dir/packs/ui.vdpk                 # si no, se copia (y después se vuelve a comparar el MD5)
```

- Solo copia los que cambiaron. Comparar el tamaño no basta: el ASTC da el mismo tamaño con otro contenido. Con
  `-Force` lo copia todo sin comparar.
- Para subir un pack suelto a mano: `& $adb push packs\models.vdpk $dir/packs/models.vdpk`.
- Cierra antes la app (`& $adb shell am force-stop dev.cer.vanilladroid`): tiene los packs mapeados en memoria.
- No borra del móvil los packs que ya no estén en el PC. Para verlos: `& $adb shell ls -l $dir/packs`. Para ver el
  espacio libre: `& $adb shell df -h /sdcard`.

**5. El servidor, y `client.cfg` (opcional)**

El servidor se elige en la pantalla de acceso con el botón **Server** (el realmlist: `host` o `host:puerto`; se guarda
como `SET realmList` en `WTF/Config.wtf`); sin servidor, «Login» abre ese diálogo. El orden: lo que se pasa al lanzar
(desarrollo: `-Realmd`), lo que escribió el jugador (el diálogo, `Config.wtf` o un `realmlist.wtf` del 1.12 junto a
la carpeta `WTF`, que gana a `Config.wtf`), `realm.address` de `client.cfg` y, si no hay nada, vacío.

`client.cfg` sirve para dejar fijos la dirección, la cuenta y el idioma en un móvil de pruebas. La plantilla es
[config/client.example.cfg](../config/client.example.cfg); la contraseña no se guarda nunca:

```
realm.address = 192.168.1.20:3724
account = admin
```

Para cambiarla, la escribes en `config\client.cfg` (ignorado por git), la subes con
`& $adb push config\client.cfg $dir/client.cfg` y la lees con `& $adb shell cat $dir/client.cfg`. Por cable, con
`.\scripts\android.ps1 -Reverse`, la dirección es `127.0.0.1:3724`.

**6. Abrir y comprobar**

- Abre la app desde el icono de VanillaDroid, o con `.\scripts\android.ps1 -Run` (que hace `am force-stop` y luego
  `& $adb shell am start -n dev.cer.vanilladroid/.MainActivity`). El móvil tiene que estar **desbloqueado**: con la
  pantalla bloqueada la app arranca pero no dibuja.
- Mira el log con `.\scripts\android.ps1 -Logcat`, que hace
  `& $adb logcat -v time -s VanillaDroid:V GameActivity:V AndroidRuntime:E DEBUG:V libc:E`. Al arrancar lista los
  packs (`World packs: 43 maps`, `UI: ui.vdpk`...), después dice `Terrain map Azeroth ready in ...` y, cada 5 s,
  `60.0 fps avg | ...`. Un `WARN` con «not baked» o «Cannot open» significa que ese pack falta o está anticuado.
- Tiene que verse la pantalla de acceso del 1.12, con el Portal Oscuro y su música.
- Para entrar en el servidor: arranca tu servidor (por ejemplo vMaNGOS en el PC) y escribe la contraseña en la
  pantalla, o lanza
  `.\scripts\android.ps1 -Run -Account <cuenta> -Password ... -Character <personaje> -Realmd 192.168.1.20:3724`.

### Problemas frecuentes

| Síntoma | Causa y solución |
|---|---|
| `adb devices` vacío, o el móvil `offline` | La depuración inalámbrica se apagó (pasa al cambiar de red o al reiniciar) o cambió el puerto. Actívala otra vez y haz `adb connect` con el puerto nuevo. Si sigue igual: `& $adb kill-server` y vuelve a probar. |
| `unauthorized` | Acepta en el móvil el aviso de la huella RSA del PC. |
| `more than one device/emulator` | Define `$env:ANDROID_SERIAL` (paso 1). |
| El servidor ADB se reinicia solo | Hay otro `adb` de otra versión (el de scrcpy) en el PATH: usa siempre `$adb`. |
| `INSTALL_FAILED_UPDATE_INCOMPATIBLE` | El APK instalado se firmó con otra clave debug (se compiló en otro PC). Desinstalar borra los packs: o copias antes `%USERPROFILE%\.android\debug.keystore` del PC original, o asumes que hay que subirlos de nuevo. |
| `INSTALL_FAILED_USER_RESTRICTED` (Xiaomi) | Activa «Instalar vía USB» en las opciones de desarrollador y confirma en el móvil. |
| `No se pudo crear .../packs` | Abre la app una vez: así Android crea su carpeta. |
| `MD5 distinto tras copiar` | Se cortó la copia por Wi-Fi: vuelve a lanzar `push-data.ps1`. |
| No encuentra el reino; el log dice `realmd: Cannot connect to ...` | La dirección del servidor está mal: tócala en la pantalla de acceso y corrígela (o `realm.address` en `client.cfg`, paso 5). Con `.\scripts\android.ps1 -Reverse` (por cable) la dirección es `127.0.0.1:3724`; por Wi-Fi, la IP del PC en la red local. |
| La app se queda en negro o se cierra | Mira `.\scripts\android.ps1 -Logcat` y busca `ERROR` o `FATAL`. Lo habitual es que falte un pack. |

## Ciclo de vida que el motor debe respetar

| Evento GameActivity | Acción del motor |
|---|---|
| `APP_CMD_INIT_WINDOW` | Crear `VkSurfaceKHR` + swapchain (el dispositivo Vulkan se crea una sola vez) |
| `APP_CMD_TERM_WINDOW` | Destruir swapchain + surface; dejar de dibujar (el loop se bloquea) |
| `APP_CMD_WINDOW_RESIZED` / `CONFIG_CHANGED` | Recrear swapchain con el nuevo tamaño (plegar/desplegar, rotar) |
| `onTrimMemory(nivel)` (`MainActivity` → `nativeOnTrimMemory`, y luego `APP_CMD_LOW_MEMORY`) | 5–15: el terreno deja de crecer y se vacía la caché de modelos de efecto; 40+: además se sueltan las páginas de los packs mapeados (ver abajo) |
| `APP_CMD_PAUSE` / `STOP` | Pausar audio y guardar los ajustes si cambiaron; la red sigue viva un tiempo razonable |

## Prueba manual de la Fase 0 (móvil desbloqueado)

Lanzar con `.\scripts\android.ps1 -Variant release -Install -Run -Logcat` y comprobar:

1. Triángulo que **oscila con la punta hacia arriba** y HUD arriba a la izquierda (fps, CPU, GPU, gráfica).
2. HUD estable en **60,0 fps**, gráfica plana (sin picos) y `worst` < 17 ms tras unos segundos.
3. **Girar 180°** (horizontal ↔ horizontal invertido): la punta sigue hacia arriba y el HUD arriba a la izquierda.
   El log debe mostrar `Swapchain: ... rotate-90`/`rotate-270` al cambiar.
4. **Desplegar y plegar** 20 veces con la app abierta (pantalla exterior ↔ interior): sin cierres, el HUD
   se adapta al nuevo tamaño y el log muestra la swapchain recreada con las dimensiones de cada pantalla.
5. Botón de inicio y volver: la app se reanuda sin cierre ni pantalla negra.

Si la punta del triángulo o el HUD aparecen girados, la pre-rotación está al revés: corregir el signo en
`Swapchain::preRotation()`.

## Notas y trampas conocidas

- **Git Bash convierte rutas**: `adb shell ls /sdcard/...` se convierte en `C:/Program Files/Git/sdcard/...`.
  Usar `MSYS_NO_PATHCONV=1` solo en comandos `adb` (no exportarlo para Gradle) o los scripts de PowerShell.
- **MIUI (Xiaomi)**: `INSTALL_FAILED_USER_RESTRICTED` = falta "Instalar vía USB" en opciones de desarrollador
  o confirmar el aviso en el móvil.
- **Swappy** registra `SwappyDisplayManager ... couldn't find "libvanilladroid.so"` al arrancar: su clase Java va
  embebida y se carga desde memoria; los métodos nativos se registran igualmente. Inofensivo.
- **Constantes de especialización en el Adreno 650** (Mi 11X, Vulkan 1.1.128, controlador `0x801f6000`): con la
  constante booleana `kAlphaToCoverage` sin especializar (falsa), el `discard` del recorte alfa desaparecía y el follaje
  salía en cuadrados opacos (el Fold5 iba bien). Desde entonces el recorte con *alpha to coverage* es otra variante
  compilada (`shaders/model_alphakey_a2c.frag`), no una constante de especialización: evitarlas en los shaders.
- **Layouts de pipeline mezclados en el Adreno 650** (el mismo controlador): los doodads animados (F14) enlazaban la
  paleta de huesos (set 2) con el layout de los actores y después el set 1 (textura) con el de dos sets de los modelos.
  Es Vulkan válido (layouts compatibles), y en la GTX 1080 del escritorio iba bien, pero en el Xiaomi el set 2 se perdía:
  **todos** los doodads animados eran invisibles y solo quedaban sus partículas (en Cañada Umbría, la luz de las
  farolas sin farola). Por eso cada draw enlaza todos sus sets con el layout de su propio pipeline
  (`ModelRenderer::draw`). Para aislar algo así en el móvil: `VD_DOODAD_ANIM=0`, `VD_FX_OFF=1` por el extra `env`.
- **Memoria *lazily allocated***: el Fold5 (Adreno 740) la expone para el depth; el Adreno 650 del Xiaomi no, y
  el renderer cae a memoria normal automáticamente.
- **ADPF en el Xiaomi Mi 11X**: Android 13 expone la API, pero `APerformanceHint_createSession` devuelve null (su
  HAL de energía no lo soporta). Sin sesión, el planificador dejaba el hilo de juego en los A55 (cpu0–3) con los
  grandes a ~800 MHz: CPU del frame 4–5,7 ms en las ciudades. El motor fija entonces el hilo a los núcleos que no
  son del clúster lento (log `ADPF performance hint unavailable: thread N pinned...`): 1,3–1,5 ms. Para ver en qué
  núcleo corre cada hilo: `adb shell ps -T -p <pid> -o TID,PSR,PCPU,NAME`.
- `adb shell input tap X Y` sirve para pulsar botones del HUD de depuración de forma remota (coordenadas de
  pantalla en la orientación actual).

## Conectar con el servidor (fase 4)

- `.\scripts\android.ps1 -Run -Account <cuenta> -Password … -Character <personaje> -Realmd 192.168.1.20:3724`: por
  Wi-Fi a la IP del PC en la red local. Por cable: `-Reverse` y `-Realmd 127.0.0.1:3724`.
- La contraseña solo viaja en el intent y el motor la convierte al momento en su hash SRP (en memoria); no se
  guarda en ningún fichero. La cuenta, la dirección y el personaje pueden ir en `files/client.cfg` del móvil
  (plantilla en `config/client.example.cfg`).
- El panel "Sesión" muestra estado, latencia, entidades y reconexiones; la línea de log de cada 5 s termina con
  `net: …` (estado, latencia, paquetes, errores de parseo, reconexiones).
- Pruebas de movimiento en línea (fase 6, cuenta GM): `-Commands ".tele goldshire;.modify speed 2"` dice esas
  líneas en el mundo, una cada 8 s, y `-WalkRoute walk_goldshire` recorre después esa ruta a pie desde su punto más
  cercano (extras `commands` y `walkroute`). El script arma la línea de `am start` entera para el shell del móvil,
  con cada valor entre comillas simples: PowerShell 5.1 estropea las comillas de los argumentos que reenvía.
- Controles del personaje (fase 7, HUD táctil): dedo en la zona inferior izquierda = stick (corre hacia donde
  apunta respecto a la cámara; poco recorrido = andar), cualquier otro dedo sobre el mundo = girar la cámara, dos
  dedos = zoom, toque corto sobre una unidad = seleccionarla. Saltar, atacar/hablar/saquear y las acciones son
  botones. Tres dedos a la vez muestran u ocultan los paneles de desarrollo (el panel "Personaje" da modo, flags,
  paquetes enviados y correcciones del servidor).

## Entrada de la interfaz táctil (fase 7)

- **Teclado del sistema (GameTextInput)**: se muestra u oculta tras cada tick según `Game::wantsKeyboard()` (un
  `ui::TextInput` activo). Cuando el motor empieza a editar un campo (`version()` sube), el IME se re-siembra con
  `GameActivity_setTextInputState` (cursor al final, contado en unidades UTF-16); lo que escribe el jugador llega
  entero en `app->textInputState` y el motor lo limpia (UTF-8 válido, sin controles, límite de bytes; acepta el
  UTF-8 modificado de Java). La tecla de acción del IME (`APP_CMD_EDITOR_ACTION`) es Enter; Atrás/Escape, u ocultar
  el teclado con un campo aún en edición, es Escape (cancela el campo). La app dibuja de borde a borde con
  `adjustResize`: el teclado llega como un inset, no como un cambio de tamaño de la superficie.
- **Zona segura**: máximo de barras del sistema, recorte de cámara y bordes curvos, en la orientación del jugador
  (el mismo espacio que los toques); se actualiza al crear la ventana y en `APP_CMD_WINDOW_INSETS_CHANGED`. La
  altura del teclado va aparte (`Game::keyboardInset()`). Escritorio: `--safe-area l,t,r,b` para probarlo.
- **Densidad**: `GameActivity_getDensityDpi / 160` al arrancar y en cada `APP_CMD_CONFIG_CHANGED` (plegar y
  desplegar): el HUD se recoloca solo (se construye cada frame con el tamaño y la zona segura del momento).
- **Mando (Paddleboat, games-controller 2.0.2)**: se inicia una vez antes del bucle, con ganchos de
  start/stop/shutdown y actualización por frame. Los eventos de teclas y ejes de mandos pasan antes por Paddleboat;
  el filtro de movimiento deja pasar pantalla táctil (incluido S Pen) y joystick/mando. El primer mando activo llega
  a `Game::onGamepad` (eje Y hacia arriba). Stick izquierdo = mover (zona muerta 0,24), derecho = cámara (3 rad/s de
  giro, 1,8 de inclinación), A = saltar. El enlazador fuerza (`-u`) los 7 callbacks JNI de Paddleboat. En
  escritorio, XInput lee el primer mando mientras la ventana tiene el foco.
- **Compilar desde otro checkout sin descargar dependencias**: `.\gradlew.bat assembleRelease
  -PvdDepsDir="<otro checkout>/build/windows/_deps"` reutiliza las fuentes ya bajadas por el CMake de escritorio
  (`FETCHCONTENT_FULLY_DISCONNECTED`); la variante `profile` necesita además `tracy-src` en esa carpeta.

## Benchmarks y pantalla

- `.\scripts\bench.ps1 -Route <ruta>` (F2: `elwynn`, `durotar`, `barrens`, `goldshire`; F3: `stormwind`,
  `orgrimmar`, `ironforge`, `stranglethorn`; F5: `crowd`, `crowd0`; F6, a pie: `walk_goldshire`, `walk_stormwind`,
  `walk_orgrimmar`, `walk_gorlach`) lanza la app con el extra de intent `bench` (`am start ... --es bench <ruta>`),
  espera y muestra la línea `BENCH` (o `Walk ...`) del log. Las rutas a pie necesitan `collision_*.vdpk`.
- Cada 5 s el log da fps, draws/triángulos (separando WMO y M2) y el tiempo de CPU del hilo principal por
  secciones (terreno, objetos, culling, grabación); cada frame de más de 25 ms deja una línea `Hitch` con su
  desglose, y cada bloque de memoria de GPU nuevo, lo que tardó.
- El motor activa `GAMEACTIVITY_FLAG_KEEP_SCREEN_ON`: sin él Android apagaba la pantalla al minuto sin toques y
  destruía la superficie a mitad de recorrido.

## Térmica, batería, memoria y calidad (fase 10)

- **Hilo `vd-monitor`** (`platform/DeviceMonitor`): una vez por segundo pregunta al sistema y el hilo de juego solo
  copia la última lectura. Térmica por la API de ADPF (`AThermal_getThermalHeadroom` con previsión de 10 s y
  `AThermal_getCurrentThermalStatus`, buscadas con `dlsym`: minSdk 30); batería cada 5 s por JNI
  (`BatteryManager.getIntProperty`: carga en µAh, corriente y nivel, y el intent fijo `ACTION_BATTERY_CHANGED`:
  voltaje, temperatura, enchufado); memoria cada 2 s de `/proc/self/statm` y `/proc/meminfo`. El log de cada 5 s
  añade una línea `quality: ...` con todo ello. El Fold5 informa la corriente en mA (la documentación dice µA): por
  debajo de 20.000 se toma como mA.
- **ADPF en el Fold5** (Android 16): sesión con 7 hilos (juego + workers) que recibe el tiempo de CPU y de GPU de
  cada frame (`APerformanceHint_reportActualWorkDuration2`, Android 15+). Umbrales térmicos del dispositivo:
  ligero 0,83, moderado 0,90, severo 1,00, crítico 1,07. En las rutas de benchmark arranca en ~0,6.
- **Ajustes guardados** en `files/settings.cfg` del almacenamiento interno (`app/SettingsFile`, `clave = valor`), los
  escribe `vd-monitor` 1,5 s después del último cambio y al pausar. Los benchmarks no los leen (resultados
  comparables).
- **Memoria en segundo plano**: `am send-trim-memory dev.cer.vanilladroid BACKGROUND` lo simula. En el Fold5, tras
  la multitud de Orgrimmar: residente de 648 a 358 MB, PSS de 1,23 GB a 841 MB; al volver, un frame de 35 ms (la
  swapchain nueva). Ver el desglose con `adb shell dumpsys meminfo dev.cer.vanilladroid`.
- **Frecuencia de la GPU**: el shell puede leer `/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq` y `gpu_busy_percentage`
  (la app no). El Adreno 740 va de 124,8 a 719 MHz; en nuestras rutas se queda en 220–270 MHz.
- **Sesión larga**: `.\scripts\soak.ps1 [-Minutes 30]` lanza la ruta `soak` (Orgrimmar y el combate de 40 en
  bucle), no toca el móvil mientras dura (el juego escribe cada minuto sus líneas `SOAK` en
  `files/packs/bench_soak.txt`) y al acabar las recoge con `dumpsys battery` antes y después, y deja un CSV en
  `build\soak\`. Para la batería, desenchufado (ADB inalámbrico); enchufado se niega salvo con `-AllowCharging`.
- Extras de intent de los benchmarks: `quality` (low / medium / high / ultra), `renderscale` (0,5–1, fija y sin
  calidad dinámica), `dynamicquality` (on / off), `benchminutes` (duración de una ruta en bucle); `bench.ps1`
  los acepta como `-Quality`, `-RenderScale`, `-DynamicQuality` y `-Minutes`.

## Perfilado

- **AGI** (Android GPU Inspector): abrir la app `release` (es `profileable`), capturar frame o sistema.
- **Perfetto**: `ui.perfetto.dev` → grabar traza con CPU, frecuencia, térmica, `gfx`.
- **Tracy**: instalar la variante `profile` y ejecutar `.\scripts\tracy.ps1` (hace `adb forward tcp:8086` y abre
  la GUI v0.14.1 de `.tools\`). Cliente y GUI deben tener la misma versión.
- Temperatura rápida: `adb shell dumpsys thermalservice`.

## Primera vez sin datos y vinculación con el PC (fase 22)

Sin los packs del juego (falta db, textures, ui, models, actors o algún `world_*.vdpk`, o quedó una descarga a medias)
la app arranca en su pantalla de primera vez, dibujada solo con `app.vdpk` y en el idioma del sistema: vincular con
VanillaDroid Setup (`vdsetup serve` en el PC; el QR abre en el móvil la página del PC y su botón «Vincular» abre la app
con `vanilladroid://pair?h=...&k=...`), escribir la dirección y el código de 6 cifras, o la guía de ADB. La descarga va
a `files/packs` con reanudación y SHA-256 (`pairing.json` y `downloads.json` en el almacenamiento interno) y, si se
elige, el WoW a `files/wow/Data` (el mismo sitio que `vdsetup adb send-wow`). Al acabar, «Jugar» reinicia el juego en el
sitio.

- Probar sin Wi-Fi: `adb reverse tcp:47800 tcp:47800` y la dirección `127.0.0.1:47800`.
- En el escritorio: `--packs <carpeta con solo app.vdpk>` y `--link "vanilladroid://pair?h=127.0.0.1:47800&k=..."`
  contra `vdsetup serve --loopback`; `VD_PAIRING_DEMO=<página>` muestra cada estado con datos de ejemplo.
- Con las dos apps instaladas (`dev.cer.vanilladroid` y `com.cer0labs.vanilladroid`), Android pregunta cuál abre el
  enlace.

## Actualizaciones (fase 22)

`dist` lee al arrancar el manifiesto de la última release de `cer0Labs/VanillaDroid` (hilo `vd-updates`); si hay una
versión mayor lo avisa en las pantallas de acceso y la instala con `PackageInstaller` (Android pide confirmarlo; la
primera vez, el permiso de instalar apps desconocidas: `REQUEST_INSTALL_PACKAGES`). Las variantes de desarrollo solo
miran si se les da `VD_UPDATE_URL`:

- Una release de prueba en la red local: `scripts\test-release.ps1` la monta (el APK de una versión mayor,
  `-PvdVersion=0.2.0`, el zip del Setup, `manifest.json` y su `.sig` firmados con la clave de prueba, las URL hacia la
  red local con `release-manifest.ps1 -BaseUrl`; `-ChangePack` simula una release que cambia packs) y dice las órdenes;
  a mano: una carpeta con `manifest.json`, su `.sig` y el APK, servida con
  `python -m http.server 8000 --bind 192.168.1.20`; lanzar con
  `adb shell am start -n dev.cer.vanilladroid/dev.cer.vanilladroid.MainActivity --es env VD_UPDATE_URL=http://192.168.1.20:8000/manifest.json`
  (las de desarrollo permiten HTTP sin cifrar; `dist` no).
- `VD_UPDATE_DEMO=available|permission|downloading|confirming|failed|data|dataphone|...` muestra cada estado sin servidor.

## Preparar los datos en el móvil (fase 22)

Con los 12 MPQ del juego en `/sdcard/Android/data/<id>/files/wow/Data` (`vdsetup adb send-wow`, o la casilla «copiar
también el WoW» al vincular), la pantalla de primera vez ofrece «Preparar los datos en el móvil». El servicio
`BakeService` (proceso `:baker`) ejecuta `libvdbaker.so` paso a paso; todo lo suyo está en `files/bake/`:
`plan.json` (lo escribe el juego), `status.json` (el servicio, cada 2 s) y `bake.log` (la salida del baker:
`adb pull /sdcard/Android/data/<id>/files/bake/`); en logcat, `Phone bake`. `VD_PHONE_BAKE_STEPS=db,cinematics,bodyfx`
(desarrollo) hornea solo esos pasos; `VD_PAIRING_DEMO=phone|baking|cooling|stopped|phonedone|...` muestra los estados.
Para medir el baker sin la app: `scripts/android-baker.ps1` (en `/data/local/tmp`).
