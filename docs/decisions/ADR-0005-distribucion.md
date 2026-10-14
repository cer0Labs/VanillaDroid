# ADR-0005: Distribución: APK sin datos, VanillaDroid Setup y releases públicas

Estado: aceptada; revisada al publicar el código con la licencia GPL-3.0 (§6 y §9)

## Contexto

Queremos que otras personas instalen VanillaDroid sin repartir nada de Blizzard. Cada
jugador pone su propio cliente 1.12.1 (5875) y de él salen los packs. Hasta ahora todo era manual y solo para
desarrollo: el APK `release` se firma con la clave de depuración, los packs se hornean con `vd_baker` en este PC y se
suben con `scripts/push-data.ps1` por ADB.

Requisitos:

1. Un programa para PC y Mac que pida la carpeta del WoW 1.12.1, la valide y prepare los datos.
2. Un APK básico que, sin datos, se **vincule con el PC** para descargarlos; y que el programa del PC explique también
   cómo hacerlo por ADB. Todo, también por consola.
3. **Releases** marcadas con una etiqueta en este repositorio (`cer0Labs/VanillaDroid`), y **avisos de
   actualización** en el programa del PC (que se actualiza a sí mismo) y en la app.
4. Poder **copiar el WoW al móvil** para que la app se actualice sin el PC: si la release no toca nada derivado del
   WoW, basta el APK nuevo; si lo toca, el móvil vuelve a hornear lo que cambió.

Datos de partida: los packs ocupan 4,5 GB (texturas 1,26 GB, Kalimdor 0,82 GB, Azeroth 0,54 GB, sonido
0,49 GB) y la caché del baker otros ~3,2 GB; la carpeta `Data` del cliente, 5,05 GB. Hornear todo cuesta ~12 min en
el PC. El baker solo lee los `.MPQ` de `Data` (nunca `WoW.exe`), así que la carpeta del cliente de Windows sirve en
cualquier sistema. Todas las dependencias tienen licencias permisivas (MIT, BSD, zlib, Apache 2.0), compatibles con la GPL-3.0 del
código: los binarios llevan sus avisos.

## Decisión

### 1. Un solo APK firmado, sin datos; los datos llegan aparte

- **No se monta un APK por jugador.** Firmarlo en su PC obligaría a repartir nuestra clave o a que cada actualización
  choque con la anterior (y desinstalar borra los datos); 4,5 GB dentro del APK harían que cada cambio de código
  reinstale 4,5 GB y piden el doble de espacio al instalar.
- El APK de distribución es la variante **`dist`**: `applicationId` **`com.cer0labs.vanilladroid`**, nombre
  «VanillaDroid», firmado con la **clave de release** (fuera del repositorio; en la CI como secreto). Las variantes de
  desarrollo (`release`, `profile`) siguen como `dev.cer.vanilladroid` con la clave de depuración: las dos conviven en
  los móviles de pruebas sin desinstalar nada ni tocar los flujos de ahora.
- Versión **`vX.Y.Z`** (la etiqueta de git) y `versionCode = X·1.000.000 + Y·1.000 + Z`, pasadas a Gradle y al código
  nativo (`VD_VERSION`).

### 2. Lo nuestro va en el APK: `app.vdpk`

