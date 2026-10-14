# ADR-0004: Clientes en otros idiomas: packs de idioma superpuestos

Estado: aceptada

## Contexto

Los packs se hornean de un cliente 1.12.1 (5875) en inglés (`C:\World of Warcraft 1.12`). Existe el mismo build en
castellano (`C:\World of Warcraft 1.12 esES`: el mismo `WoW.exe` byte a byte) y queremos poder jugar en ese idioma,
y en general aceptar el cliente de cualquier idioma del 1.12 (enUS, koKR, frFR, deDE, zhCN, zhTW, esES, esMX).

Comparando fichero a fichero los dos clientes (`vd_baker scan-locale --diff 1`):

- **DBC**: 51 de 158 tablas cambian, todas por sus textos. Un texto del 1.12 son 8 columnas de cadena, una por
  idioma en ese orden, y una de *flags*; cada cliente rellena y lee **solo la suya**: el enUS la 1.ª, el esES la
  7.ª (la 1.ª queda vacía). El resto de campos es igual (las cadenas que no se traducen, rutas e iconos, cambian de
  posición en el bloque de cadenas, no de contenido). `SpellAuraNames` y `SpellEffectNames` traen filas de más
  (tablas de desarrollo que no usamos).
- **Textos de la interfaz**: `GlobalStrings.lua` y `GlueStrings.lua` con las mismas claves y los mismos argumentos
  (el esES usa `%2$s` y un `$g`); las órdenes `/` (`SLASH_*`) y de emotes (`EMOTEn_CMDm`) también se traducen.
  `GlueLocalization.lua`: el aviso ESRB del login y el botón «Randomize» de la creación solo existen en enUS.
- **Texturas**: solo cambian de verdad los 631 mapas del mundo con nombres pintados (`Interface\WorldMap`); unas 750
  más son la misma imagen comprimida otra vez.
- **Sonido**: las voces de las criaturas grabadas de nuevo (mismas rutas, otra duración) y las 8 narraciones de raza.
  Los WAV del mismo tamaño son la misma grabación con el ADPCM con pérdida del MPQ aplicado otra vez. ~2.000 voces de
  jugador en carpetas que ninguna tabla nombra (`SoundEntries.dbc` es igual): el 1.12 esES no las reproduce.
- **19 ficheros de FrameXML** cambian, pero son otra revisión del mismo código (arreglos de desplegables, sonidos),
  no adaptaciones al idioma: seguimos el FrameXML enUS.
- **El servidor**: vMaNGOS guarda el idioma que el cliente dice en `AUTH_LOGON_CHALLENGE` y contesta objetos,
  criaturas, misiones, diálogos y sus propios mensajes en él (sus tablas `locales_*`) cuando los tiene. Sus DBC
  son enUS: los nombres de los canales de zona («Trade - City») los reconoce solo en inglés.

## Decisión

1. **Un pack de idioma por cada pack que cambia**, junto a él y con el mismo formato: `db.<idioma>.vdpk`,
   `textures.<idioma>.vdpk`, `sound.<idioma>.vdpk`, con **solo lo que ese idioma cambia y bajo las mismas rutas**.
   `pack::PackReader::openOverlay(idioma)` lo abre encima del base: `find()` devuelve su recurso si lo tiene y
   `findBase()` el del cliente base. Así ningún consumidor de los packs cambia. La cabecera de los packs guarda el
   idioma de sus datos (`PackHeader::locale`, en bytes antes reservados: sin cambio de versión).
2. **El texto del idioma va a la primera columna de cada texto de las DBC**, que es la que lee todo el código
   (los «enUS at N» siguen valiendo). El baker detecta los textos por su forma: 8 campos de los que solo la columna
   del idioma tiene valor, siempre el inicio de una cadena (encuentra los 66 textos de las tablas 5875 del esES,
   comprobado contra la comparación campo a campo con el enUS). Lo mismo si el **cliente base** es de otro idioma.
3. **Los textos de la interfaz de un idioma con cliente propio salen de ese cliente**: su `GlobalStrings.lua` y
   `GlueStrings.lua`, con nuestras claves `VD_*` en inglés debajo y `locale/<idioma>/*.lang` solo para lo que el
   cliente no define. Las ~2.250 traducciones a mano de claves del cliente en `locale/esES` quedan para quien no
   tenga el cliente esES. Van en `ui.vdpk` como los demás idiomas (`ui/strings/<idioma>`).
4. **Las reglas del idioma del cliente son claves derivadas** que el baker escribe al leer cada cliente:
   `VD_GLUE_SHOW_ESRB_RATING` y `VD_CREATE_SHOW_RANDOM_NAME` (de `GlueLocalization.lua`) y
   `VD_MINIMAP_TWENTYFOUR_HOUR_TIME` (si `GlobalStrings.lua` define `TwentyFourHourTime`). Los tokens de los emotes
   (`EMOTEn_TOKEN` de `ChatFrame.lua`) también entran en la tabla.
5. **Por la red, los nombres del cliente base**: los canales de zona se unen con los nombres de las tablas base
   («General - Elwynn Forest») y se muestran traducidos («General - Bosque de Elwynn») con
   `SocialData::displayChannel`. El idioma configurado se dice en `AUTH_LOGON_CHALLENGE`.
6. **Las órdenes del chat** se reconocen por las `SLASH_*` y `EMOTEn_CMDm` del idioma del cliente, como
   `ChatFrame.lua`; el propio esES repite algunas (`/s` es decir y susurrar, `/g` grupo y hermandad): gana el número
   más bajo y, a igualdad, tipos de chat, susurro, `/c`, órdenes. Lo que no nombre la tabla sigue con los nombres
   fijos de antes.
7. **Se configura** con `locale` en `client.cfg` (o `VD_LOCALE`); los clientes de otros idiomas para el baker con
   `--locale-clients`, `VD_WOW_LOCALE_DIRS` o la variable de CMake `VD_WOW_LOCALE_DIRS` (por defecto
   `C:/World of Warcraft 1.12 esES`; una carpeta que no existe se salta).

## Consecuencias

- `vd_baker bake --only locale` hornea los packs de idioma (esES: ~1 min); `--only ui` necesita los clientes de
  otros idiomas para sus textos (sin ellos, el castellano vuelve a ser la traducción sobre el inglés).
- Packs de idioma del esES: ~10 MB de DBC, ~35 MB de mapas, las voces; ~3 % de los packs.
- Cambiar de idioma pide reiniciar el juego (los packs se abren al arrancar).
- **Solo idiomas de alfabeto latino**: las fuentes horneadas cubren Latin-1 y Latin extendido A (esES, esMX, frFR,
  deDE). koKR, zhCN y zhTW necesitan las fuentes de su cliente y un juego de glifos de miles de caracteres: no hecho.
- Con un cliente base en otro idioma y sin cliente enUS, los canales de zona salen en ese idioma por la red y el
  servidor (con DBC enUS) los toma por canales propios: es lo mismo que le pasa al cliente original.
- Lo guardado con nombres (macros, `chat-cache.txt`) sigue con los del idioma en que se guardó.
