# Formatos del cliente 1.12.1 — notas para el baker

Referencia principal: **wowdev.wiki** (secciones marcadas como "Vanilla" / versión ≤ 1.12). Estas notas
recogen lo específico de vanilla que suele confundirse con versiones posteriores.

## MPQ

- Archivos en `C:\World of Warcraft 1.12\Data\`: `dbc`, `fonts`, `interface`, `misc`, `model`, `sound`,
  `speech`, `terrain`, `texture`, `wmo`, `base`, `backup`, `patch` (2 GB) y `patch-2`.
- **Prioridad** (el primero que contiene el fichero gana): `patch-N.MPQ` (N descendente, parches de servidor
  personalizados) > `patch-2.MPQ` > `patch.MPQ` > archivos base. En vanilla los parches **sustituyen ficheros
  completos** (no hay parches incrementales como en Cataclysm).
- Lectura con StormLib. Rutas con `\`, sin distinguir mayúsculas → normalizar a minúsculas y `/`.

**Verificado en el cliente 1.12.1 enUS:**
- 14 archivos, 85.849 ficheros únicos (58.502 BLP, 158 DBC). `patch.MPQ` tiene 34.341 ficheros.
- `backup.MPQ` y `base.MPQ` solo contienen ficheros del launcher (`.exe`, `.html`, `.url`): no se solapan con
  los datos de juego, así que su posición en la prioridad es irrelevante.
- **`patch.MPQ` contiene ~5.700 entradas de 0 bytes** (tiles de minimapa antiguos, algunas pieles de criaturas y
  objetos) que "tapan" la versión real de `texture.MPQ`. El lector las ignora y sigue buscando en archivos de menor
  prioridad (`MpqArchiveSet::read`).

## BLP (BLP2)

- Cabecera `BLP2`, `compression` (1 = paleta de 256 colores, 2 = DXT, 3 = ARGB sin comprimir),
  `alphaDepth` (0/1/8), `alphaType` (DXT1/3/5), `hasMips`, ancho/alto, 16 offsets + 16 tamaños de mip.
- Baker: decodificar a RGBA8 → **ASTC** con astcenc. No depender de BC/DXT en la GPU del móvil.

**Verificado (las 58.502 BLP decodifican sin errores):**
- Censo: DXT1 31.392 · paleta 20.487 · DXT3 6.622 · **DXT5 0** · ARGB 1. Profundidad de alfa: 0, 1 u 8 (nunca 4).
- **Bug del encoder original**: en texturas altas y estrechas, los mips de menos de 4 texels de ancho se guardaron
  con un solo bloque DXT (p. ej. un mip 2×16 ocupa 8 bytes en vez de 32). El decodificador corta la cadena en el
  último mip completo y regenera el resto con filtro de caja hasta 1×1.
- Muchas texturas declaran alfa de 8 bits pero son totalmente opacas: el baker lo detecta y codifica sin alfa.

**Calidad ASTC medida (PSNR frente al original decodificado):** icono 4×4 62 dB, piel 4×4 49 dB, terreno 6×6 35 dB.
Visualmente indistinguible en vegetación con alfa recortado (6×6).

**El alfa no siempre es transparencia** (encontrado en F5): muchas texturas guardan en el alfa una máscara (brillo,
especular, altura del terreno) mientras el color se ve entero. Con `ASTCENC_FLG_USE_ALPHA_WEIGHT` el codificador
ponderaba el error de color por el alfa y los texels de alfa ≈ 0 salían negros en bloques de 6×6: el suelo de
Orgrimmar (`MM_OGRMR_GROUND_01`, 28.783 texels con alfa < 8) tenía 13.200 texels negros horneados y ninguno en el
original. Se codifica sin esa ponderación (versión 2 del codificador; invalida la caché).

## DBC

- Cabecera `WDBC`: `recordCount`, `fieldCount`, `recordSize`, `stringBlockSize`; registros de tamaño fijo +
  bloque de strings. Las strings localizadas tienen 8 columnas de idioma (enUS, koKR, frFR, deDE, zhCN, zhTW, esES,
  esMX: `core/Locale`) + flags, y cada cliente rellena solo la suya. **El runtime lee siempre la primera**: con un
  cliente de otro idioma el baker copia ahí la suya (fase 21, ADR-0004; `bake::localizeDbc`).
- Layouts para 5875: los que usa el servidor están en vMaNGOS (`DBCfmt`); los solo-cliente (`CharSections`,
  `CreatureModelData`, `Light*`, `SpellVisual*`, `ZoneMusic`…) en wowdev.wiki (sección 1.12).

## WDT / ADT (terreno)

- Mapa = 64×64 tiles; tile = 533,33 yardas; 16×16 chunks por tile (33,33 yardas).
- WDT: `MPHD`, `MAIN` (qué tiles existen), `MWMO`/`MODF` en mapas que son solo un WMO (mazmorras).
- ADT vanilla: `MVER`, `MHDR`, `MCIN`, `MTEX`, `MMDX`/`MMID`, `MWMO`/`MWID`, `MDDF` (doodads), `MODF` (WMOs), 256×`MCNK`.
- `MCNK`: `MCVT` (145 alturas: 9×9 + 8×8 intercaladas), `MCNR` (normales), `MCLY` (≤ 4 capas), `MCRF`,
  `MCSH` (sombra 64×64 de 1 bit), `MCAL` (alphas de 4 bits 64×64 en vanilla), **`MCLQ`** (líquidos; `MH2O` no
  existe en vanilla), `MCSE` (emisores de sonido). Agujeros en la cabecera del MCNK (máscara 4×4).

**Verificado en el cliente 1.12.1 enUS (`vd_baker scan-terrain`):** 43 WDT, 2.429 tiles en 21 mapas con terreno,
0 fallos. Parser en `tools/formats/src/formats/Adt.{h,cpp}`.
- **Ejes** (X = norte, Y = oeste, Z = arriba): el fichero `Mapa_<tx>_<ty>.adt` tiene su esquina noroeste en
  X = (32 − ty)·533,33, Y = (32 − tx)·533,33. Chunk `indexX` y columna de vértice avanzan hacia −Y; `indexY` y fila
  hacia −X. `MCNK.position` = (X, Y, Z) de la esquina noroeste del chunk (redondeada a ~0,001). Comprobado:
  normales `MCNR` (X, Y, Z en snorm8) frente a pendientes de `MCVT`: 0,997 de acuerdo; transpuesto, 0,81.
- WDT `MAIN`: entrada `ty·64 + tx`. Azeroth tiene 687 tiles en el WDT pero 736 ADT en los MPQ (sobran huérfanos).
- Offsets de subchunks relativos al inicio del `MCNK` (su cabecera de 8 B incluida) y apuntan a la cabecera del
  subchunk. `MCNR` declara 435 B pero ocupa 448 (13 de relleno). `MCLQ` declara tamaño 0: el tamaño real viene de
  `MCNK.sizeLiquid` = 8 + 804·n, una instancia por bit de líquido (0x4 río, 0x8 océano, 0x10 magma, 0x20 limo).
- `MCAL`: 2048 B por capa (4 bits, nibble bajo primero), filas a lo largo de −X como los vértices (comprobado por
  continuidad entre chunks vecinos: 19,8 de desajuste frente a 77 transpuesto). Última fila/columna = copia de la
  anterior (corrección del cliente; igual en `MCSH`, 1 bit por texel, LSB primero).
- Censo: capas por chunk 0:50.719 (fondo marino sin textura), 1:252.144, 2:77.078, 3:106.196, 4:135.687. Agujeros en
  1.389 chunks de 353 tiles. Líquidos: océano 224.984, río 12.411, magma 128, limo 0. Alturas −550…1.971.

**Fase 12, los 43 mapas (`vd_baker scan-maps`):** 22 con terreno (ADT), 20 de un solo WMO y 1 vacío (`ScottTest`); todos
tienen WDL. Los de un solo WMO: Blackfathom, BlackRockSpire, BlackrockDepths, DeeprunTram, DireMaul,
GnomeragonInstance, Mauradon, MoltenCore, Monastery (sin usar), OnyxiaLairInstance, OrgrimmarInstance (Sima Ígnea),
StormwindJail, StormwindPrison (sin usar), SunkenTemple, Uldaman, WailingCaverns, los cuarteles JcJ (Alliance/Horde
PVPBarracks) y los de prueba (Collin, test). **Las Minas de la Muerte, el Monasterio Escarlata, Zul'Gurub, Stratholme,
Scholomance, Zul'Farrak y los campos de batalla tienen terreno** (en varios, plano y bajo los edificios).
- `MODF` del WDT: **siempre en (0, 0, 0)** y con los extremos en los ejes del propio WMO. El extractor de vmaps de
  vMaNGOS (`wmo.cpp`: posición x = z = 0 → 533,33·32) coloca entonces el origen del edificio en el centro del mapa, de
  modo que **las coordenadas del servidor son las del propio WMO**. `formats::parseWdt` aplica la misma regla (posición
  y extremos desplazados al centro), así que el baker de mundo y el de colisión lo leen igual que un `MODF` de ADT.
  Dire Maul es el único con rotación (0, 180, 0).

**Pack de mundo de un mapa sin ADT** (`world_<mapa>.vdpk`, `pack/TerrainFormat.h`): mapa sin tiles, un array de
texturas de una sola capa (`Tileset\Generic\Grey.blp`, solo para tener qué enlazar), el horizonte WDL y el asset
`world/maps/<mapa>/objects` (`AssetType::TerrainObjects`): una `TerrainTileHeader` con solo su sección de WMO, que
`Terrain` entrega al `TerrainListener` como la tile `kMapObjectsTile` (64·64) mientras el mapa está abierto. Pesan
0,6 MB. Los frames de líquidos (120 capas, 4,8 MB) están desde la fase 12 en **`liquids.vdpk`**, uno para todos los
mapas; un pack de mundo antiguo que los lleve dentro los usa primero.

| Conjunto (fase 12) | Mundo | Colisión |
|---|---|---|
| Azeroth + Kalimdor | 1.420 MB | 381 MB |
| Los otros 40 mapas (33 s y 17 s de horneado) | 351 MB | 134 MB |
| `liquids.vdpk` | 4,8 MB | — |
- Texturas de terreno: 346 distintas (máx. 198 por mapa, 27 por tile); 332 son 256×256, 12 de 128×128 y 2 diminutas
  → el baker las reescala todas a 256² para el texture array del mapa.

**Líquidos (MCLQ, verificado):** alturas absolutas; rejilla 9×9 con la misma orientación que MCVT (comprobado
cruzando la profundidad por vértice con la altura del terreno); los vértices sin usar valen FLT_MAX; el byte de
profundidad ≈ 9 por yarda; baldosas 8×8 con nibble bajo 0xF = no se dibuja. Texturas animadas en
`XTextures\river\lake_a.N`, `ocean\ocean_h.N`, `lava\lava.N`, `slime\slime.N` (N = 1..30); las de agua tienen RGB
negro y el patrón en el alfa (brillos): el color sale de `Light.dbc`.

**WDL** (`<mapa>.wdl`): `MAOF` con 64×64 offsets absolutos a chunks `MARE` de 545 alturas i16 (17×17 en las esquinas
de chunk + 16×16 en los centros), misma orientación que el ADT (fila 0 = borde norte). Sin `MAHO` en vanilla.

## Iluminación (Light*.dbc y las tablas DayNight de WoW.exe)

- `Light.dbc` (12 campos): ID, mapa, X, Y, Z, radio interior, radio exterior, LightParams[5] (despejado, bajo el
  agua, tormenta, tormenta bajo el agua, muerte). Posición y radios **×36** y en ejes de colocación
  (X' = 17066,67 − Y, altura, Z' = 17066,67 − X). La luz con posición y radio 0 es la global del mapa. Las cinco
  columnas están siempre rellenas (Azeroth: 12, 13, 10, 11, 4). Fase 11: `LightConditions` elige el conjunto (bajo
  el agua la cámara, tormenta mezclada por la intensidad del clima, muerte en vez de todo lo demás).
- `LightParams.dbc` (9): ID, highlightSky, skybox (`LightSkybox.dbc`), ? (siempre 0), glow (peso del brillo a
  pantalla completa, 0,5 por defecto), alfa agua somera/profunda, alfa océano somero/profundo. Solo los conjuntos de
  muerte 1–5 tienen skybox (3 = `Environments\Stars\DeathClouds`).
- `LightSkybox.dbc` (2): ID, modelo (`.mdx` → `.m2`). Seis filas; `PortalWorldLegionSky` no está en los MPQ.
- `LightIntBand.dbc` (34): ID, nº de claves, 16 tiempos (medios minutos, 2880 = 24 h), 16 colores `0x00RRGGBB`;
  18 bandas por LightParams (`id = params·18 − 17 + i`): 0 difusa, 1 ambiente, 2–6 cielo (cénit → horizonte),
  7 niebla, 8 sombra, 9 sol, 10 halo del sol (y luz de las nubes del lado del sol), 11 emisivo de las nubes,
  12/13 ambiente de las dos capas de nubes, 14/15 océano cerca/lejos, 16/17 río cerca/lejos.
- `LightFloatBand.dbc` (34): igual con floats; 6 por params (`id = params·6 − 5 + i`): 0 distancia de niebla (×36),
  1 multiplicador (inicio = fin · mult.; **negativo** en 241 conjuntos, 124 de ellos bajo el agua: la niebla empieza
  detrás de la cámara y tiñe lo que está pegado a ella), 2 brillo celeste (0 o 1: oculta sol, lunas y resplandor),
  3 densidad de nubes.
- **El 1.12 no lee `World\dnc.db`** (WoW.exe no lo nombra): el ciclo día/noche son tablas propias del ejecutable
  (fase 11, `world/Celestial`, direcciones comprobadas con el desensamblado): la luz viene siempre del **noroeste**,
  a 37° de altura a mediodía y medianoche y 20° a las 06:00 y 18:00 (la luna alumbra desde el mismo sitio); el sol
  sube por el noroeste (ángulo polar 100° a las 05:30, 5° de 11:55 a 12:05, 100° a las 21:30); la Dama Blanca
  (`moon.blp`) a 55° a medianoche, de ~22:20 a ~03:20; el Niño Azul (`moon02.blp`) con la misma curva en un ciclo
  de 1,7 días de juego (días desde el 2000-01-01 según la fecha del servidor); estrellas (`Stars.m2`) llenas de
  00:00 a 03:00 y fuera de 04:30 a 22:30; resplandores aditivos sin prueba de profundidad. La cúpula es una esfera
  bajada cos 45° con anillos a 0, 30,6, 36, 41,4, 43,2, 45 y 180° del cénit (bandas de cielo a 90, 16,8, 9,8, 3,7 y
  1,8° de altura vistas desde la cámara). Las nubes las genera el cliente con ruido (cobertura de Hugo Elias:
  umbral 255·(1 − densidad), nitidez 0,96).
- **Modelos del cielo** (`pack/SkyFormat.h`, `AssetType::SkyModel` en `models.vdpk` bajo `sky/<ruta del m2>`):
  `Stars.m2`, los de `LightSkybox.dbc` y los skyboxes de los WMO (MOSB, su hash en `ModelHeader::reserved2`):
  vértices con posición, UV y la opacidad estática de su batch, partes por textura y mezcla, y la rotación del
  hueso raíz con su bucle.

## WMO (v17)

- Raíz: `MOHD`, `MOTX`, `MOMT` (materiales), `MOGN`/`MOGI` (grupos), `MOPV`/`MOPT`/`MOPR` (portales),
  `MOLT` (luces), `MODS`/`MODN`/`MODD` (doodad sets), `MFOG`.
- Grupos (`_NNN.wmo`): `MOGP` con `MOPY` (flags de triángulo, incl. colisión), `MOVI`, `MOVT`, `MONR`, `MOTV`,
  `MOBA` (batches), `MOCV` (colores de vértice, iluminación de interiores), `MOBN`/`MOBR` (BSP de colisión), `MLIQ`.
- Tamaños fijos (v17): `MOHD` 64 B, `MOMT` 64 B, `MOGI` 32 B, `MOPT` 20 B, `MOPR` 8 B, `MODS` 32 B, `MODD` 40 B,
  cabecera de `MOGP` 68 B (flags, AABB, portales, nº de batches trans/int/ext, niebla, `liquid`, id de
  `WMOAreaTable`), `MOBA` 24 B. Hay 816 WMO en 1.12.1; todos se parsean y hornean sin errores.
- Baker: los grupos se ordenan en Morton por su centro y las partes por (mezcla, material, iluminación, grupo);
  los WMO de más de 65.536 vértices usan índices de 32 bits. El runtime fusiona el mismo material de grupos
  vecinos visibles en un único draw (Ventormenta desde la puerta: 2.656 → 325 draws).

Comprobado con los datos reales:

- **El flag `0x2000` no significa "solo visible a través de portales"**, sino "iluminado por los colores de
  vértice": Ventormenta tiene 305 de sus 306 grupos con `0x2000` y solo uno con `0x8` (exterior). vMaNGOS
  (`IsOutdoorWMO`) decide interior/exterior con `WMOAreaTable`/`AreaTable` además de `0x8`. Por eso no sirve
  para acortar la distancia de dibujo de un grupo (probado: desaparecía el castillo de Ventormenta). Lo que
  distingue las calles de los interiores de verdad es `0x40` ("iluminado como el exterior"): calles y castillo
  llevan `0x2000|0x40`; las casas por dentro, solo `0x2000`, y esas sí se ven únicamente a través de sus portales.
- Ficheros truncados: `Undercity_144.wmo` acaba un byte antes (último color de `MOCV`); el lector recorta el
  último chunk y se repite el color que falta.
- `MODD`: rotación como cuaternión (x, y, z, w); el offset del nombre en `MODN` son los 24 bits bajos (los 8
  altos son flags); los nombres acaban en `.MDL`/`.MDX` → `.m2`.
- `MLIQ`: cabecera empaquetada de 30 B (vértices X/Y, tiles X/Y, esquina, material), vértices de 8 B (4 B de
  flujo o UV + altura f32) y 1 B de flags por tile (`& 0xF == 0xF` → tile sin líquido). La rejilla está alineada a
  tiles de 4,1667 yd y su esquina puede quedar hasta un tile fuera del AABB del grupo; los vértices que solo tocan
  tiles vacíos tienen altura 0.
- Tipo de líquido: en 1.12 el campo `liquid` de `MOGP` vale 15 en todos los grupos vistos, así que el tipo sale
  del primer tile con líquido: `(tile & 0xF) & 3` → 0 agua (océano si el grupo tiene `0x80000`), 1 océano,
  2 magma, 3 limo. Es la regla del extractor de vmaps de vMaNGOS (`contrib/vmap_extractor/vmapextract/wmo.cpp`).
  Canales de Ventormenta: agua; Forjaz: magma y dos grupos de agua.
- Colocación (`MODF`/`MDDF`) → mundo, verificada contra las extensiones de `MODF`: en ejes de colocación
  T(pos)·Ry(ry − 90°)·Rz(−rx)·Rx(rz)·S; modelo → colocación (x, y, z) → (x, z, −y); colocación → mundo
  (x', y', z') → (h − z', h − x', y') con h = 17.066,67. Con ry + 90° no coincide ninguna extensión.

### Luz de los WMO y de los M2 (fase 11, `models.vdpk` v6)

- **MOLT** (luces del WMO: tipo, color, intensidad, atenuación), **MFOG** (nieblas: posición, radios interior y
  exterior, fin, color y su variante bajo el agua), los índices de niebla de cada MOGP y **MOLR** (luces de cada
  grupo) se hornean en una sección propia (`ModelHeader::lightingOffset` → `ModelLightingHeader`): luces, claves de
  intensidad de las luces de M2, referencias, nieblas, la luz de cada grupo, una rejilla de luz para los interiores de
  verdad (celdas ≥ 3 yd, ≤ 24×24×12, sacada del MOCV de alrededor) y el grupo de cada doodad (MODR; gana el interior).
- **MODD color** es la luz horneada en el sitio del doodad: los doodads de un grupo interior se iluminan con ella
  más el ambiente de MOHD y la dirección de la lámpara MOLT más fuerte que les llega (o desde arriba).
- **Colores de vértice de los WMO** por clase de lote (los recuentos de MOGP): lotes interiores = MOCV ×
  (1 + 4·alfa) (`MapObjOverbright.bls`) + ambiente; exteriores y grupos sin MOCV = la luz de fuera (de día y de
  noche); lotes de transición = mezcla por el alfa del MOCV. Horneado: RGB = la luz a la mitad, A = parte de luz de
  fuera. MOMT `0x08` = `kModelMaterialExteriorLight` (caras interiores de ventanas: ambiente + directa sin orientación).
- **MOGP `0x80` / `0x8000`** (B1): `kModelGroupUnreachable` / `kModelGroupAlwaysDraw`. Un grupo inalcanzable que no es
  exterior ni "siempre dibujado" no lo dibuja el 1.12 (Ventormenta guarda así tres borradores de la catedral sobre la
  ciudad y uno del castillo), y sus doodads que solo él lista se hornean sin modelo.
- **MOSB**: el skybox del WMO, su hash en `ModelHeader::reserved2`.
- **Luces de los M2** (M2Light, 212 B): color e intensidad difusos animados (secuencia 0), atenuación de 0,5–1,7 yd:
  solo alumbran el propio modelo (los troncos y piedras de una hoguera).

### Terreno: especular y capas animadas (fase 11)

- Cada textura de terreno `X.blp` tiene a menudo una gemela `X_s.blp` con los mismos colores y la **máscara especular**
  en el alfa (427 de 1.048 en los tilesets): el baker usa la `_s` cuando existe (el resto, alfa 0) y marca el array
  con `kTextureSpecularMask`. "Terrain Highlights" (CVar `specular`): especular por vértice × alfa × sombra, como
  `terrain*_s.bls`. La sombra horneada oscurece toda la luz al 70 % (`terrain1.bls`: color · (sombra · 0,3 + 0,7)).
- **MCLY**: bits 0x07 = dirección en pasos de 45°, 0x08/0x10/0x20 = velocidad, 0x40 = animada (una repetición de
  textura cada 11,2/velocidad s, tabla de Noggit), 0x80 = sin luz (todas las capas de lava: brillan).

## M2 (versión 256, vanilla)

- Cabecera `MD20`, versión `0x100`. Las **vistas (skins/LOD) van embebidas** en el propio `.m2`
  (los `.skin` separados son de WotLK).
- Cabecera de 324 B con 22 `M2Array` (incluye `playableAnimationLookup` y `textureFlipbooks`, que desaparecen
  en versiones posteriores). Vista 44 B, submesh 32 B (su campo `level` da los bits altos de los índices en
  modelos de más de 65.535 vértices), texture unit 24 B, vértice 48 B, textura 16 B, material 4 B (flags, blend).
- Hay modelos solo de emisores (partículas) sin vértices: se hornean vacíos (3.805 M2 referenciados, 0 fallos).
- Bloques relevantes: vértices (pos, pesos/índices de hueso ×4, normal, UV), huesos, secuencias,
  texturas (incl. texturas "de tipo" que se sustituyen en runtime: piel, pelo, capa…), render flags/blend
  modes, texture units, attachments, partículas, ribbons, colisión (bounding triangles).

**Esqueleto y animación, verificado con los 9.717 M2 del cliente (`vd_baker scan-actors`, 0 fallos):**
- Hueso (`M2CompBone` anterior a 2.0) **108 B**: `keyBoneId i32`, `flags u32` (`0x8` billboard esférico),
  `parent i16`, `submesh u16`, pistas de traslación, rotación y escala (28 B cada una) y `pivot` 3×f32. Sin CRC
  del nombre. **Los padres siempre van antes que sus hijos.** Máximo 244 huesos (`UI_Tauren`); los modelos de
  actor, 178.
- Pista vanilla (28 B): `interpolation u16`, `globalSequence i16`, `ranges` (pares u32 **inclusivos** [primera,
  última] clave por secuencia, a veces con 1–3 entradas de más), `timestamps u32`, `values`. Todas las
  secuencias comparten **una línea de tiempo** (cada una ocupa su `[start, end]` en ms) y el rango solo acota la
  búsqueda: puede incluir claves fuera de la secuencia (`HumanMale`, hueso 0: claves en 3.333 y 550.000 ms para la
  secuencia 0, `[3.333, 6.000]`). Fuera de las claves se mantiene el valor. Solo aparecen interpolación **0 (paso) y
  1 (lineal)**; ni hermite ni bezier. 556 pistas tienen menos rangos que secuencias: usan todas sus claves.
- **Rotaciones en cuaterniones float** (`C4Quaternion`, 16 B; los comprimidos `M2CompQuat` son de 2.0 en adelante).
- Secuencia **68 B**: `id u16` (AnimationData), `variation u16`, `start u32`, `end u32`, `moveSpeed f32`,
  `flags u32`, `frequency i16` (peso entre variaciones), `pad u16`, `replay` (min, max u32), `blendTime u32`,
  caja 6×f32 + radio, `nextVariation i16`, `aliasNext u16`.
- Vértices: `boneIndices` indexan **directamente** los huesos del modelo (no la tabla `boneLookup`, que es para
  las paletas por submesh); pesos u8 que suman 255.
- Attachment **48 B**: `id u32`, `bone u16`, `u16`, posición 3×f32 y una pista `animate` (28 B). Ids vistos: 0
  escudo (y silla de las monturas), 1/2 manos derecha/izquierda, 5/6 hombros derecho/izquierdo, 11 casco,
  18 nombre (sobre la cabeza), 26/27/28 armas envainadas en la espalda (principal, secundaria, escudo), 32/33
  caderas. El modelo mira a +X, su izquierda es +Y.
- Color (`M2Color` 56 B: pista RGB + pista alfa fixed16) y transparencia (pista fixed16 por `transparencyLookup`)
  **animados**: 1.543 batches empiezan con opacidad 0 (brillos de elementales, limos, fantasmas); se hornean las
  pistas y se muestrean por actor, no se fija el primer valor.
- En los **doodads estáticos** (`models.vdpk`) el color y la opacidad del lote se fijan en su primera clave (la más
  alta si empieza en 0) y van en `ModelMaterial::color` con `kModelMaterialTinted`; un lote opaco que transparenta
  pasa a mezcla alfa. Son 360 lotes de 205 modelos (haces de luz, runas, frascos, lava; el pentagrama de Orgrimmar
  late entre 0,36 y 0,5). Antes se dibujaban blancos y opacos: el haz del farol de la tienda del Ticket Master de la
  Feria de la Luna Negra (5 % de un blanco amarillento, aditivo) era una pared blanca.
- Tipos de textura reemplazable: 1 piel del cuerpo (compuesta), 2 capa / piel de objeto, 6 pelo, 8 pelaje extra
  (tauren), 11–13 variaciones de criatura (`CreatureDisplayInfo`). Aparecen también 3, 9, 10 y 14 (muy pocos).

- **Efectos** (fase 9): emisores de partículas (504 B), ribbons (220 B) y transformaciones de textura (84 B) del
  M2 v256, con sus pistas animadas; un byte en 0x2C dice cabeza/cola/ambas de la animación de celdas; en las
  esferas el área es (radio mínimo, radio máximo). Censo de los 9.717 M2 (`vd_baker scan-particles`): 7.863 emisores
  en 1.854 modelos (336 sin malla), 590 ribbons en 176, 315 transformaciones de textura en 121; mezclas 1:26, 2:1204,
  4:6632, 5:1; formas plano 5908, esfera 1926, spline 29; ningún flipbook. Se hornean en `pack/ParticleFormat.h`
  para los dos packs: `models.vdpk` **v5** (cabecera de 192 B con su tabla de pistas en bucle sobre la secuencia 0 y
  la pose del hueso de cada emisor de doodad) y `actors.vdpk` **v3** (cabecera de 160 B con `effectsOffset`; las
  pistas de los efectos en la tabla del actor); los materiales llevan `uvAnimation`. Los packs viejos se rechazan.
  78 emisores que emiten modelos o apuntan a otro modelo no se hornean.

## Actores horneados (`actors.vdpk`, `engine/src/pack/ActorFormat.h`)

- 3.814 modelos desde la fase 9 (3.097 antes): los de hechizo (`Spells\*.m2` y los de `SpellVisualEffectName.dbc`,
  campo 2) además de las 18 razas, los de `CreatureModelData` (3 de sus rutas no existen en el cliente) y
  los 2.690 de `Item\ObjectComponents` (armas, escudos, cascos por raza y sexo, hombreras). 113 MB (LZ4), 3 s.
- Vértice de 32 B (posición f32, normal snorm8, UV f32, 4 pesos u8, 4 huesos u8); partes por (material, geoset)
  ordenadas por clase de mezcla y material; tabla ordenada de geosets.
- **Claves agrupadas por secuencia (v2)**: muestrear una pose recorre todas las pistas de una secuencia, así que el
  baker convierte cada pista en claves locales a cada secuencia (valor en su inicio, las claves de dentro y el
  valor en su final, muestreados con la regla del cliente) y las coloca secuencia a secuencia con su tabla de
  rangos. Medido en el Mi 11X: 51 → ~30 µs por esqueleto (la v1 saltaba por un blob de 2–3 MB: fallos de caché y
  de TLB). Pistas constantes, de identidad (se omiten) y de secuencia global aparte. Cuaterniones en snorm16.
- **Tamaño del círculo de selección** (`ActorModelHeader::selectionHalfSize`, antes `reserved`; sin cambio de
  versión: en los packs anteriores vale 0 y el runtime usa la caja de la pose de reposo). Regla de WoW.exe (CGUnit_C
  0x60AEE0, dibujado en 0x608E09): el círculo es un cuadrado de semilado √(0,5 · diagonal horizontal de la caja) ×
  `OBJECT_FIELD_SCALE_X` alrededor de los pies; la caja es la de la animación Stand (primera secuencia de la
  animación 0) o, si no la hay, la de la cabecera del M2 (1,2 si no tiene anchura); con montura, la de la montura.
  **No usa `UNIT_FIELD_BOUNDINGRADIUS`**: vMaNGOS lo escala por `display_scale / escala del DBC` y salían círculos
  enormes (lobo joven de Villanorte: 1,84 yd de radio de contorno → anillo de 3,7 yd; con la regla, 0,9 yd).

## Colisión horneada (`collision_<mapa>.vdpk`, `engine/src/pack/CollisionFormat.h`)

- Un asset por tile del mapa con algo con lo que chocar (`World\Maps\<mapa>\<mapa>_<tx>_<ty>.col`, LZ4): Azeroth
  687 tiles (7,4 M triángulos, 176 MB), Kalimdor 1.018 (7,8 M, 206 MB); los dos continentes en 30 s.
- **Qué choca** (la regla del extractor de vmaps de vMaNGOS, `contrib/vmap_extractor/vmapextract/wmo.cpp`): de los
  WMO, los triángulos con `MOPY & 0x08` (colisión) o con `0x20` (se dibuja) sin `0x04` (detalle); los que llevan
  `0x02` se marcan `kCollisionNoCamera` (la cámara los atraviesa). De los M2, sus *bounding triangles*
  (`collisionIndices/Positions` de la cabecera v256; hierba y flores no tienen: se atraviesan). Entran los doodads
  del terreno (MDDF) y los de los WMO (set 0 + el de la colocación, como se dibujan).
- **Reparto por tiles**: cada instancia se transforma a coordenadas de mundo y cada triángulo va a todo tile cuyo
  cuadrado toca su caja XY; así una consulta solo mira los tiles que toca su propia caja. Vértices soldados (bits
  idénticos), triángulos degenerados fuera. Un edificio que cruza tiles aparece en cada uno con su parte.
- **Terreno**: no se guardan triángulos; alturas en u16 (pasos de 1/128 yd) sobre la base de cada chunk (su punto
  más bajo, f32) y la máscara de agujeros de cada chunk. Las consultas reconstruyen los 4 triángulos del abanico
  de cada quad que miran (los mismos del render), saltándose los agujeros (entradas de Forjaz, Entrañas).
- **BVH**: SAH con 16 cubetas sobre los centros, hojas de hasta 8 triángulos, nodos de 32 B (caja + primer
  triángulo / segundo hijo); el primer hijo sigue a su padre. Profundidad máxima medida 27 (Ventormenta, 204k
  triángulos en el tile 30_48). Los líquidos (MCLQ del terreno — un chunk lleno y plano son 2 triángulos — y MLIQ de
  los WMO, por su tipo) van en un segundo BVH igual.
- El hilo `vd-collision` valida cada tile al cargarlo (límites de cada sección, índices y nodos): el hilo principal
  lo usa en sitio sin comprobar nada más.
- `vd_baker check-collision` compara el suelo que encuentra el runtime bajo una lista de puntos con su altura. Con
  las 53.744 criaturas de los dos continentes de la base de datos de vMaNGOS: ≤ 0,25 yd el 92,8 % en Azeroth y el
  90,5 % en Kalimdor (≤ 1 yd, el 98,7 % y 98,2 %: el resto vuelan, nadan o tienen el `z` mal); en las ciudades,
  96,4–99,4 % a ≤ 0,25 yd.

## Interfaz táctil (`ui.vdpk`, `engine/src/pack/UiFormat.h`)

`vd_baker bake --only ui` (3 s, 8,9 MB). Decisión y motivos: [ADR-0002](decisions/ADR-0002-ui.md).

- **`ui/atlas`**: array 2D de páginas de 2048² en ASTC 4×4 sin mips con las texturas de `Interface\` que usa la UI
  (lista en `tools/baker/UiCommands.cpp`; fase 7: 180 texturas en una página; fase 8: 344 texturas en 2 páginas, 67 %
  llenas, con las de PaperDoll, Durability, TalentFrame, ComboFrame, CharacterCreate, PartyFrame, FriendsFrame…). Cada textura entera, en una
  posición múltiplo de 4 y con 4 px de borde repetido alrededor: un bloque ASTC nunca mezcla dos texturas y el
  filtrado bilineal no las contamina. **`ui/sprites`**: dónde quedó cada una (hash de su ruta del cliente, página,
  x, y, tamaño original); el runtime dibuja partes de ellas con las coordenadas de textura de FrameXML.
- **`ui/fonts`**: array R8 de 1024² con los campos de distancia (SDF, stb_truetype, 6 px de rango a cada lado del
  contorno) de las 4 fuentes del cliente a 40 px; **`ui/font/<frizqt|arialn|morpheus|skurri>`**: métricas
  (ascenso, descenso, avance y desplazamiento de cada glifo, *kerning* de todos los pares). Juego de caracteres:
  ASCII, Latin-1, Latin extendido A y puntuación tipográfica (FRIZQT solo trae 210 de esos 329: los que faltan se
  ven como «?», como en el cliente enUS).
- **`ui/minimap/<mapa>`**: la textura de minimapa de cada tile ADT del mapa, de `textures\Minimap\md5translate.trs`
  (`<mapa>\map<x>_<y>.blp` → `<md5>.blp`; 25 mapas, 2.356 tiles). Las texturas ya están en `textures.vdpk`; el
  runtime las carga bajo demanda. Las de interiores de WMO se ignoran de momento.
- Los iconos (`Interface\Icons\*.blp`, 64×64 ASTC 4×4) no van al atlas: el runtime los sube por capas a un array de
  256 iconos en la GPU (LRU) según los necesita.
- **`ui/strings/<idioma>`** (fase 8, ADR-0003): los textos de la interfaz de un idioma. Cabecera + `UiString[]`
  (hash FNV-1a de la clave, desplazamiento, longitud) ordenados por hash + los textos UTF-8 terminados en NUL. Se
  hornean del cliente (`GlueStrings.lua` y luego `GlobalStrings.lua`, que gana en las dos claves que comparten con
  texto distinto: 5.857 claves) más `locale/enUS/*.lang`; cada otro idioma (`locale/esES/*.lang`) se aplica encima
  del inglés. ~0,5 MB por idioma. Fase 21 (ADR-0004): un idioma con cliente propio (`--locale-clients`) parte de los
  textos de ese cliente, con nuestras claves en inglés debajo y su `locale/<idioma>` solo para lo que el cliente no
  define. Claves derivadas de cada cliente: `VD_GLUE_SHOW_ESRB_RATING`, `VD_CREATE_SHOW_RANDOM_NAME`
  (`GlueLocalization.lua`), `VD_MINIMAP_TWENTYFOUR_HOUR_TIME` (`TwentyFourHourTime`) y los `EMOTEn_TOKEN` de
  `ChatFrame.lua`.

## Packs de idioma (fase 21, ADR-0004)

- `db.<idioma>.vdpk`, `textures.<idioma>.vdpk`, `sound.<idioma>.vdpk` junto a su pack base (`vd_baker bake --only
  locale`): el mismo formato, **solo lo que cambia un cliente de ese idioma, bajo las mismas rutas**: las tablas DBC
  con textos (texto movido a la primera columna; las demás columnas como en ese cliente), las texturas cuyos píxeles
  cambian (los mapas del mundo con nombres pintados: se comparan decodificadas, no por bytes) y los sonidos que
  `SoundEntries.dbc` nombra y se grabaron de nuevo (otra longitud). `PackHeader::locale` (4 bytes, antes reservados)
  dice el idioma de los datos; un pack de idioma sin él se rechaza.
- `pack::PackReader::openOverlay(idioma)`: `find()` mira primero en el de idioma, `findBase()` solo en el base;
  `entry(i)` recorre las entradas del base, con el recurso del de idioma donde lo hay.
- Comprobar qué cambia un cliente nuevo: `vd_baker scan-locale --diff 1` (por extensión: iguales, mismo tamaño,
  otro tamaño, solo en él; y la FrameXML que cambia).
- El arte grande (mapas del mundo y de vuelo `Interface\WorldMap\<zona>\<zona>1..12.blp` y `TaxiFrame\TAXIMAP0/1`,
  pantallas de carga `Interface\Glues\LoadingScreens\*`) tampoco: `UiResources::texture()` lo sube desde
  `textures.vdpk` a texturas sueltas en streaming (como mucho 96 vivas; se liberan a los 90 frames sin uso).

## DBC de apariencia (5875, comprobados con los datos)

| Tabla | Campos usados |
|---|---|
| `CharSections` | 0 id, 1 raza, 2 sexo, 3 sección (0 piel, 1 cara, 2 vello facial, 3 pelo, 4 ropa interior), 4 variación, 5 color, 6–8 texturas, 9 flags |
| `CharHairGeosets` | 1 raza, 2 sexo, 3 peinado, 4 geoset (0 = calvo), 5 mostrar cuero cabelludo |
| `CharacterFacialHairStyles` | 0 raza, 1 sexo, 2 estilo, **6/7/8 geosets 1xx/3xx/2xx** (3–5 son basura sin inicializar) |
| `ChrRaces` | 4/5 display masculino/femenino, 6 prefijo (`Hu`, `Or`… para los cascos `_HuM`) |
| `CreatureDisplayInfo` | 1 modelo (`CreatureModelData`), 3 extra (`CreatureDisplayInfoExtra`), 4 escala, 6–8 texturas |
| `CreatureDisplayInfoExtra` | 1 raza, 2 sexo, 3 piel, 4 cara, 5 peinado, 6 color de pelo, 7 vello, 8–17 equipo (casco, hombros, camisa, pecho, cintura, piernas, pies, muñecas, manos, tabardo), 18 `BakeName` (`Textures\BakedNpcTextures\`) |
| `CreatureModelData` | 2 ruta (`.mdx` → `.m2`), 4 escala |
| `ItemDisplayInfo` | 1–2 modelos (izq./der.), 3–4 sus texturas, 5 icono, 6–8 `geosetGroup`, 12–13 `helmetGeosetVis` (hombre, mujer), 14–21 texturas por región (brazo sup./inf., mano, torso sup./inf., pierna sup./inf., pie) |
| `HelmetGeosetVisData` | 1 pelo, 2 1xx, 3 2xx, 4 3xx, 5 orejas: **máscaras de bits por id de raza** que se ocultan |
| `AnimationData` | 0 id, 1 nombre, **6 animación de reserva** (Correr → Andar, EmoteWorkNoSheathe → EmoteWork) |
| `Emotes` | 2 animación de un estado de emote (`UNIT_NPC_EMOTESTATE`) |

- **Piel compuesta** (256×256): base de `CharSections` y regiones brazo sup. (0,0,128,64), brazo inf. (0,64,128,64),
  mano (0,128,128,32), cara sup. (0,160,128,32), cara inf. (0,192,128,64), torso sup. (128,0,128,64), torso inf.
  (128,64,128,32), pierna sup. (128,96,128,64), pierna inf. (128,160,128,64), pie (128,224,128,32). Orden: piel, cara,
  vello facial, cuero cabelludo, ropa interior, y el equipo (camisa, piernas, pies, pecho, muñecas, manos, tabardo,
  cintura). Las texturas de equipo están en `Item\TextureComponents\<Región>Texture\<nombre>_{M,F,U}.blp`.
- **Geosets**: grupo·100 + valor, con valor 1 por defecto en cada grupo (401 manos desnudas, 501 pies, 1301 piernas,
  1501…) y 702 orejas; el equipo lo cambia con `geosetGroup` (guantes 4xx, botas 5xx, mangas 8xx, pecho 10xx,
  pantalón 11xx, rodilleras 9xx, túnica 13xx, tabardo 12xx, capa 15xx).
- Los PNJ humanoides con `BakeName` traen la piel ya compuesta (ropa incluida): solo se les ponen geosets, pelo y los
  modelos de casco, hombreras y armas.

## DBC de jugabilidad (fase 7, 1.12.1)

| Tabla | Campos usados |
|---|---|
| `Spell` (173 campos) | 1 escuela, 2 categoría, 6 atributos, 13 objetivos, 18 tiempo de lanzamiento, 19/20 enfriamiento propio / de categoría, 30 duración, 31 tipo de poder, 32 coste, 36 alcance, 61–63 efectos, 82–84 / 85–87 objetivos implícitos A / B, 106–108 `EffectMiscValue`, 117 icono, 120 nombre, 129 rango, 156 % de maná, 157/158 categoría y tiempo del enfriamiento global |
| `SpellIcon`, `SpellCastTimes`, `SpellRange`, `SpellDuration` | 1 ruta del icono; 1 ms; 1/2 alcance mín./máx.; 1 ms |
| `Lock` | 1–8 tipo de llave (2 = habilidad), 9–16 índice (`LockType` cuando el tipo es 2) |
| `GameObjectDisplayInfo` | 1 modelo (`.mdx` → `.m2`; los `.wmo` son transportes) |

- Ojo: las columnas de la base de datos de vMaNGOS (`SpellEntry.h`) van **3 por delante** del `Spell.dbc` del cliente
  a partir de la 79 (añaden `EffectBonusCoefficient`): nombre 120 y no 123, icono 117 y no 120.
- `models.vdpk` incluye también los M2 de `GameObjectDisplayInfo.dbc` (cofres, cubos, buzones, carteles…): el runtime
  los coloca como instancias dinámicas donde el servidor tiene sus objetos (`app/ServerObjects`).

## Colisión de objetos móviles (`collision_objects.vdpk`, `engine/src/pack/ObjectCollision.h`)

`vd_baker bake --only objects` (2 s, 8,8 MB): la colisión de los modelos de `GameObjectDisplayInfo.dbc` (927 de 1.619;
660 sin triángulos de colisión, 32 que faltan en el cliente) con la misma regla que la colisión del mundo, en
coordenadas del modelo. El runtime la coloca y la mueve con su objeto (`CollisionWorld::addObject` / `placeObject`,
triángulos marcados `kTriangleMoving`): barcos, zepelines y ascensores sobre los que el personaje se sube.

## Sonido horneado (`sound.vdpk`, `engine/src/pack/SoundFormat.h`)

`vd_baker bake --only sound` (~50 s; ~25 s con la caché `packs/.cache/sound` y `/ground`): todos los ficheros que nombra
`SoundEntries.dbc` (7.062; 11 nombrados no existen en el cliente) más el tema de la pantalla de acceso, con su ruta del
cliente (el runtime los busca por `DirectoryBase + "\" + File[i]`). Cada uno es un `SoundHeader` (32 B: códec,
canales, frecuencia, frames, bloque) más sus datos, sin comprimir: el mezclador los lee en sitio.

- **Efectos, voces y ambiente**: los WAV del cliente son todos PCM16 (1.135 MB) → **IMA ADPCM** 4:1 en el formato de
  bloques de WAV (1017 frames por bloque, codificador que elige el nibble óptimo; > 30 dB de SNR), 279 MB.
- **Música**: los MP3 tal cual sin etiquetas ID3 (228 MB), decodificados trama a trama al sonar (streaming).
- **`ground/<mapa>`** (LZ4): la superficie del suelo para los pasos, por chunk de terreno en 4×4 celdas de 8,3 yd: el
  `SoundID` de `TerrainType.dbc` (tierra, metal, piedra, nieve, madera, hierba, hojas, arena, barro) de la capa que más
  se ve, mezclando los alphas del ADT y tomando el efecto de suelo de la capa (`MCLY` → `GroundEffectTexture` →
  `TerrainType`) o, si no lo dice, el nombre de su textura. 2.429 tiles, ~1 MB.
- Tablas de sonido de `db.vdpk` (sin estructura en vMaNGOS: campos comprobados con filas reales en tests):
  `SoundEntries`, `ZoneMusic`, `ZoneIntroMusicTable`, `SoundAmbience`, `AreaTable`, `NPCSounds`,
  `CreatureSoundData`, `CreatureDisplayInfo`, `CreatureModelData`, `WeaponImpactSounds`, `WeaponSwingSounds2`,
  `Spell`, `SpellVisual`, `SpellVisualKit`, `VocalUISounds`, `EmotesTextSound`, `FootstepTerrainLookup` (el baker usa
  además `TerrainType` y `GroundEffectTexture` para los pasos).

## DBC de la fase 8 (1.12.1, comprobados con los datos en tests)

| Área | Tablas (índice propio de cada módulo, nunca en `GameData`) |
|---|---|
| Objetos (`game/ItemData`) | `DurabilityCosts`, `DurabilityQuality`, `BankBagSlotPrices`, `ItemSet`, `SpellItemEnchantment`, `ItemRandomProperties`, `SkillLine`, `Faction`, `Spell` (descripciones de los efectos de uso y equipo) |
| Combate (`game/AbilityData`) | `Spell` + `SpellDuration` / `SpellRadius` / `SpellRange` / `SpellCastTimes` / `SpellIcon` (descripciones con sus `$` resueltos), `SkillLine`, `SkillLineAbility`, `TalentTab`, `Talent`, `Faction` (reputación base por raza y clase), `SpellShapeshiftForm` |
| Social (`game/SocialData`, `game/CharCreate`) | `AreaTable`, `ChatChannels`, `EmotesText`, `EmotesTextData`; `CharBaseInfo` (40 parejas raza/clase: la de 1.12 trae 41, el enano mago se excluye como en vanilla), `CharSections`, `CharacterFacialHairStyles`, `ChrRaces`, `CharStartOutfit` |
| Viajes (`game/AreaData`, `TaxiData`, `WorldMapData`) | `AreaTable`, `AreaTrigger`, `Map`, `LoadingScreens`, `GameTips`, `WorldMapArea`, `WorldMapContinent`, `WorldMapOverlay`, `TaxiNodes` (85), `TaxiPath` (287), `TaxiPathNode`, `TransportAnimation`, `GameObjectDisplayInfo` |

- Las descripciones de hechizos usan tokens (`$s1`, `$d`, `$o1`, `$t1`, `$m1`, `$a1`, referencias como `$6136s2`,
  `$*N;`, `$/N;`, `$l…:…;`, `$g…:…;`) que el cliente resuelve con los campos del hechizo y el nivel del lanzador.
  Hoy hay dos implementaciones (`game/SpellText` del libro de hechizos y la de `game/ItemData` de los tooltips de
  objetos): pendiente de unificar.

## Pasada de paridad (1.12.1, comprobado con los datos en tests)

- **`actors.vdpk` v4**: cabecera con `portraitOffset` (antes `reserved[0]`) hacia un `ActorPortrait` de 48 B por
  modelo: la cámara de retrato del M2 (la de tipo 0 de la tabla `cameraLookup`) en el primer frame de la secuencia 0, o
  un encuadre del baker con los vértices de la cabeza en Stand (hueso clave Head y sus hijos; si no, el punto del casco
  o la parte alta de la caja). 440 M2 traen cámara de retrato; solo 7 de los 407 modelos de criaturas y razas usan el
  encuadre del baker. **Cámaras M2 v256**: 124 B cada una (tipo, fov, far, near, pista y base de la posición, pista y
  base del objetivo, roll; claves spline: valor y dos tangentes, se guarda el valor). El fov es el diagonal en
  radianes (tan(fov/2)/√2 por eje en un cuadrado); near 2/9 yd, far 1000/36 yd. Ahora también se hornean los efectos
  propios del cliente (`Particles\*.m2`: brillo de botín `LootFX`, polvo al correr, salpicaduras de sangre...): 3.820
  modelos.
- **`ui.vdpk`**: arte nuevo de la pasada (UI-LootPanel, TargetDead, UI-TOD-Indicator, flechas de grupo y cadáver del
  minimapa, botones de zoom, Nameplate-Border, cursores `Interface\Cursor\*`, UI-PVP-FFA, UI-ExhaustionTick*,
  UI-Quickslot...). El círculo de selección del 1.12 es `Textures\UnitSelectTexture.blp` (blanco, forma en el alfa;
  `Selection_SelectionA.blp` es un atlas de pruebas).
- **Spell.dbc**: campo 4 = Dispel (SpellDispelType.dbc, 12 campos: id, nombre localizado en el 1; los tipos 1–4 tienen
  color en la interfaz), campo 118 = activeIconID.
- **CreatureSoundData** tiene 30 columnas en 1.12 y **ninguna es de NPCSounds** (la fase 9 la leía mal): 23 = bucle
  (FireElementalLoop...), 24 = tipo de impacto, 25–26 = salto (vacías en 1.12), 27–29 = frases de los demonios del
  brujo. NPCSounds sale de CreatureDisplayInfo (campo 11; el 9 es sizeClass). Los esfuerzos de salto y aterrizaje del
  jugador (`Sound\Character\PlayerExertions\<Raza><Sexo>MainJump/MainLand`) no están enlazados en ninguna tabla ni
  los nombra WoW.exe: el 1.12 no los toca y nosotros tampoco (PROTOCOL §2.29). ChrRaces: columnas 13 = chapuzón,
  15 = exploración.
  ItemDisplayInfo 11 = grupo de sonidos del objeto; GameObjectDisplayInfo 2–11 = sonidos; SoundEntries bandera 0x200
  = bucle. 1.12 no tiene SoundEmitters.dbc: los emisores ambientales son eventos de los M2 (pendiente hornearlos).
- **CreatureType.dbc**: 11 campos (nombre localizado). **Lock.dbc** para los puntos de rastreo de recursos.

## Fase 13 (1.12.1, comprobado con los datos en tests)

- **`actors.vdpk`** (sin cambio de versión) incluye ahora los M2 de `GameObjectDisplayInfo` que tienen alguna de las
  animaciones de objeto 145–157 (Open, Opened, Close, Closed, Destroy, Destroyed, Rebuild, Custom0..3, Spawn, Despawn):
  331 modelos animados (puertas, cofres, el corcho de pesca, trampas); los 1.247 sin ellas siguen en `models.vdpk`. El
  pack pasa a 4.151 modelos y 118,6 MB. `app/ServerObjects` dibuja como actor un objeto cuyo modelo esté en él.
- Tablas nuevas que lee el cliente: `ServerMessages.dbc` (10 campos: id, texto ×8, flags; las cinco filas de la cuenta
  atrás del servidor), `SpellRadius.dbc`, `SpellItemEnchantment.dbc` (nombre y flags de cada encantamiento),
  `ChrClasses.dbc` (familia de hechizos de la clase), `SkillLineAbility.dbc` con sus rangos de dificultad (campo 10
  gris, 11 amarillo; 0 = gris − 25), `SkillLine`, `ItemSubClass`, `CreatureFamily`, `QuestSort`, `QuestInfo` y la
  columna `reputationListID` de `Faction.dbc`.
- `Interface\Minimap\POIIcons.blp`: rejilla de 8 × 8 iconos de 16 px; la celda 6 es la bandera roja de las
  indicaciones de los guardias (todas las filas de `points_of_interest` de vMaNGOS la usan).

## Fase 14 (1.12.1, comprobado con los datos en tests)

- **Eventos de los M2** (`M2Event` anterior a 2.0, **44 B**): `identifier u32` (4 caracteres, `$` en el byte bajo),
  `data u32`, `bone u32`, posición 3×f32 (espacio del modelo, pose de reposo) y una pista **solo de tiempos**
  (`M2TrackBase`: interpolación, secuencia global, rangos y tiempos; sin valores). Censo de los 9.717 M2
  (`vd_baker scan-events`, `VD_SCAN_VERBOSE=1` lista cada evento por secuencia): 1.466 modelos con eventos, ninguno
  sobre una secuencia global. **Los rangos por secuencia no sirven** (muchos eventos de un solo tiempo repiten el
  mismo rango en todas): el tiempo de cada evento se asigna a la secuencia cuya ventana `[start, end]` lo contiene.
  Ids que importan: pisadas `$FL0`/`$FR0` al andar (HumanMale: derecha a 0 ms, izquierda a 533 de 1.000) y
  `$RL0`/`$RR0` al correr (33 y 367 de 667), `$FL1..3`/`$RL1..3`/`$BL0`/`$BR0` para las otras patas de cuadrúpedos y
  monturas, `$FSD` en cada paso (304 modelos), `$BTH` aliento (cabeza), `$DTH` muerte, `$HIT`/`$CAH`/`$CPP`/`$CSS`/
  `$CST` combate y hechizos, `$CSD` sonido de emote (`data` = `SoundEntries`, p. ej. 6576 y 6919–6923 en
  HumanMale), `$DSL`/`$DSO`/`$DSE` sonidos de doodads (235/52/16 modelos, `data` = `SoundEntries`), `$GO0-4`/`$GC0-3`
  estados de objetos, `$SND` sonido de los modelos de hechizo, `$WNG`/`$WGG` alas, `$WTB`/`$WTT` estela de arma,
  `$SHK` sacudida de cámara (11 modelos, `data` 1–3).
- **`actors.vdpk` v5**: `ActorModelHeader::eventsOffset` (antes `reserved[0]`) apunta a un `ActorEventsHeader`
  (32 B) con los eventos (`ActorEvent` de 28 B: id, data, hueso, `kActorEventTimed`, posición, primer rango), una
  tabla de `ActorRange` (eventos × secuencias) y los tiempos locales a cada secuencia, ascendentes. Sin tiempos: el
  evento está siempre activo mientras se ve el modelo. +0,3 MB (118,9 MB).
- **Cámaras de las cinemáticas** (`Cameras\*.m2`, 16 modelos; `M2Camera` de 124 B): las pistas de posición y objetivo
  son **bezier** (interpolación 2) con sus puntos de control absolutos (valor, entrada, salida), la de giro paso o
  bezier en torno a 2π; `fov` 0,785 rad diagonal. Se hornean en **`cinematics.vdpk`** (nuevo, 23,5 KB,
  `pack/CinematicFormat.h`): cabecera de 96 B, claves de 40 B con sus dos puntos de control, tiempos desde el inicio
  de la secuencia 0 (la duración del vuelo: 87,5 s el humano, 70,2 s el orco).
- `CinematicSequences.dbc` (10 campos: id, sonido, 8 cámaras) y `CinematicCamera.dbc` (7: id, modelo `.mdx`, voz en
  off de `SoundEntries` — `Sound\CinematicVoices\*Narration.mp3`, tipo 31 —, origen x/y/z, orientación): la cámara
  vuela en el espacio del modelo y se coloca en el mundo con **origen + el punto girado por la orientación** sobre
  +Z (comprobado: las de humano y orco acaban 6,5 yd detrás del punto de inicio de su raza, mirando al norte).
  `ChrRaces.dbc` campo 16: la secuencia de cada raza (humano 81, orco 21, enano 41, elfo de la noche 61, no-muerto 2,
  tauren 141, gnomo 101, trol 121). Las secuencias 1 (`PalantirOfAzora`) y 161 (`Scry_cam`) son de objetos de
  cámara (tipo 13).
- **Doodads animados** (`vd_baker scan-doodad-anim`): de 3.727 M2 colocados (552.779 instancias), 383 se mueven de
  verdad en su bucle (154 con secuencias globales) y 156 tienen vértices billboard; flags de hueso `0x8` (esférico,
  120 modelos) y `0x40` (cilíndrico en Z, 67: cadenas, halos); `0x10`/`0x20` no aparecen. 88 modelos tienen Stand
  fuera de la secuencia 0 y 58 varias variaciones de Stand (el molino de Elwynn, `westfallwindmill.m2`: rotor 97 %,
  veleta 3 %). **`models.vdpk` v7** (323,4 MB): `ModelHeader` de 208 B (`skeletonOffset`, `fileRadius`), sección de
  esqueleto (`ModelSkeletonHeader` 32 B, `ModelSkinVertex` 8 B con pesos todo 0 = vértice billboard,
  `ModelLoopVariation`), `ModelLight::bone`; la tabla de pistas cubre las variaciones de Stand (efectos, UV y luces
  de los doodads hacen bucle sobre Stand, no sobre la secuencia 0).
- **Hierba** (`grass.vdpk`, 16 MB, `pack/GrassFormat.h`, `AssetType` 18 y 23; `vd_baker scan-grass`):
  `GroundEffectTexture.dbc` 7 campos (id, 4 doodads, densidad, `TerrainType`; sin pesos antes de 2.x) y
  `GroundEffectDoodad.dbc` 3 (id, **etiqueta**, nombre bajo `World\NoDXT\Detail\`): los doodads de una textura apuntan
  a la etiqueta, no al id. El `effectId` de MCLY se lee en 16 bits. La capa de cada una de las 8×8 celdas sale del
  mapa de baja calidad del MCNK (u16 por fila, 2 bits por columna); los bits "no effect doodad" (un byte por fila) y
  los agujeros las apagan. Colocación de WoW.exe (`CMapChunk::CreateDetailDoodads` 0x6BFC10, `CRndSeed`
  0x44EA80/0x4531E0): semilla `((ty*16+fila)<<16)|(tx*16+col)`, `frillDensity` (16 por defecto, sin opción en el
  1.12) celdas al azar con repetición, `densidad` doodads por celda (0 = 8), punto, ranura `(celda+doodad)&3`, escala
  0,9–1,1 y giro ±π; altura y normal del triángulo del chunk, sombreado 0xC0 con el bit de MCSH. Hasta 70 yd de
  profundidad (rampa de alfa desde 52,5), alfa 128 (`detailDoodadAlpha`), dos caras, sin viento. 87.076 chunks con
  hierba en Azeroth y 87.426 en Kalimdor.
- **Sonido del mundo** (`worldsound.vdpk` v2, 5,3 MB, `pack/WorldSoundFormat.h`, `AssetType` 19, 20, 24):
  `modelsounds/<mapa>` (los 14.817 doodads que suenan, en su punto del mundo, por celdas de 32 yd), `wmoareas/<mapa>`
  (1.365 colocaciones de los 443 WMO con fila en `WMOAreaTable`: id del WMO, conjunto de nombres y una rejilla de 2 yd
  con la altura de los suelos de cada grupo) y `emitters/<mapa>`. **MCSE** de 52 B: solo 36 en todo el cliente, de
  prueba. `WMOAreaTable` se busca por (WMO, conjunto de nombres, grupo) y cada columna cae a la fila del grupo −1.
  **`CreatureSoundData`**: las pausas (fidget) son las columnas 14–17 y los ataques especiales 18–21 (estaba mal
  desde la fase 9); el bucle de la criatura, la 23. Eventos de sonido (WoW.exe 0x5FFBE0 / 0x6951E0): `$FSD` paso,
  `$FD1-4` fidget 0–3, `$FDX` sonido de estar, `$AH0-3` ataques especiales, `$WNG`/`$WGG` alas, `$CSD` su
  `SoundEntries`, `$ESD` la columna 6 de `Emotes.dbc`; `$DSL` empieza o cambia el bucle del modelo, `$DSE` lo para,
  `$DSO` suena una vez. Los molinos de Elwynn y Páramos de Poniente no tienen eventos (no suenan en el 1.12).
- **Efectos del cuerpo** (`bodyfx.vdpk`, 261 KB, `pack/BodyFxFormat.h`, `AssetType` 22): la caja de cada animación
  (variación 0) de los 407 modelos de `CreatureModelData`, para la sombra.
- **Partículas que emiten modelos** (`pack/ParticleFormat.h`, sin cambio de versión: campos en bytes reservados):
  `ParticleEmitter::mesh`, `EffectsHeader` con `meshCount`, `meshesOffset`, `meshVerticesOffset`, `ParticleMesh`
  (48 B), `ParticleMeshVertex` (20 B), flag `kParticleMeshRandomSigns` 0x8000. 57 emisores de 15 modelos en
  `actors.vdpk`; dos doodads en `models.vdpk`.

## Fase 15 (1.12.1, comprobado con los datos en tests)

- **Minimapas de interiores** (`ui.vdpk` → `ui/wmominimaps`, `pack/UiFormat.h`): las líneas `WMO\<dir>\<nombre>_<grupo>_
  <x>_<y>.blp` de `textures\Minimap\md5translate.trs` (6.248; 6.041 con su edificio y su textura) son el minimapa de
  382 WMO (`World\wmo\<dir>\<nombre>.wmo`). Cada grupo se pinta a **2 píxeles por yarda** desde la esquina mínima de
  su caja (MOGI): la tesela (x, y) empieza 128 yd · (x, y) más allá a lo largo de las X e Y del edificio; las columnas
  de la textura van hacia +X y las filas hacia −Y (la fila de arriba es la Y alta); las últimas teselas y los grupos
  pequeños usan la potencia de dos siguiente, transparente fuera de la caja (comprobado pintando la geometría de
  grupos de la Masacre y de Ventormenta contra sus teselas). Tabla: `UiWmoMinimapHeader` + `UiWmoMinimap` (16 B,
  ordenado por el `assetPathHash` del WMO) + `UiWmoMinimapTile` (32 B: textura y caja del edificio). +0,2 MB.
- **Iconos de macro** (`ui/macroicons`, `AssetType::UiMacroIcons` 25): la lista de `GetNumMacroIcons` /
  `GetMacroIconInfo` de WoW.exe (0x4F0090): todos los ficheros de `Interface\Icons` de los MPQ, sin extensión,
  ordenados con `stricmp` y sin repeticiones: 2.684. `UiMacroIconsHeader` (16 B) + `UiMacroIcon` (16 B: textura,
  nombre) + los nombres.
- `WorldMapContinent.dbc` (13 campos): id, mapa, teselas ADT (izquierda 2, derecha 3, arriba 4, abajo 5), desplazamiento
  en el mapa World (6, 7, en unidades de 16 píxeles), escala (8: 0,75 los dos), mapa de vuelo (9–12). El mapa
  `Interface\WorldMap\World` no tiene fila en `WorldMapArea.dbc`.
- Mascotas: `CreatureFamily.dbc` (nombre, `petFoodMask`, icono), `PetLoyalty.dbc`, `ItemPetFood.dbc` (el texto de la
  dieta), `StableSlotPrices.dbc`, `PetPersonality.dbc` (felicidad → % de daño). Tabardos: `Textures\GuildEmblems\`
  (`Emblem_%02d_%02d_TU_U` / `_TL_U`, `Border_%02d_%02d_…`, `Background_%02d_…`: torso alto y bajo), 170 emblemas,
  17 colores, 6 bordes, 17 colores de borde, 51 fondos (WoW.exe); `ItemDisplayInfo` flags & 1 = tabardo de hermandad.
  Ayuda: `GMTicketCategory.dbc` y `GMSurvey*.dbc`. Personaje: `SkillRaceClassInfo`, `ChrClasses`, `Faction.dbc` (el
  cliente toma la **última** fila de reputación base que encaja; vMaNGOS la primera).
- **Arte nuevo del atlas de `ui.vdpk`**: 485 texturas en 2 páginas (77 %): PaperDollInfoFrame, `PvPRankBadges`, botones de
  girar modelos, el arte de hermandad, ayuda, opciones y macros. El arte grande (fondos de bolsas, del probador, de las
  ventanas de hermandad) va por `textures.vdpk` bajo demanda.

## Fase 18 (1.12.1, comprobado con los datos)

- **Escenas de las pantallas de acceso** (`cinematics.vdpk`, `AssetType::GlueScene` 26, `pack/GlueFormat.h`): los 13
  modelos de `Interface\Glues\Models\*.m2` (`UI_MainMenu` = el Portal Oscuro; `UI_Human`, `UI_Orc`, `UI_Dwarf`,
  `UI_NightElf`, `UI_Scourge`, `UI_Tauren`; los `UI_RS_*` no los usa el GlueXML) van también a `actors.vdpk` como
  actores (huesos, partículas, UV animadas). El blob de escena: `GlueSceneHeader` (48 B: luces, la cámara 0 como un
  blob completo de `CinematicCameraHeader` con sus claves, la duración de la secuencia 0, el punto de enganche 0 y su
  orientación: donde se pone el personaje) + `GlueLight` (64 B: tipo, posición y dirección hacia la luz al primer
  frame, ambiente y difusa × intensidad, atenuación). `vd_baker scan-glue` las lista. Medidas: el Portal 7.970
  vértices, 57 huesos, 28 emisores, 4 luces (2 puntuales de fuego, 2 direccionales), cámara de 1,50 rad diagonales; las
  escenas de raza tienen cámara fija a 3–5 yd del enganche 0 y 2–4 luces (clave cálida, relleno frío, ambiente).
- **`WTF\Config.wtf`** (`<cacheDir>/WTF/Config.wtf`, el del 1.12): `SET accountName`, `SET realmName`, `SET realmList`,
  `SET lastCharacterIndex`. La contraseña nunca se guarda.
- **`chat-cache.txt`** (por personaje en `WTF/Account/<CUENTA>/<reino>/<personaje>/`, igual byte a byte que el de WoW.exe:
  lector 0x498A60, escritor 0x499A80): `VERSION`, `ADDEDVERSION`, diez bloques `WINDOW` con nombre, tamaño de letra,
  color y opacidad del fondo, bloqueo, acoplado, y sus `MESSAGES` (68 grupos), `CHANNELS` y `ZONECHANNELS`.
  `macros-cache.txt` de cuenta y de personaje (el `macros-local.txt` de la fase 15 se migra) y `cache.md5` (los MD5 de los
  datos de cuenta) van en las mismas carpetas.
- `NameGen.dbc` (nombre, raza, sexo): la fuente de `GetRandomName`. Los textos de la creación (FACTION_INFO_*,
  RACE_INFO_*, ABILITY_INFO_*, CLASS_*) son de `GlueStrings.lua`.
- **Fuera de los MPQ** (carpeta del cliente): `Data\Interface\Cinematics\WOW_Intro_1024.avi` (47 MB) y `Logo_1024.avi`
  (códec `DXGM`: MPEG-4 de Blizzard) son la introducción y el logo que MovieFrame pone la primera vez y el botón
  «Cinematics» de AccountLogin; `Data\Credits.html` es el texto de CreditsFrame. Aún no se hornean.

- **`models.vdpk` v8** (fase 18, 408,3 MB; +85 MB): `ModelBspHeader` por WMO con los grupos, nodos de 16 B
  (MOBN), referencias (MOBR), triángulos y vértices, sin las caras de detalle (MOPY 0x04 / 0x80) ni las verticales, y
  listas de grupos por doodad (un doodad puede estar en el MODR de varios grupos): el grupo de la cámara y los portales
  como WoW.exe (0x6821F0, segmento vertical de 1.760 yd). El runtime exige la versión exacta.

## Retoques de la fase 20 (1.12.1)

- **Modelos de la interfaz** (`ui.vdpk` → `ui/models`, `AssetType::UiModels` 27, `pack/UiModelFormat.h`,
  `kUiModelsVersion` 1): los tres M2 que el 1.12 pinta en frames Model (`Interface\Cooldown\UI-Cooldown-Indicator`,
  `Interface\Buttons\UI-AutoCastButton`, `Interface\MiniMap\Ping\MinimapPing`) para dibujarlos con el pipeline de
  la UI en vez de una escena 3D por botón: `UiModelsHeader` (96 B) y sus arrays (modelos, secuencias, huesos, triángulos,
  lotes, emisores, pistas con su rango de claves por secuencia, claves, los texels de las texturas 1-D y las secuencias
  globales). La textura 1-D se dibuja como geometría: triángulos cortados en los centros de texel donde cambia la
  pendiente, con colores de vértice que reproducen el filtrado bilineal. Reglas: PROTOCOL §2.26.
- Atlas: 559 texturas en 3 páginas (la tercera desde estos retoques; es un array, no añade draws).

## Otros

- Fuentes en `fonts.MPQ`: `FRIZQT__.TTF`, `ARIALN.TTF`, `skurri.ttf`, `MORPHEUS.TTF`.
- Sonido: WAV (efectos, voces) y MP3 (música). Metadatos en `SoundEntries.dbc`, `ZoneMusic.dbc`, `SoundAmbience.dbc`.
- En la carpeta del cliente hay además `maps/`, `vmaps/`, `mmaps/` y `dbc/` **extraídos para el servidor**: no son
  del cliente y el runtime no los usa (el tap-to-move con navmesh se descartó del plan).