Los textos propios (`VD_*` de `locale/enUS` y las traducciones de `locale/<idioma>`), el arte propio (`art/`) y una
**fuente libre** (DroidSans, Apache 2.0, en `art/fonts/`) para las pantallas que se ven antes de tener datos se hornean en `app.vdpk`
(`vd_baker bake --only app`, sin el WoW: no abre ningún MPQ) y viajan **dentro del APK** (`assets/app.vdpk`, que
`scripts/android.ps1` hornea antes de Gradle; el `dist` no compila sin él). La app lo copia a su almacenamiento interno
cuando falta o cambia la instalación (`versionCode` y la hora de instalación del APK: en desarrollo el `versionCode`
no cambia) y lo abre como los demás packs (`pack/AppPack`, `AppPackInstall`). `ui.vdpk` deja de llevar lo nuestro:
solo lo que sale del cliente (sus texturas, fuentes, minimapas, `GlobalStrings.lua` / `GlueStrings.lua` y las reglas
derivadas de ADR-0004). `core/Text` funde al cargar las tablas de los dos packs en una sola (una búsqueda binaria por
texto, como antes), en este orden: para un idioma que no es el inglés, **el cliente de ese idioma, nuestras
traducciones, nuestro inglés y el cliente base**; para el inglés, nuestro inglés y el cliente base. Nuestro inglés va
antes que el cliente base (un test comprueba la tabla fundida con los clientes enUS y esES reales); las comillas que
el propio cliente perdió en algunos textos (U+FFFD) las restaura el baker. `ui::Assets` pone las
páginas del atlas y las capas de fuente de `app.vdpk` detrás de las de `ui.vdpk`, y funciona con `app.vdpk` solo
(`VD_UI_GALLERY=apponly`); un icono nuevo va en `kOwnArt` de `tools/formats/src/bake/AppPackBaker.cpp`.

Así **un cambio de textos o de arte propio llega con el APK, sin re-hornear**. La regla 1 del proyecto sigue: en el
APK nunca hay nada de Blizzard.

### 3. Recetas: saber qué packs hay que re-hornear

- `vd_baker digest --packs packs` calcula para cada pack un **resumen de su contenido** (rutas, tipos y bytes de cada
  recurso, sin depender del orden en el fichero) y lo escribe en **`release/packs.json`**, que se commitea con cada
  release. Ese resumen, horneado con los clientes de referencia (enUS y esES), es la **receta** del pack: cambia solo
  si el baker produce otra cosa.
- El baker y el Setup llevan dentro el `packs.json` de su versión. Al hornear escriben **`packs/manifest.json`** con la
  receta de cada pack, el build y el idioma de los clientes y la versión del Setup.
- Comparando recetas se sabe **qué pasos del baker repetir** (`world_*` → `world`, `collision_*` → `collision`,
  `*.esES` → `locale`...). Si ninguna cambia, la release no toca los datos.
- **El mismo cliente da las mismas recetas en cualquier plataforma** (PC, Mac, el móvil): si no, una actualización
  re-hornearía packs que no cambiaron. Reglas del baker (`tools/formats/src/bake/Portable.h`): se compila con
  `-ffp-contract=off` (sin FMA); seno, coseno, tangente y `atan2` se calculan en `double` y se redondean una vez
  (`portableRotate` en lugar de `glm::rotate`); nada de `std::partition` (`portablePartition` copia el algoritmo de
  MSVC), `nth_element` ni ordenaciones con empates (`std::stable_sort`). astc-encoder da los mismos bloques en todos
  sus ISA (comprobado: AVX2, SSE4.1, SSE2 y sin SIMD en el PC; NEON en el móvil), pero solo si el `sinf`, `cosf` y
  `logf` de la biblioteca de C coinciden: el de Android (bionic) se apartaba en 26 y 22 entradas de sus tablas y 19 de
  las 58.502 texturas salían con un bloque distinto. `cmake/astcenc/` compila esos dos ficheros de astc-encoder con las
  funciones calculadas en `double` y redondeadas una vez (el PC no cambia). Al adoptar estas reglas cambiaron una vez
  las recetas de `world_*`, `collision_*`, `worldsound` y `ui` (el `cosf` de MSVC difería en 1 ulp en 87 de 35.037
  giros). `vd_baker digest-assets --pack <fichero>` da la huella de cada recurso para encontrar diferencias.
- La app compara al arrancar su `packs.json` con `packs/manifest.json`: con un formato que ya no lee (`k*FormatVersion`)
  explica qué hay que actualizar en vez de fallar; con una receta distinta pero el mismo formato, avisa y deja jugar.

### 4. VanillaDroid Setup (Windows y macOS), con ventana y por consola

