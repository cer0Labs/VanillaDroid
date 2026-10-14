# Arte propio

Arte de VanillaDroid que **no** viene del cliente de Blizzard. El baker lo hornea en `app.vdpk`, el pack de lo
nuestro que viaja dentro del APK (fase 22, ADR-0005 §2): el runtime solo lee packs (regla 7) y un cambio de este arte
llega con el APK, sin volver a hornear nada del WoW de cada jugador. Hay iconos SVG de un color para el racimo táctil
(fase 20, `docs/MOBILE_CONTROLS.md`) y una fuente libre para las pantallas que se ven antes de tener datos.

El icono de la aplicación está en `app-icon/VanillaDroid-transparent.png`. Los tamaños derivados que Android
empaqueta viven en `apps/android/app/src/main/res/mipmap-*/ic_launcher.png`; el manifiesto los referencia como icono
normal y redondo. VanillaDroid Setup (fase 22, `tools/setup/gui`) lleva en Windows `app-icon/VanillaDroid.ico` (16 a
256 px, hecho con Pillow a partir de `VanillaDroid-transparent.png`) y dentro del programa
`VanillaDroid-transparent-512.png` como logo; en macOS el `.icns` se hace al compilar con `sips` e `iconutil` del
sistema (no está en el repositorio).

| Fichero | Uso | Origen |
|---|---|---|
| `touch/jump.svg` | Botón Saltar | "Jump To Top", [SVG Repo 374927](https://www.svgrepo.com/svg/374927/jump-to-top): CC BY 4.0 |
| `touch/mount.svg` | Botón Montura / viaje | "Horse Head Outline", [SVG Repo 173978](https://www.svgrepo.com/svg/173978/horse-head-outline): CC0 (antes "Saddle") |
| `touch/target.svg` | Botón Cambiar de objetivo | "Target", [SVG Repo 503107](https://www.svgrepo.com/svg/503107/target): MPL 2.0 (su SVG va en los avisos de terceros) |
| `touch/settings.svg` | Botón Ajustes | "Settings", [SVG Repo 511122](https://www.svgrepo.com/svg/511122/settings): CC BY 4.0 |
| `touch/talk.svg` | Botón Hablar | "Talk Chat Message...", [SVG Repo 338040](https://www.svgrepo.com/svg/338040/talk-chat-message-communication-speech-conversation-interaction): CC BY 4.0 |
| `fonts/DroidSans.ttf` | Fuente propia (`ui::FontId::App`): pantallas sin datos y, sin `ui.vdpk`, todo el texto | Droid Sans 1.00 («Digitized data copyright © 2007, Google Corporation», Ascender Corporation; «Droid» es marca de Google), copiada de `Android Studio\jbr\lib\fonts`. Licencia Apache 2.0 (la que declara la propia fuente): `fonts/LICENSE-DroidSans.txt`, copiado del `NOTICE.txt` de `platform-tools` del SDK de Android |

Las licencias de los iconos son las de su página de SVG Repo y van en `THIRD_PARTY_NOTICES.md`, en la raíz del
repositorio. Los CC BY exigen atribución (el enlace a su página, que da autor, origen y licencia, y decir que se
modificaron); un icono nuevo necesita su fila aquí y allí. La fuente se puede repartir dentro del APK con el texto de
su licencia (va en `THIRD_PARTY_NOTICES.md`). El icono de la aplicación es propio y no está cubierto por la GPL.

## Cómo entran en el juego

- `vd_baker bake --only app` (sin el WoW: no abre ningún MPQ) los hornea en `app.vdpk`; `--art-dir` cambia la carpeta
  (por defecto esta). La lista de iconos es `kOwnArt` y la de fuentes `kOwnFonts`, en
  `tools/formats/src/bake/AppPackBaker.cpp`.
- Los iconos se rasterizan (`bake::rasterizeSvgIcon`, `tools/formats/src/bake/UiBaker.cpp`) a 128×128 en blanco con la
  cobertura como alfa, en un atlas propio con una ruta propia (`Interface\VanillaDroid\Touch-Jump.blp`...); el runtime
  pone sus páginas detrás de las del atlas de `ui.vdpk` (`ui/UiAssets`).
- La fuente se hornea como campos de distancia, igual que las del cliente (`bake::bakeUiFonts`: ASCII, Latin-1 y Latin
  extendido A), en capas propias detrás de las de `ui.vdpk`.
- En el escritorio `app.vdpk` va en `packs/`; en Android, `scripts/android.ps1` lo hornea en los assets del APK
  (`apps/android/app/build/generated/vdassets`) y la app lo copia a su almacenamiento interno al instalar una versión
  nueva.
- El HUD dibuja los iconos teñidos sobre el fondo oscuro del botón (`TouchButtonView::glyph`, `app/HudTouchBar.cpp`).
- Qué admite el rasterizador: `<path>` con `d` (M L H V C S Q T A Z, absolutos y relativos), relleno de un color con la
  regla no-cero y el `viewBox`. Sin transformaciones, trazos ni otras formas: si un SVG las usa, el baker da error en
  vez de dibujarlo mal. Para añadir un icono: el SVG aquí, una línea en `kOwnArt` (`AppPackBaker.cpp`) y su hash en
  `app/HudShared.h`.
