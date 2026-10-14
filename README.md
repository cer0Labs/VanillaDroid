# VanillaDroid

[English](#english) · [Español](#español)

**Downloads · Descargas:** [latest release · última versión](https://github.com/cer0Labs/VanillaDroid/releases/latest)

> VanillaDroid is not affiliated with, endorsed or sponsored by Blizzard Entertainment. World of Warcraft and Blizzard
> are trademarks of Blizzard Entertainment, Inc. No game file is distributed here: you need your own copy of the game.
>
> VanillaDroid no está afiliado, respaldado ni patrocinado por Blizzard Entertainment. World of Warcraft y Blizzard son
> marcas de Blizzard Entertainment, Inc. Aquí no se reparte ningún fichero del juego: necesitas tu propia copia.

---

## English

VanillaDroid is a native **World of Warcraft 1.12.1** client for Android phones, written from scratch for the phone:
its own graphics engine (C++20 and Vulkan) designed for 60 fps and a touch interface made for playing with your thumbs.
It is neither an emulator nor a port of the original client: it uses the data of **your own copy** of the game, which
**VanillaDroid Setup** prepares on your computer (or the phone prepares by itself).

### What you need

- An **Android 11 or newer** phone with **Vulkan** (64-bit, arm64) and about **5 GB free** (5 GB more if you want to
  copy the game to the phone so it can update without the computer).
- A **PC with Windows 10 or 11** (64-bit) or a **Mac**.
- **Your World of Warcraft 1.12.1 (build 5875) client** in a Latin-alphabet language (English, Spanish, French,
  German...). The Windows client also works on a Mac.
- A **1.12.1 server without Warden** (the PC client's anti-cheat check), for example your own
  [vMaNGOS](https://github.com/vmangos/core) server. VanillaDroid cannot log in to servers that require Warden.

### Install

1. From the [latest release](https://github.com/cer0Labs/VanillaDroid/releases/latest) download
   `VanillaDroidSetup-X.Y.Z-windows.zip` (or `-macos.zip`) and unzip it. The app's APK is inside.
2. Open **VanillaDroid Setup** (`VanillaDroidSetup.exe`, or "VanillaDroid Setup" on a Mac). The first time, the system
   will warn that it does not know the program: see [Opening an unsigned program](#opening-an-unsigned-program).
3. Choose your World of Warcraft 1.12.1 folder. The Setup checks that it is the right version and **prepares the data**
   for the phone (the first time it takes a while; afterwards it only prepares what changes).
4. Get the app and the data onto the phone in one of two ways:
   - **Over Wi-Fi** (phone and computer on the same network): in the Setup's "Phone over Wi-Fi", scan the QR code
     with the phone's camera. On the page that opens, "Download VanillaDroid" installs the app (Android asks for
     permission to install apps from the browser) and "Link" opens the app, which downloads the data by itself.
     Without a camera: in the app, "Enter address", with the address and the 6-digit code the Setup shows.
   - **Over ADB** (Android 11 wireless debugging or a USB cable): in "Phone with ADB", the Setup explains how to turn
     on the developer options, pairs the phone, installs the app and sends the data.
5. Open VanillaDroid on the phone, [set your server](#your-server) and play.

Everything the Setup does can also be done from a terminal with `vdsetup` (`vdsetup help`): `vdsetup wow <folder>`,
`vdsetup bake`, `vdsetup serve`, `vdsetup adb help`.

### Your server

VanillaDroid connects to the server you choose, like the `set realmlist` line of the original client's
`realmlist.wtf`. On the login screen, tap **Server** and type your server's login address: a host name or an IP
address, with the port when it is not the usual 3724 (`logon.example.org`, `192.168.1.20`, `play.example.org:3725`).
The app remembers it; tap it again to change it.

### Updates

The Setup and the app tell you when there is a new version. The Setup updates itself and offers to install the new app
on the phone; the app can also download and install it (Android asks for a tap to confirm). The data only has to be
prepared again when a version changes something in it, and then only what changed: on the computer or, if you copied
the game to the phone, on the phone itself.

Besides the APK and the Setups, every release publishes `manifest.json`, with the size and SHA-256 of each file, and
its signature, `manifest.json.sig`: the Setup and the app install nothing that was not signed with the release key.

### Opening an unsigned program

VanillaDroid Setup has no paid code signature, so the first time:

- **Windows** (SmartScreen, "Windows protected your PC"): click "More info", then "Run anyway".
- **macOS** (Gatekeeper, "cannot be opened because the developer cannot be verified"): open System Settings → Privacy &
  Security and click "Open Anyway" next to the message about VanillaDroid Setup (before macOS 15, right-click → Open
  also works). From the Terminal: `xattr -dr com.apple.quarantine "VanillaDroid Setup.app"`.

### Building from source

The engine is one CMake project for Android and for Windows (the Windows build is for development; performance is
measured on the phone). CMake downloads the libraries it needs. The design documents in [docs/](docs/) are in Spanish;
code and comments are in English.

- **Windows**: Visual Studio 2019 16.11 or newer with the C++ workload (the `windows` preset uses the Visual Studio 2019
  generator; with a newer one add `-G "Visual Studio 17 2022"` or `-G "Visual Studio 18 2026"`), CMake 3.22 or newer,
  a GPU with Vulkan, and `glslc` for the shaders (found in the Android NDK or the Vulkan SDK, or
  `-DVD_GLSLC=<glslc.exe>`).

  ```powershell
  cmake --preset windows -DVD_WOW_DIR="D:/Games/World of Warcraft 1.12"
  cmake --build --preset windows-release
  ctest --preset windows-release
  .\build\windows\tools\baker\RelWithDebInfo\vd_baker.exe bake --out packs          # your client -> packs/
  .\build\windows\apps\desktop\RelWithDebInfo\vanilladroid_desktop.exe --packs packs
  ```

- **Android**: Android SDK with NDK `28.2.13676358` and CMake `3.22.1`, JDK 21.
  `.\scripts\android.ps1 -Variant release -Install -Run` builds, installs and starts the app on the connected phone;
  the data goes over with VanillaDroid Setup or `.\scripts\push-data.ps1`. Details in
  [docs/ANDROID.md](docs/ANDROID.md).

How it works: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) (engine), [docs/FORMATS.md](docs/FORMATS.md) (the baked
packs), [docs/PROTOCOL.md](docs/PROTOCOL.md) (network), [docs/PERFORMANCE.md](docs/PERFORMANCE.md) (budgets),
[docs/MOBILE_CONTROLS.md](docs/MOBILE_CONTROLS.md) (touch controls) and the decisions in
[docs/decisions/](docs/decisions/).

### License

VanillaDroid is free software: you can redistribute it and/or modify it under the terms of the
[GNU General Public License version 3](LICENSE). It comes with NO WARRANTY. The VanillaDroid name and icon are not
covered by that license. The third-party libraries, data and art it uses have their own licenses:
[third-party notices](THIRD_PARTY_NOTICES.md).

---

## Español

VanillaDroid es un cliente nativo de **World of Warcraft 1.12.1** para móviles Android, escrito desde cero para el
móvil: su propio motor gráfico (C++20 y Vulkan) pensado para 60 fps y una interfaz táctil hecha para jugar con los
pulgares. No es un emulador ni una adaptación del cliente original: usa los datos de **tu propia copia** del juego, que
prepara **VanillaDroid Setup** en tu ordenador (o el propio móvil).

### Qué necesitas

- Un móvil **Android 11 o posterior** con **Vulkan** (64 bits, arm64) y unos **5 GB libres** (otros 5 GB si quieres
  copiar el juego al móvil para que se actualice sin el ordenador).
- Un **PC con Windows 10 u 11** (64 bits) o un **Mac**.
- **Tu cliente de World of Warcraft 1.12.1 (build 5875)** en un idioma de alfabeto latino (inglés, castellano,
  francés, alemán...). El cliente de Windows sirve también en el Mac.
- Un **servidor de la versión 1.12.1 sin Warden** (la comprobación antitrampas del cliente de PC), por ejemplo uno
  propio con [vMaNGOS](https://github.com/vmangos/core). VanillaDroid no puede entrar en servidores que exigen Warden.

### Instalar

1. Descarga de la [última versión](https://github.com/cer0Labs/VanillaDroid/releases/latest)
   `VanillaDroidSetup-X.Y.Z-windows.zip` (o `-macos.zip`) y descomprímelo. El APK de la app ya va dentro.
2. Abre **VanillaDroid Setup** (`VanillaDroidSetup.exe`, o «VanillaDroid Setup» en el Mac). La primera vez el
   sistema avisará de que no conoce el programa: mira [Abrir un programa sin firmar](#abrir-un-programa-sin-firmar).
3. Elige la carpeta de tu World of Warcraft 1.12.1. El Setup comprueba que es la versión buena y **prepara los
   datos** para el móvil (la primera vez tardará un poco; después solo prepara lo que cambia).
4. Pasa la app y los datos al móvil de una de estas dos formas:
   - **Por Wi-Fi** (el móvil y el ordenador en la misma red): en «Móvil por Wi-Fi» del Setup, escanea el código QR
     con la cámara del móvil. En la página que se abre, «Descargar VanillaDroid» instala la app (Android pedirá
     permiso para instalar apps desde el navegador) y «Vincular» abre la app, que descarga los datos sola. Sin
     cámara: en la app, «Introducir dirección», con la dirección y el código de 6 cifras que muestra el Setup.
   - **Por ADB** (depuración inalámbrica de Android 11 o un cable USB): en «Móvil con ADB», el Setup explica cómo
     activar las opciones de desarrollador, empareja el móvil, instala la app y envía los datos.
5. Abre VanillaDroid en el móvil, [elige tu servidor](#tu-servidor) y a jugar.

Todo lo del Setup se puede hacer también por consola con `vdsetup` (`vdsetup help`): `vdsetup wow <carpeta>`,
`vdsetup bake`, `vdsetup serve`, `vdsetup adb help`.

### Tu servidor

VanillaDroid se conecta al servidor que elijas, como la línea `set realmlist` del `realmlist.wtf` del cliente
original. En la pantalla de acceso, toca **Servidor** y escribe la dirección de acceso de tu servidor: un nombre o una
IP, con el puerto si no es el habitual 3724 (`logon.example.org`, `192.168.1.20`, `play.example.org:3725`). La app la
recuerda; tócala otra vez para cambiarla.

### Actualizaciones

El Setup y la app avisan cuando hay una versión nueva. El Setup se actualiza solo y ofrece instalar la app nueva en el
móvil; la app también puede descargarla e instalarla (Android pide un toque para confirmar). Solo hay que volver a
preparar datos cuando la versión cambia algo de ellos, y entonces solo lo que cambió: en el ordenador o, si copiaste el
juego al móvil, en el propio móvil.

Cada versión publica, además del APK y los Setup, `manifest.json` con el tamaño y el SHA-256 de cada fichero y su
firma, `manifest.json.sig`: el Setup y la app no instalan nada que no esté firmado con la clave de las releases.

### Abrir un programa sin firmar

VanillaDroid Setup no lleva una firma de pago, así que la primera vez:

- **Windows** (SmartScreen, «Windows protegió su PC»): pulsa «Más información» y luego «Ejecutar de todas formas».
- **macOS** (Gatekeeper, «no se puede abrir porque no se puede verificar el desarrollador»): abre Ajustes del Sistema
  → Privacidad y seguridad y pulsa «Abrir igualmente» junto al aviso de VanillaDroid Setup (en versiones anteriores a
  macOS 15, también vale clic derecho → Abrir). Desde la Terminal:
  `xattr -dr com.apple.quarantine "VanillaDroid Setup.app"`.

### Compilar desde el código

El motor es un único proyecto de CMake para Android y para Windows (la versión de Windows es para desarrollar; el
rendimiento se mide en el móvil). CMake descarga las bibliotecas que necesita. Los documentos de diseño de
[docs/](docs/) están en castellano; el código y sus comentarios, en inglés.

- **Windows**: Visual Studio 2019 16.11 o posterior con la carga de trabajo de C++ (el preset `windows` usa el
  generador de Visual Studio 2019; con uno más nuevo añade `-G "Visual Studio 17 2022"` o `-G "Visual Studio 18 2026"`),
  CMake 3.22 o posterior, una GPU con Vulkan y `glslc` para los shaders (lo encuentra en el NDK de Android o en el
  SDK de Vulkan, o `-DVD_GLSLC=<glslc.exe>`).

  ```powershell
  cmake --preset windows -DVD_WOW_DIR="D:/Juegos/World of Warcraft 1.12"
  cmake --build --preset windows-release
  ctest --preset windows-release
  .\build\windows\tools\baker\RelWithDebInfo\vd_baker.exe bake --out packs          # tu cliente -> packs/
  .\build\windows\apps\desktop\RelWithDebInfo\vanilladroid_desktop.exe --packs packs
  ```

- **Android**: el SDK de Android con el NDK `28.2.13676358` y CMake `3.22.1`, y el JDK 21.
  `.\scripts\android.ps1 -Variant release -Install -Run` compila, instala y abre la app en el móvil conectado; los
  datos se pasan con VanillaDroid Setup o con `.\scripts\push-data.ps1`. Detalles en [docs/ANDROID.md](docs/ANDROID.md).

Cómo funciona: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) (el motor), [docs/FORMATS.md](docs/FORMATS.md) (los
packs), [docs/PROTOCOL.md](docs/PROTOCOL.md) (la red), [docs/PERFORMANCE.md](docs/PERFORMANCE.md) (presupuestos),
[docs/MOBILE_CONTROLS.md](docs/MOBILE_CONTROLS.md) (los controles táctiles) y las decisiones de
[docs/decisions/](docs/decisions/).

### Licencia

VanillaDroid es software libre: puedes redistribuirlo y modificarlo según los términos de la
[GNU General Public License versión 3](LICENSE). Viene SIN NINGUNA GARANTÍA. El nombre y el icono de VanillaDroid no
están cubiertos por esa licencia. Las bibliotecas, los datos y el arte de terceros que usa tienen sus propias
licencias: [avisos de terceros](THIRD_PARTY_NOTICES.md).