Programa de escritorio en `tools/setup/`: una biblioteca común (`vd_setup`) con el baker dentro y dos ejecutables, la
ventana **«VanillaDroid Setup»** (Dear ImGui sobre SDL3; textos por claves, ADR-0003, `locale/<idioma>/setup.lang`) y
la consola **`vdsetup`**:

```
vdsetup status                         versiones, datos, móvil vinculado, actualizaciones
vdsetup wow <carpeta> [--locale-client <carpeta>|none] [--include <mpq>|none]   elegir y validar el WoW 1.12.1
vdsetup bake [--only <paso>] [--force] hornear (con progreso); solo lo que falta o cambió
vdsetup serve [--port] [--ip] [--wow]  servidor en la red local: QR en la terminal y código
vdsetup adb help|setup|pair|connect|devices|install|push|send-wow   la vía ADB, guiada
vdsetup lang [enUS|esES|system]        el idioma del programa
vdsetup update [--check]               actualizarse, descargar el APK nuevo (22.5)
```

La ventana (hecha así): Dear ImGui sobre SDL3 con su renderizador 2D, un hilo por operación larga y nada de CPU en
reposo; en macOS se compila solo lo del Setup (`VD_BUILD_GAME=OFF`) y el `.app` lleva dentro `vd_baker`, `vdsetup` y el
APK. Ajustes en `setup.json` y la caché de SHA-256 en `hashes.json` (por tamaño y fecha), en `%LOCALAPPDATA%\VanillaDroid`
(macOS: `~/Library/Application Support/VanillaDroid`; `VD_SETUP_DIR` o `--data-dir` los cambian); los packs, por
defecto en su carpeta `packs`. El Setup hornea con `vd_baker` como **proceso hijo**, un paso por proceso (aislado de
un fallo, el mismo binario que llevará el móvil, su log como progreso), y solo los pasos que faltan, los de receta
distinta, los que quedaron a medias o todos si cambió el idioma o el conjunto de MPQ.

- **Validar el WoW**: la carpeta `Data` (sin importar mayúsculas) con los 12 MPQ del juego del 1.12.1 (`base.MPQ` y
  `backup.MPQ` son oficiales pero solo del lanzador: no se hornean ni se envían); como los tamaños cambian con el idioma
  (solo `fonts.MPQ` mide lo mismo en enUS y esES), se comprueba que la cabecera de cada MPQ cuadre con el tamaño del
  fichero, lo que detecta copias cortadas sin leer 5 GB. La versión 1.12.1.5875 del recurso de versión de `WoW.exe` (o
  del `Info.plist` del cliente de Mac), sin ejecutarlo; el idioma por sus DBC (`detectClientLocale`, ahora en
  `vd_formats`): alfabeto latino (enUS, enGB, esES, esMX, frFR, deDE; ADR-0004). Los MPQ que no son oficiales (parches
  de servidores, `patch-3+`, `patch-A..Z`) se listan y por defecto **no** se hornean: `vd_baker --skip-archives` (y
  `--locale-skip-archives` para el segundo cliente). En Mac se elige normalmente la carpeta del cliente de Windows: el
  baker solo lee los MPQ.
- **ADB**: `platform-tools` se descarga de Google al pedirlo (no se reparte). Guía de opciones de desarrollador y de
  depuración inalámbrica (emparejar con código, Android 11+), con el comando equivalente junto a cada paso.

### 5. Vincular la app con el PC por la red local

- `vdsetup serve` (o el botón «Conectar móvil») abre un servidor HTTP en la red local (puerto **47800** o el siguiente
  libre; solo contesta a direcciones locales: 10/8, 172.16/12, 192.168/16, 169.254/16, 100.64/10 y el propio PC) con
  una **clave de emparejamiento** aleatoria (128 bits), y muestra un **QR** con `http://<ip>:47800/?k=<clave>`.
