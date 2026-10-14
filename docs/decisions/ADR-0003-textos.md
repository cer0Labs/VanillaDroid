# ADR-0003: Textos de la interfaz en claves, del cliente y propios, traducibles

Estado: aceptada

## Contexto

Hasta la fase 8 los textos de la interfaz estaban escritos en castellano dentro del código (ventanas, mensajes,
registro de combate, tooltips…). Eso tiene tres problemas: no se puede cambiar de idioma; muchos de esos textos ya
existen en el cliente original con su redacción exacta (y traducidos en los clientes de otros idiomas); y cada
traducción a mano se aleja del juego original.

El cliente 1.12 no escribe sus textos en el código de su interfaz: los lee de tablas Lua,
`Interface\FrameXML\GlobalStrings.lua` (5.470 textos: errores `ERR_*`, registro de combate `SPELLLOG*` /
`COMBATLOG_*`, botín `LOOT_*`, tooltips `ITEM_*`, instructor `TRAINER_*`, chat `CHAT_*`…) e
`Interface\GlueXML\GlueStrings.lua` (429: acceso, selección y creación de personaje). Los nombres (hechizos, zonas,
razas, clases, facciones, habilidades) están en los DBC, también por idioma.

## Decisión

1. **Ningún texto que lea el jugador se escribe en el código**: se pide por clave a `core/Text`
   (`text::get("ERR_INV_FULL")`, `text::format(buffer, "LOOT_ITEM_SELF", nombre)`).
2. **Si el texto existe en el juego, la clave es la del cliente**. El baker lee `GlobalStrings.lua` y
   `GlueStrings.lua` del MPQ y los hornea en `ui.vdpk` (`ui/strings/<locale>`). Con un cliente enUS salen en inglés,
   con la redacción del original; con un cliente de otro idioma saldrían en ese idioma sin hacer nada. Los nombres
   siguen saliendo de los DBC.
3. **Lo que solo tiene este cliente** (controles táctiles, ajustes del móvil, ayudas) va en un diccionario propio en
   el repositorio: `locale/enUS/<área>.lang`, en inglés, con claves `VD_<ÁREA>_<NOMBRE>` y el mismo formato que
   `GlobalStrings.lua` (`CLAVE = "texto";`).
4. **Traducciones**: `locale/<idioma>/<área>.lang` (de momento `esES`, con el castellano que ya estaba escrito)
   traduce claves propias y, si se quiere, del cliente; lo que falte cae al inglés. El idioma se elige con `locale`
   en `client.cfg` (por defecto `enUS`).
5. **Formato seguro**: los textos traen especificadores de printf (`%s %d %c %.1f`, posicionales `%2$s` para
   reordenar en una traducción). `text::vformat` los interpreta él mismo con argumentos tipados; nunca se pasa a
   printf un formato que venga de datos.
6. Los textos que envía el servidor (misiones, diálogos de PNJ, chat) se muestran tal cual.

## Consecuencias

- La interfaz pasa a inglés por defecto, igual que el cliente original enUS; el castellano queda como traducción.
- Cada texto nuevo de la interfaz obliga a buscar primero su clave en `GlobalStrings.lua` (`vd_baker extract --file
  "Interface\FrameXML\GlobalStrings.lua" --out ...`).
- La búsqueda es una búsqueda binaria en la tabla mapeada (sin asignaciones); `ui.vdpk` crece ~0,5 MB por idioma.
- Una clave que falta se ve en pantalla tal cual y avisa una vez en el log; una clave de una traducción que no existe
  en inglés es una errata y el baker avisa.
- Actualización (fase 21, [ADR-0004](ADR-0004-idiomas.md)): un idioma con cliente propio (el esES) toma los textos de
  ese cliente; `locale/<idioma>` solo cuenta para lo que el cliente no define (nuestras claves `VD_*`).
