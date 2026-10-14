# ADR-0002 — Interfaz táctil: UI inmediata propia, atlas horneados y texto SDF sin FreeType

- **Estado:** aceptada (inicio de la fase 7)
- **Contexto:** la fase 7 necesita una interfaz táctil completa (HUD, diálogos, chat, menús) con la estética del
  cliente original, a ≤ 1,5 ms de CPU por frame en el móvil y sin tirones. El plan inicial (ARCHITECTURE §9) era
  FreeType en el runtime generando un atlas SDF dinámico.

## Decisión

| Pieza | Elección |
|---|---|
| Texto | **SDF horneado en el PC** por el baker (`bake --only ui`) con las 4 fuentes TTF del cliente (FRIZQT__, ARIALN, MORPHEUS, SKURRI): un atlas R8 por fuente, métricas y *kerning* en el pack. Juego de caracteres fijo: ASCII, Latin-1, Latin extendido A y la puntuación tipográfica habitual |
| Rasterizado SDF | **stb_truetype v1.26**, la copia que ya trae Dear ImGui (`imstb_truetype.h`, fijada por la etiqueta de ImGui): solo en el baker |
| Sprites | **Atlas horneado** con las texturas de `Interface\` que usa la UI (página 2D array ASTC 4×4, sprites alineados a bloques de 4 px con borde extendido para que el filtrado no mezcle vecinos) |
| Iconos | **Array de iconos en la GPU** con huecos que se llenan bajo demanda desde `textures.vdpk` (un icono = una capa 64×64); LRU cuando se llena |
| Modelo de UI | **Inmediata** (se describe cada frame) con *hit test* contra los rectángulos del frame anterior: sin árbol de widgets ni retención de estado salvo lo que cada pantalla guarda |
| Unidades | **dp** (píxel lógico × escala de densidad × escala del jugador), anclas a bordes y *safe areas* |
| Render | **Un pipeline**, un *descriptor set* (atlas + iconos + fuentes) y una lista de quads en un buffer por frame; solo se parte en varias llamadas por *scissor* o por textura externa (minimapa) |
| Fundido | **Alfa premultiplicado** (`ONE`, `ONE_MINUS_SRC_ALPHA`): el shader multiplica el color por el alfa, y un quad marcado como aditivo (el `alphaMode="ADD"` de FrameXML: chispas, destellos, brillos) escribe alfa 0 y suma su luz. Así los dos modos del cliente caben en el mismo pipeline y en el mismo *draw call* |
| Pantallas | **Vistas puras** (`app/Dialogs`): cada diálogo dibuja un *view-model* que rellena quien lo llama y devuelve lo que hizo el jugador ese frame; no tocan la red ni el estado del juego. Dos perfiles de pantalla: `Wide` (20:9 y la exterior del Fold5, 23:9: diálogos bajos a dos columnas) y `Square` (la interior, ~6:5: una columna) |
| Texto escrito | **Teclado del sistema** (GameTextInput) sobre un único `ui::TextInput` de capacidad fija; el campo dice su tipo (`Plain`, `Secret`, `Message`) y Android elige el teclado (contraseña enmascarada y sin aprendizaje, nombres sin sugerencias, chat con tecla "enviar"). Al terminar de editar el texto se borra de la memoria |

## Por qué

1. **Regla 7 ("el runtime solo lee packs")**: rasterizar glifos en el móvil es transformar datos en caliente. El
   baker ya tiene las fuentes a mano (están en los MPQ) y el resultado cabe en unos pocos MB.
2. **Cero tirones**: un atlas dinámico rasteriza al ver un carácter nuevo (el chat puede traer cualquiera); con el
   juego de caracteres del cliente enUS horneado entero, nunca hay que hacerlo.
3. **APK más pequeño y sin dependencia nativa nueva**: FreeType no entra en el APK. stb_truetype genera SDF exactos
   desde los contornos y ya está descargado.
4. **UI inmediata**: la UI de un juego cambia cada frame (barras, cooldowns, textos flotantes); describirla cada
   frame con ~500 quads cuesta décimas de milisegundo y evita sincronizar un árbol retenido con el estado del
   juego. El *hit test* contra el frame anterior da respuesta al toque sin un frame de retraso.

5. **Premultiplicado y no un segundo pipeline**: los brillos aditivos del cliente se mezclan con quads normales en
   el mismo orden de dibujo; con dos pipelines habría que partir la lista cada vez que cambian. Las texturas
   aditivas que el cliente guarda sin alfa se hornean con alfa = brillo y color / alfa: dibujadas como aditivas
   suman exactamente lo mismo que en el cliente, y dibujadas normales quedan como un brillo razonable.

## Consecuencias

- Caracteres fuera del juego horneado (p. ej. CJK o cirílico en el chat) se muestran como "?": igual que el
  cliente enUS original, que tampoco los tiene en sus fuentes.
- Cambiar el juego de caracteres o las fuentes = volver a hornear `ui.vdpk` (segundos).
- ARCHITECTURE §9 y `cmake/Dependencies.cmake` se actualizan: FreeType sale del plan.
- Las ventanas del juego (PNJ, misiones, registro, botín, chat, menú, ajustes, confirmaciones y la entrada con
  selección de personaje) se prueban sin servidor con `VD_UI_GALLERY=<página>` (`gossip`, `details`, `progress`,
  `reward`, `log`, `loot`, `chat`, `menu`, `settings`, `confirm`, `widgets`, `login`, `all`) y `--hide-ui`; en
  escritorio, `--fake-keyboard 0.45` simula el teclado del móvil al editar un campo.