- El móvil lo escanea con su cámara: se abre una página servida por el PC con «Descargar VanillaDroid» (`GET
  /vanilladroid.apk`, sin clave: es nuestro binario publicado) y, con la clave correcta, «Vincular», un enlace
  `vanilladroid://pair?h=<ip>:<puerto>&k=<clave>` que abre la app. Sin lector de QR en la app.
- Alternativa sin cámara: la app pide la dirección y un **código de 6 cifras** que el PC muestra, distinto en cada
  `serve`, y lo cambia por la clave una vez (`GET /pair?code=NNNNNN` → `{"key": ...}`; tras 10 códigos erróneos deja
  de aceptar hasta reiniciar `serve`).
- Con la clave (`?k=` o la cabecera `X-VD-Key`) la app pide `GET /files.json` (`apk`, `client`, `packs[]` con ruta,
  tamaño, SHA-256 y receta, `manifest` y, con `serve --wow`, `wow[]` con los 12 MPQ del juego) y descarga lo que le
  falta o cambió a `packs/*.part`, con reanudación (`Range`, `If-Range` sobre el `ETag`, que es el SHA-256) y
  comprobación del hash antes de renombrarlo; mientras el PC calcula los SHA-256 contesta 503 con su progreso. Lo
  mismo para el WoW (`wow/Data/*.MPQ`). En la red local, sin TLS: la clave autentica y el hash protege los datos. El
  protocolo está al principio de `tools/setup/PhoneServer.h`.
- **Primera vez sin datos**: la app arranca en una pantalla propia (fuente y arte de `app.vdpk`) con «Vincular con el
  PC», «Introducir dirección», «Cómo hacerlo por ADB» y, si encuentra `wow/Data`, «Preparar los datos en el móvil».

### 6. Releases: una etiqueta y su release, en el mismo repositorio

- **Marcar una release** = `scripts/release.ps1 -Version X.Y.Z`: comprueba el árbol limpio, hornea con los clientes de
  referencia (incremental), escribe `release/packs.json` y pide las notas de la versión (`release/notes/vX.Y.Z.md`,
  escritas para el jugador: **nunca los mensajes de commit**, que cuentan el funcionamiento interno), hace el commit y
  la etiqueta `vX.Y.Z`. Subir la etiqueta (`git push origin vX.Y.Z`) lanza la CI.
- **GitHub Actions** (`.github/workflows/release.yml`, solo con etiquetas `v*`; secretos de repositorio): el APK
  `dist` firmado (con `app.vdpk` horneado allí: no necesita el WoW; **la CI nunca ve datos de Blizzard**), el Setup de
  Windows y el de macOS, el `manifest.json` y su firma, y la release en este mismo repositorio con el token de la
  propia ejecución. Una bifurcación no tiene los secretos, y lo instalado solo busca actualizaciones en
  `cer0Labs/VanillaDroid`.
- **El repositorio**: el código con la licencia GPL-3.0 (`LICENSE`; el nombre y el icono de VanillaDroid quedan
  fuera), el README para jugadores y para quien compila, `THIRD_PARTY_NOTICES.md` (las bibliotecas, los datos de
  vMaNGOS, la fuente y los iconos de `art/`, cada uno con su licencia) y las releases.
- **`manifest.json`** (en `releases/latest/download/manifest.json`, una URL fija):

  ```json
  { "version": "0.2.0", "versionCode": 2000,
    "apk":   { "url": "...", "size": 0, "sha256": "..." },
    "setup": { "windows": { "url": "...", "sha256": "..." }, "macos": { "url": "...", "sha256": "..." } },
    "packs": { "models": "<receta>", "world_azeroth": "<receta>", "...": "..." },
    "notes": "https://github.com/cer0Labs/VanillaDroid/releases/tag/v0.2.0" }
  ```

  Va firmado (**ECDSA P-256** con Mbed TLS, que ya enlazan la app y el Setup) con una clave nuestra: la pública va en
  el código, la privada en el secreto de la CI y en una copia fuera de línea. Sin firma válida no se instala nada:
  una cuenta de GitHub comprometida no puede repartir un programa malicioso. Hecho así (`core/ReleaseSignature`):
  `manifest.json.sig` es el base64 de la firma DER sobre el SHA-256 de los bytes exactos de `manifest.json` (lo que da
  `openssl dgst -sha256 -sign`); la clave pública, el punto sin comprimir de 65 bytes en
  `release/release-key.pub.hex`, compilado dentro; el manifiesto lleva además `format` y `date`. Mientras esa clave sea
  la de prueba (`tests/data/release-test-key.pem`), `release.ps1` y la CI se niegan a publicar.
- **Los zips del Setup llevan el APK**: Windows, los ficheros en la raíz (`VanillaDroidSetup.exe`, `vdsetup.exe`,
  `vd_baker.exe`, `VanillaDroid.apk`, la licencia y los avisos); macOS, `VanillaDroid Setup.app/` con `vd_baker`, `vdsetup` y
  el APK en `Contents/MacOS`. Así el Setup sirve al móvil el APK de su misma versión.
- **Antes de la primera release** se crean la clave real (`scripts/release-key.ps1`, la privada fuera de todo
  repositorio y con copia fuera de línea), la clave de firma de Android y los secretos de repositorio
  (`RELEASE_SIGNING_KEY`, `ANDROID_KEYSTORE_B64`, `VD_KEYSTORE_PASSWORD`, `VD_KEY_ALIAS`, `VD_KEY_PASSWORD`).

### 7. Actualizaciones

- **Setup**: al abrirse (y cada pocas horas abierto) lee el manifiesto. Si hay versión nueva la descarga, comprueba
  el SHA-256 y se sustituye (Windows: renombra el ejecutable en uso y arranca el nuevo; Mac: cambia el `.app`). Luego
  ofrece instalar el APK nuevo (Wi-Fi o ADB) y re-hornear y enviar solo los packs cuya receta cambió.
- **App**: al arrancar con red lee el manifiesto (HTTPS del sistema por una clase Java pequeña) y avisa. El APK se
  descarga con `DownloadManager`, se comprueba y se instala con `PackageInstaller`: Android pide un toque para
  confirmarlo (no permite instalar en silencio una app instalada a mano).
  - Solo cambia código o lo nuestro → basta el APK.
  - Cambian recetas y el WoW está en el móvil → tras instalar, la app nueva re-hornea allí solo esos pasos.
  - Cambian recetas y el WoW no está → pide abrir el Setup en el PC (o copiar el WoW al móvil).
- HTTPS: la app y el Setup usan el TLS del sistema; Mbed TLS sigue solo para la sesión del juego y las firmas. Hecho
  así: la app, una clase Java (`Updates.java`: `HttpsURLConnection`, `DownloadManager`, `PackageInstaller`); el Setup, el
  `curl` que traen Windows 10+ y macOS (el de System32 primero); el juego de escritorio de desarrollo, WinHTTP.
- Detalles: el Setup mira como mucho cada 6 h (`update.json`), se sustituye renombrando lo que reemplaza a `*.old`
  (y lo deshace si un paso falla), se relanza con `--updated-from` (que corta un bucle si la versión nueva no arranca)
  y borra los `*.old` en el siguiente arranque; una compilación de desarrollo nunca se sustituye. En la app, «Más
  tarde» dura 24 h; el aviso sale en las pantallas de acceso. `VD_UPDATE_URL` (desarrollo) apunta a otro manifiesto, y
  las variantes de desarrollo permiten HTTP sin cifrar para probar con una release servida en la red local
  (`usesCleartextTraffic` falso en `dist`).

### 8. Hornear en el móvil

- El baker se compila también para Android (arm64) y corre en un **proceso aparte (`:baker`)** dentro de un servicio
  en primer plano con notificación de progreso: nunca en el proceso del juego. La regla 7 («el runtime solo lee
  packs») sigue valiendo para el juego.
- El WoW copiado vive en `files/wow/Data` del almacenamiento EXTERNO de la app (`/sdcard/Android/data/<id>/files`, el
  único sitio al que llega `adb`). Espacio: WoW 5,05 GB + packs 4,7 GB (la caché de las texturas solo existe durante su
  paso, para reanudarlo). Se avisa antes de copiar y se recomienda hornear cargando; el número de hilos baja con la
  temperatura, que lee el propio servicio (`PowerManager`: el juego puede estar cerrado).
- Hecho así: el `vd_baker` de consola va en el APK como `libvdbaker.so` (Android solo deja ejecutar desde
  `nativeLibraryDir`) y lo lanza `BakeService.java` (proceso `:baker`, servicio en primer plano `dataSync`); el juego y
  el servicio se hablan por `plan.json` y `status.json` en `files/bake/`; un paso terminado no se repite.
- El tiempo de hornear en el Fold5 se mide **al principio** (la mayor incógnita); si fuera inaceptable, el móvil solo
  re-hornea pasos pequeños y lo grande sigue en el PC.

### 9. Binarios endurecidos

- La variante `dist` (y el Setup) se compila endurecida (`VD_DIST`), para que lo publicado no lleve nada de
  desarrollo:
  - sin interruptores `VD_*`: se leen con `VD_DEV_ENV` (`core/DevEnv.h`), que en `dist` es siempre nulo, así que ni sus
    nombres ni el código que activan llegan al binario; tampoco los extras de desarrollo del intent (cuenta,
    contraseña, rutas de prueba);
  - sin la capa de depuración: `Renderer` no crea Dear ImGui y `Game` la trata como una constante falsa, de modo que sus
    paneles, la galería (`UiGallery::active()`) y la propia biblioteca se quedan fuera al enlazar;
  - las bibliotecas de terceros que compilamos (LZ4, Mbed TLS, volk, VMA, ImGui) con `-fvisibility=hidden` como las
    nuestras: el `.so` no exporta ningún símbolo `vd::` ni de ellas;
  - los shaders ya salen sin nombres de `glslc -O`; los asserts ya no existen en release; símbolos quitados (los
    completos se guardan aparte para leer los cuelgues) y sin PDB en Windows;
  - ninguna ruta del PC que compila (ni su carpeta personal ni la del repositorio): `vd_map_build_paths`
    (`cmake/CompilerOptions.cmake`) recorta con `-ffile-prefix-map` / `/d1trimfile` las rutas de las fuentes, de la
    compilación, de las dependencias y del NDK en todos los objetivos, y el `vd_baker` de `dist` no lleva sus carpetas
    por defecto (la CI hornea `app.vdpk` con `--locale-dir locale --art-dir art`).

## Consecuencias

- **Java deja de ser solo `MainActivity`**: unas clases pequeñas para lo que Android no ofrece en C++ (instalar un APK,
  el servicio del baker, el enlace de vinculación, HTTPS y descargas).
- Nuevas dependencias, solo en el Setup: SDL3 (zlib) y un generador de QR (qrcodegen, MIT); se añaden en
  `cmake/Dependencies.cmake` en el paso que las necesita. El baker tiene que compilar en macOS, Linux (CI) y Android.
- Las reglas 1 y 7 del proyecto y la de Java del stack se reescriben al cerrar la fase con lo que dice este ADR.
- Sin certificados de pago por ahora: Windows SmartScreen y macOS Gatekeeper avisarán la primera vez; el README explica
  cómo abrirlo. Se revisará más adelante.
- La app solo entra en servidores con Warden desactivado (no se emula; fase 18). Un vMaNGOS modificado podría admitirla
  sin quitar Warden a los clientes de PC mediante la atestación de claves de Android; queda fuera de esta fase.
- `docs/ANDROID.md` y los scripts siguen valiendo para desarrollo (`dev.cer.vanilladroid`).
- La CI consume minutos del plan de GitHub (los de macOS cuentan bastante más): solo corre con etiquetas.
