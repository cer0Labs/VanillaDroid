# Arquitectura

## 1. Visión general

```
 PC (una vez por versión de datos)                          Android (Fold5)
┌─────────────────────┐    ┌──────────────────┐  adb push  ┌────────────────────────────────────┐
│ C:\World of Warcraft│───▶│ tools/baker (C++)│──packs────▶│ /sdcard/Android/data/<pkg>/files/  │
│ 1.12\Data\*.MPQ     │    │ MPQ → VDPK       │            │        │ mmap                      │
└─────────────────────┘    └──────────────────┘            │        ▼                           │
                                                           │  libvanilladroid.so (engine)       │
                                                           │        ▲ TCP 3724 / 8085            │
                                                           └────────┼───────────────────────────┘
                                                                    │  (adb reverse en desarrollo)
                                                              vMaNGOS (PC)
```

Dos programas, un mismo código base:

- **baker** (PC): toda la transformación cara — descompresión MPQ, decodificación BLP, transcodificación a
  ASTC, construcción de mallas, composición de terreno, colisión. Puede tardar minutos; no importa.
- **runtime** (Android / Windows): solo mapea packs, sube a GPU, simula y dibuja. Nada de parsing en caliente.

## 2. Módulos del motor (`engine/src/`)

| Módulo | Responsabilidad | Depende de |
|---|---|---|
| `core` | Tipos, log, asserts, `Result<T>`, reloj, allocators (arena de frame, pools), hash, textos de la interfaz (`Text`, ADR-0003) | — |
| `platform` | Superficie nativa, rutas de datos, hilos/afinidad, ADPF, entrada cruda | core |
| `jobs` | Pool de workers, `parallelFor`, dependencias simples, colas SPSC/MPMC | core |
| `pack` | Lector de packs VDPK (mmap, TOC por hash, LZ4). El escritor vive en el baker | core |
| `render` | Vulkan: dispositivo, swapchain, frames en vuelo, pipelines, buffers/texturas (VMA), batch 2D | core, platform |
| `world` | Terreno, WMO, doodads M2, cielo/niebla/luz, streaming por tiles, culling; actores (`Actors`) | render, pack, jobs, anim |
| `anim` | Esqueletos de los actores: muestreo de pistas, mezcla entre secuencias, variantes, reservas | pack |
| `physics` | Colisión horneada en streaming (`CollisionWorld`: BVH, terreno reconstruido, líquidos), consultas (rayo, barrido de esfera), controlador del personaje | core, pack |
| `net` | Sockets, hilo de red, realmd (SRP6), world (cifrado de cabecera), serialización | core |
| `game` | Almacén de entidades + update fields; apariencia desde los DBC (`Appearance`); jugabilidad (`Gameplay` y sus áreas: inventario, habilidades, social, viajes) | net, pack |
| `ui` | UI táctil propia: layout por anclas, widgets, texto SDF, gestos | render |
| `audio` | miniaudio (AAudio / OpenSL ES en Android, WASAPI en escritorio, null y WAV para pruebas): mezclador de 48 voces (PCM16, IMA ADPCM, MP3 por streaming), categorías, 3D, tablas de sonido de los DBC, superficie del suelo para los pasos | core, pack |
| `app` | `Game`: orquesta el frame y el ciclo de vida; lo llaman las plataformas | todos |

**Reglas de dependencia**: `render` no conoce el juego (recibe listas de dibujo); `game` no conoce Vulkan;
`ui` dibuja a través del batch 2D de `render`. `net` entrega paquetes ya enmarcados; `game` los interpreta.

Targets CMake: `vd_core` (core, usable por tools y tests) → `vd_net` (red y protocolo) → `vd_game` (estado del
juego y apariencia), `vd_anim` (esqueletos) y `vd_physics` (colisión y controlador; también lo usa el baker) →
`vd_engine` (runtime completo, con Vulkan). `vd_formats`: parsers de formatos originales, **solo PC**
(baker y tests). Terceros compilados por el motor: `vd_lz4`, `vd_mbedtls` (bignum + SHA-1), `vd_zlib` (en Android,
la libz del sistema), `vd_vulkan`, `vd_imgui`.

## 3. Hilos y flujo de un frame

```
Hilo principal  │ input ─ cola red ─ simulación ─ lanzar jobs ─ esperar ─ construir listas ─ grabar+submit ─ present(Swappy)
Workers (N-2)   │                 [culling tiles] [animación] [partículas] [UI layout]
Streaming (3)   │  lee pack (mmap) ─ descomprime ─ prepara staging ─────────── (subida en el siguiente frame)
                │  vd-io (terreno), vd-assets (WMO/doodads), vd-actors (personajes, criaturas, objetos),
                │  vd-collision (tiles de colisión), vd-grass (fase 14: efectos de suelo por tile),
                │  vd-worldsound (fase 14: sonidos de doodads, salas de WMO y emisores del mapa)
Red (1)         │  recv ─ descifra cabecera ─ enmarca ─▶ cola SPSC ─▶ (principal)   ◀─ send
Audio (miniaudio)│ callback AAudio: mezcla (sin asignaciones; órdenes por cola sin bloqueo)
Audio IO (2)    │ vd-audio-io (toca las páginas de cada sonido antes de sonar y lee por delante la música),
                │ vd-ground (tabla de superficies de pasos del mapa)
Monitor (1)     │ vd-monitor, 1 vez/s: térmica ADPF, batería (JNI), memoria (/proc); escribe ajustes e informes
Vibración (1)   │ vd-haptics (fase 20): las vibraciones del racimo táctil por JNI, sin bloquear el hilo principal
Descarga (1)    │ vd-download (fase 22): vincular con VanillaDroid Setup y bajar los packs por la red local
                │ (app/DataDownload: HTTP con reanudación y SHA-256; estado al hilo principal sin asignaciones)
```

- **Un único hilo de juego** empieza grabando y enviando los comandos Vulkan. Si el hilo principal supera
  ~8 ms se separa un **hilo de render** (pipelining de 1 frame) — decisión basada en medición, no a priori.
- **2 frames en vuelo** (latencia vs. throughput); recursos por frame en anillos.
- Workers = núcleos − 2, sin fijar afinidad a mano. El hilo de juego abre una sesión ADPF (*performance hint*,
  `platform/PerformanceHint`) y le informa cada frame del trabajo real (frame − esperas) contra 16,67 ms. Medido
  en el Mi 11X: sin ella, el hilo vivía en los núcleos pequeños (A55 a 1,8 GHz) con los grandes a ~800 MHz. Si
  el dispositivo no ofrece sesión (Android < 13, o sin soporte en su HAL de energía, como ese Xiaomi), el hilo se
  fija a los núcleos que no son del clúster más lento. Desde F10 informa también del tiempo de GPU del frame
  (`APerformanceHint_reportActualWorkDuration2`, Android 15+) y cambia el objetivo a 33,3 ms con el límite de 30 fps.
- **Calidad dinámica** (F10, `app/QualityGovernor`, lógica pura con tests): el hilo `vd-monitor` (`platform/
  DeviceMonitor`) lee una vez por segundo el *thermal headroom* de ADPF (previsión a 10 s; 1,0 = estrangulamiento
  severo) y el estado térmico, y el hilo de juego solo copia la última lectura (`try_lock`, nunca espera). El
  gobernador baja por niveles el techo de la resolución del mundo, la distancia de visión y de los doodads y las
  partículas antes de que el sistema recorte frecuencias (nivel 1 a 0,75 de headroom: 90 % de resolución y
  distancia, 85 % de partículas; nivel 4 a 1,05 o estado crítico: 60 % de resolución y de distancia, 35 % de
  partículas, y 30 fps solo en estado crítico), y además reacciona a los frames perdidos: si el cuello es la GPU baja la resolución, si es la
  CPU las distancias y los efectos, y vuelve a subir probando con esperas crecientes. Nunca por encima de los
  ajustes del jugador (presets Baja / Media / Alta / Ultra, que también se guardan en disco).
- Comunicación entre hilos por colas sin bloqueo; nada de mutex en el camino caliente.
- **Workers y ADPF** (medido en F5): los workers entran en la sesión ADPF del hilo de juego. Sin sesión (Mi 11X) sus
  ráfagas de ~1 ms caían en núcleos pequeños a baja frecuencia (132 µs por esqueleto frente a 9–15 µs en el núcleo
  grande del hilo de juego) y fijarlos a los núcleos rápidos empeoraba (7 hilos en 4 núcleos: despertarlos le
  quitaba el núcleo al hilo de juego ~2 ms). Por eso el muestreo de esqueletos va a workers solo con ADPF, y el
  `JobSystem` despierta solo los workers que hacen falta y permite lanzar (`begin`) y esperar (`wait`) por separado.

## 4. Pipeline de assets y formato de pack (VDPK)

### 4.1 Baker

1. Abre los MPQ en orden de prioridad (`patch-N` > `patch-2` > `patch` > bases) con StormLib.
2. Enumera ficheros (listfile interno + listfile externo si hace falta).
3. Por tipo de asset, transforma y escribe en el pack correspondiente. Paralelo por asset, incremental
   (hash de la entrada → si no cambió y la versión del baker coincide, se reutiliza la salida).

| Entrada | Salida horneada |
|---|---|
| BLP (paleta / DXT1-3-5 / RAW) | **ASTC** con mips (UI 4×4, personajes 4×4–5×5, mundo 6×6, terreno 6×6–8×8). ETC2 como fallback opcional |
| DBC | Copia binaria tal cual (formato fijo, se lee con mmap) + índices por ID precalculados |
| ADT/WDT | Tile: malla de chunks (145 vért.), normales, agujeros; **una textura RGBA por tile** con 3 alphas + sombra; lista de capas por chunk; referencias a doodads/WMO; malla LOD lejana + textura compuesta; **colisión** |
| WMO | Por grupo: vértices entrelazados + índices 16 bits, batches **fusionados por material**, colores de vértice, portales, doodad sets, líquidos, colisión |
| M2 | Geometría (vistas/LOD embebidas en vanilla), huesos, pistas de animación normalizadas, materiales → id de pipeline, attachments, partículas/ribbons |
| Fuentes TTF | SDF horneado (stb_truetype) en `ui.vdpk`, con métricas y *kerning* |
| Audio WAV/MP3 | WAV (todos PCM16) → **IMA ADPCM** 4:1 (bloques de 1017 frames, codificador de nibble óptimo); MP3 de la música tal cual, en streaming. `sound.vdpk`: 7.062 sonidos, 509 MB (1,33 GB de entrada), ~50 s; más la superficie de pasos por chunk de terreno (`ground/<mapa>`) |

### 4.2 Formato VDPK v1 (fijado en `engine/src/pack/PackFormat.h`)

```
PackHeader (64 B)  magic 'VDPK', version, bakerVersion, clientBuild=5875, entriesOffset, entryCount, names…,
                   locale (fase 21: idioma de los datos)
Blobs              alineados a 64 B
PackEntry[] (32 B) pathHash u64, offset u64, storedSize, rawSize, nameOffset, type u16, compression u16
                   — ordenados por hash → búsqueda binaria
Name table         rutas canónicas terminadas en NUL (depuración y herramientas)
```

- **Runtime** (`pack/PackReader`): mapea el fichero en memoria (`platform/MappedFile`) y usa los datos en sitio;
  abrir un pack de GB es instantáneo y solo se cargan de disco las páginas que se tocan. Fase 21 (ADR-0004): un pack
  puede llevar encima su **pack de idioma** (`db.esES.vdpk` sobre `db.vdpk`), que gana en `find()`.
- **Baker** (`tools/formats/src/bake/PackWriter`): escribe los blobs según se producen (thread-safe) y la tabla
  ordenada al final.
- Hash: FNV-1a 64 de la ruta canónica (minúsculas, `/`), calculado sin reservar memoria (`core/Hash.h`). El baker
  aborta si detecta una colisión o un duplicado.
- Blob de textura: `TextureHeader` (formato, flags alfa/recorte, tamaño, tabla de 16 mips) + mips ASTC.
- Compresión: ninguna para ASTC y DBC; **LZ4** (alta compresión en el baker) para los tiles de terreno. El
  runtime descomprime en el hilo de IO (`PackReader::read`).
- Packs actuales: `db.vdpk` (158 DBC), `textures.vdpk`, `models.vdpk` (WMO y doodads), `actors.vdpk` (modelos
  animados), `sound.vdpk` (fase 9) y un `world_<mapa>.vdpk` por mapa con terreno.

### 4.3 Terreno horneado (`engine/src/pack/TerrainFormat.h`)

- Por mapa: `<mapa>.wdt` (qué tiles existen + alturas mín./máx. + índices compartidos de los 4 niveles LOD),
  `terraintextures` (**un texture array** con todas las texturas de terreno del mapa, 256² ASTC 6×6), `horizon`
  (alturas WDL de todos los tiles + malla de un tile) y un blob por tile; en cada pack, `liquidtextures`
  (4 líquidos × 30 frames).
- Tile: 256 chunks × 145 vértices de **8 bytes** (altura f32 + normal snorm8): la posición XY es implícita y la
  reconstruye el vertex shader desde `gl_VertexIndex`. Índices u16 (37.120 vértices): los tiles sin agujeros usan
  un índice compartido del mapa; los que tienen agujeros llevan el suyo. Tabla de 256 chunks (capas, flags,
  alturas, rangos de índices) leída como SSBO en el vertex shader.
- Alphas + sombra: una textura 1024×1024 **B4G4R4A4** por tile (R, G, B = alpha de las capas 1–3, A = sombra),
  sin pérdidas respecto a los 4 bits originales; el shader muestrea dentro del bloque 64×64 de su chunk.
- LOD: 4 cadenas de índices (pasos 1/2/4/8 vértices exteriores) sobre los mismos vértices; el borde del tile se
  conserva completo en todos los niveles, así que tiles vecinos de distinto nivel no dejan grietas.
- Líquidos: vértices de 8 bytes (altura + rejilla 9×9 implícita + profundidad + tipo), primero los translúcidos
  (río, océano) y luego los opacos (magma, limo). Tamaño máximo de un tile horneado: 4 MB.
- Ubicación en el móvil: `getExternalFilesDir()/packs` = `/sdcard/Android/data/dev.cer.vanilladroid/files/packs/`
  (sin permisos; se sube con `scripts/push-data.ps1`). En escritorio: `<repo>/packs` o `--packs <dir>`.
- GPU sin ASTC (la GTX 1080 del PC de desarrollo): el runtime de escritorio decodifica en CPU al subir.
- Colocaciones: cada tile lleva sus doodads (`TerrainDoodadInstance`, 64 B) y WMO (`TerrainWmoInstance`, 96 B)
  con la transformación modelo → mundo ya calculada por el baker (matriz 3×4) y el `uniqueId` del WMO.

### 4.4 Modelos horneados (`engine/src/pack/ModelFormat.h`, `models.vdpk`)

- Un formato común para M2 (doodads estáticos) y WMO: vértices de 28 B (posición f32, normal snorm8, UV f32,
  color RGBA8 = colores de vértice del WMO), índices u16, materiales deduplicados (textura por hash, mezcla,
  flags), partes (rango de índices + material + grupo), grupos (AABB, flags, portales), portales, doodad sets
  y doodads del WMO.
- WMO: vértices de los grupos en orden Morton y partes ordenadas por (mezcla, material, iluminación, grupo): el
  mismo material de grupos vecinos queda contiguo y el renderer lo dibuja con un solo draw. Los WMO de más de
  65.536 vértices (las ciudades) usan índices de 32 bits (`kModelIndices32`) para que un material nunca quede
  partido en varios rangos de vértices (Ventormenta desde la puerta: 569 → 258 draws de WMO).
- Líquidos de WMO (`MLIQ`): partes con material `kModelMaterialLiquid` (tipo en `color`), dibujadas con el
  `liquid.frag` del terreno y su texture array de frames animados.
- 816 WMO + 3.805 M2 → 295 MB en el pack (~22 s de horneado). Texturas en `textures.vdpk`, por hash de ruta.
- Fase 11 (v6, 321,7 MB, 26 s): sección de luz de los WMO (MOLT, MFOG, MOLR, rejilla de luz de los interiores, grupo
  de cada doodad), luces de los M2, los flags "inalcanzable" / "siempre dibujado" de los grupos, el skybox MOSB y los
  5 modelos del cielo (`AssetType::SkyModel`). Ver FORMATS.md.

### 4.5 Actores horneados (`engine/src/pack/ActorFormat.h`, `actors.vdpk`)

- Razas, criaturas (`CreatureModelData`) y objetos equipables (`Item\ObjectComponents`): 3.097 modelos, 113 MB.
- Vértices con 4 pesos, partes por (material, geoset), materiales que toman la textura del actor (piel, pelo, capa,
  pieles de criatura), huesos con pivote, secuencias, attachments y pistas de color/opacidad. Las claves de cada
  secuencia van juntas (tiempos locales, valores en los bordes calculados en el baker): una pose se lee de un bloque
  de pocos KB. Detalle en [FORMATS.md](FORMATS.md).

### 4.6 Colisión horneada (`engine/src/pack/CollisionFormat.h`, `collision_<mapa>.vdpk`)

- Por tile del mapa: alturas del terreno (u16 sobre la base de cada chunk) y agujeros, los triángulos de colisión de
  los WMO y M2 colocados (en coordenadas de mundo, en un BVH) y las superficies de los líquidos (otro BVH). Un pack
  aparte del mundo: se re-hornea (30 s los dos continentes) sin volver a subir el terreno. Detalle en
  [FORMATS.md](FORMATS.md).

## 5. Renderer (diseñado para GPU de tiles)

- **Forward**, un render pass principal. `loadOp = CLEAR/DONT_CARE`, depth `storeOp = DONT_CARE` y
  memoria `LAZILY_ALLOCATED` (transitoria). Nunca se lee el depth en un pase posterior.
- **Orden actual (F5)**: antes del render pass, las subidas del frame, las **pieles compuestas** (un render pass
  256×256 por piel nueva + mips con blits) y la copia de la paleta de huesos a memoria de GPU. Dentro: terreno
  (1 draw/tile, de delante a atrás, nivel LOD por distancia) → líquidos opacos (magma, limo) → modelos opacos y
  alpha-test (M2 + WMO, con su magma) → **actores opacos** (dos barridas: partes normales y luego alpha-test, para
  cambiar de pipeline pocas veces) → **manchas en el suelo** (fase 14: sombras y huellas, quads del búfer de
  partículas) → **hierba** (fase 14, mezclada con alpha test a 128) → horizonte WDL (1 draw instanciado) → **cielo** (fase 11, a profundidad 1, solo
  donde no hay nada: cúpula con los anillos del 1.12, estrellas y las dos lunas y el sol sumados, nubes mezcladas, o
  el skybox del conjunto de luz o del WMO) → agua translúcida del terreno → modelos transparentes (alpha, add, mod,
  mod2x, agua de WMO) → actores transparentes (y los **translúcidos**: fantasmas y sigilo, fase 11) → **partículas y
  ribbons** (fase 9) → **clima** (fase 11) → **resplandor del sol o la luna** (aditivo, sin prueba de profundidad) →
  UI.
- **Clima** (fase 11, `game/Weather` + `world/WeatherEffect` + `app/WeatherControl` + `render/WeatherRenderer`):
  `SMSG_WEATHER` fundido en el cliente, partículas sin búferes de vértices (cada vértice saca su gota, copo o grano
  de `gl_VertexIndex`, una semilla y el reloj, con las reglas de los programas `rain.bls` / `patter.bls` /
  `snowpoint.bls` / `sand.bls` del cliente: cola con desenfoque de movimiento, salpicaduras, nieve que se posa, nieblas
  de nieve y de arena), 2 draws, 3 pipelines creadas al cargar; el suelo de las salpicaduras es una rejilla toroidal
  de 32×32 alturas alrededor de la cámara muestreada contra la colisión (≤ 48 puntos por frame). Nada dentro de los
  edificios ni bajo el agua; la intensidad empuja la luz hacia los conjuntos de tormenta de `Light.dbc`.
- **Cielo** (fase 11, `world/Celestial` + `world/Sky` + `render/SkyRenderer`): curvas del ciclo día/noche sacadas de
  WoW.exe (dirección de la luz desde el noroeste, sol, Dama Blanca y Niño Azul, estrellas, resplandores, luz de las
  nubes), nubes de ruido generado al cargar (256² RGBA8, excepción a la regla de ASTC) con la cobertura de Hugo
  Elias, modelos del cielo horneados aparte (`pack/SkyFormat.h`). La ocultación del resplandor es un rayo repartido
  en varios frames contra la colisión y las esferas de los árboles grandes (el 1.12 usa una consulta de oclusión de
  la GPU, que aquí obligaría a leer el depth).
- **Efectos a pantalla completa sin pases** (fase 11, `shaders/screenfx.glsl`, espejo en CPU `render/ScreenFx.h`):
  el "Full-Screen Glow" (`ffxGlow`: color + glow·color², glow = `LightParams.glow`) y el "Death Effect" (`ffxDeath`:
  luminancia + tinte 0x5393A8 en los tonos medios) del 1.12 se aplican al final de cada fragment shader del mundo
  (terreno, modelos, actores, líquidos, horizonte, cielo, partículas, clima) en vez de en pases propios: en una GPU
  de tiles cada pase completo vaciaría los tiles. La luz se satura a 1 como en el cliente (sus colores de vértice del
  terreno van fijados y `Model2.bls` suma con `ADD_SAT`).
- **Luz de interiores y de noche** (fase 11, `world/InteriorLighting`, `shaders/interior.glsl` y `surface.glsl`):
  cada instancia de M2/WMO lleva 8 B de luz (ambiente y directa RGB8 + dirección octaédrica; 0 = fuera,
  `ModelInstanceData` 56 B): los doodads de un interior la reciben al aparecer (MODD + MOHD + lámpara MOLT más
  fuerte), los actores la buscan con `WorldObjects::lightAt` (caja de interior más pequeña a la altura del pecho y su
  rejilla de luz + la lámpara más cercana, en caché hasta moverse 0,5 yd o 32 frames). Las superficies de los WMO se
  iluminan por clase de lote con el alfa del color de vértice (sin draws extra): por fin las calles de Ventormenta y
  los edificios de Orgrimmar siguen a la noche. Ventanas que brillan de noche (materiales SIDN × factor nocturno),
  la niebla MFOG del interior donde está la cámara en vez de la de fuera, las luces propias de los M2 por push
  constant (112 B) y, en el terreno, especular por vértice, capas MCLY que se desplazan y lava sin luz.
- **Unidades translúcidas** (fase 11, `game/UnitFade`): la opacidad y el tinte salen de los procs 14 y 1 del
  `SpellVisualKit` de estado de las auras visibles de la unidad (fantasma 0,5 azulado, Sigilo / Acechar / Esfumarse
  0,3, Invisibilidad y Desvanecerse 0,5, Forma de las Sombras 0,65); se dibujan con 4 pipelines de actor mezcladas
  sin escribir profundidad, en una segunda barrida del pase transparente que solo corre si alguna lo es.
- **Partículas** (fase 9, `vd_fx`: `fx/ParticleSystem`, `fx/EffectSet`; `world/Effects`; `render/ParticleRenderer`):
  los emisores y ribbons de los M2 (doodads, objetos del servidor, WMO de transportes, actores y lo que llevan) se
  simulan con las reglas del cliente 1.12 (formas plano/esfera/spline, espacio de mundo o de modelo, gravedad,
  arrastre, curvas de tres puntos, celdas de textura) y se dibujan como quads construidos en la CPU (relativos a la
  cámara, 20 B por vértice, anillo de 3 búferes mapeados copiados con un `memcpy`), en el mismo render pass: 4
  pipelines (alpha, add, mod, mod2x), profundidad probada y no escrita, alpha de atrás a delante y aditivas agrupadas
  por textura. Presupuesto: pools reservados al arrancar (~9 MB), ≤ 512 partículas por emisor y 16.384 quads por
  frame, nada fuera de pantalla ni a más de 100 yd + su alcance, emisión al 30 % entre 25 y 100 yd y recortada sola
  si la simulación pasa de 1 ms. Workers solo con ADPF (y ≥ 2.048 quads). La animación de UV de los materiales
  (agua, lava, energía) va en una matriz 2×3 por push constant.
- **Mundo vivo** (fase 14):
  - **Eventos de los M2** (`Actors::events()`): el `Animator` guarda el tramo de animación que avanzó cada frame
    (`played()`, hasta dos trozos al dar la vuelta un bucle) y `Actors::beginCollect` cruza con él los tiempos
    horneados de cada evento (`actors.vdpk` v5) de los actores a menos de 80 yd de la cámara; `finishCollect` les
    pone la posición del punto de su hueso con la pose ya muestreada. Los usan los pasos, el polvo y las huellas
    (`$FL/$FR/$RL/$RR`, `$FSD`), el aliento (`$BTH`), los sonidos de emote (`$CSD`), las sacudidas (`$SHK`) y el sedal
    (`$CCH` de la caña).
  - **Doodads animados y billboards** (`world/DoodadAnimation`, `shaders/doodad.vert`): los 383 M2 que se mueven y los
    156 con vértices billboard llevan esqueleto en `models.vdpk` v7; una pose por modelo y fase (8 por bucle, con la
    hora a la que apareció la instancia como el `CM2Model::SetAnimation` del 1.12) en una paleta propia, LOD de
    animación por distancia, skinning de 4 huesos y giro hacia la cámara en el vertex shader (8 pipelines al cargar).
    Distancias y fundido del 1.12 (WoW.exe `0x683F80`): 50 / 125 / 200 yd según el radio del fichero, medido en
    horizontal, con un tramo de fundido de 10 / 25 / 50 yd hecho con un tramado 4×4 (sin mezcla: los opacos siguen
    opacos para el LRZ).
  - **Hierba** (`world/GroundEffects` + `world/GrassPlacement` + `render/GrassRenderer`): los registros por tile de
    `grass.vdpk` se leen en `vd-grass`; el generador de WoW.exe (`CMapChunk::CreateDetailDoodads`) coloca ≤ 6 chunks
    por frame (presupuesto 0,5 ms) en un pool de 160; un draw instanciado por modelo; texturas creadas de 2 en 2 por
    frame. Se desvanece entre 52,5 y 70 yd de profundidad como la rampa del cliente.
  - **Efectos del cuerpo** (`world/UnitBodyEffects`, `world/BodyDecals`, `game/BodyData`): sombra (cajas de las
    secuencias en `bodyfx.vdpk`), huellas y ondas en pools fijos, rociadas, sangre por `UnitBloodLevels`, aliento,
    burbujas y la nube al montar, todo como quads del búfer de partículas (las manchas en su propio tramo del pase).
  - **Efectos de hechizo** (`world/SpellBeams`, `world/CameraShakes`, `game/SpellKitFx`): haces de
    `SpellChainEffects` (la geometría `CLightning` de WoW.exe) y el sedal como quads aditivos del pase de partículas;
    sacudidas aplicadas solo a la matriz de vista dibujada; tintes y destellos de los kits por `ActorDraw::fade`;
    partículas que emiten modelos escritas como triángulos de su malla horneada.
  - **Sonido del mundo** (`app/GameAudioModels`, `world/WmoAreas`, `audio/WorldSoundTables`): el oyente es la cabeza
    del personaje (regla del 1.12), atenuación DirectSound con `SoundRolloffFactor` 4, sonidos de doodads horneados por
    mapa en celdas de 32 yd y consultados 4 veces por segundo, salas de `WMOAreaTable` por la rejilla de suelos de cada
    grupo (música, ambiente y nombre de subzona), pasos de todos y una etapa de salida (+12 dB y limitador a −1 dBFS).
  - **Cinemáticas** (`app/Cinematics`, `world/CinematicCamera`, `cinematics.vdpk`): la cámara del M2 (bezier) se
    coloca con `CinematicCamera.dbc`; la interfaz se oculta como `CinematicFrame` y la voz en off suena con ella.
    **Primera persona**: con el zoom al mínimo el personaje no se dibuja (`Actors::setCameraFade`); con la cámara
    metida en él, translúcido hasta `PlayerFadeOutAlpha`.
- **Actores** (`ModelRenderer::drawActors`): skinning en el vertex shader con 4 huesos de una paleta por frame (SSBO:
  el juego la escribe en un anillo de 3 buffers mapeados, porque la rellena antes de esperar la fence de su frame, y
  el frame la copia a memoria de GPU). Cada actor lleva sus texturas (piel compuesta, pelo, capa…) y sus geosets
  visibles; los índices de sus partes visibles se empaquetan por material en una malla compartida entre actores que
  llevan los mismos geosets, así que un personaje son ~3 draws más sus armas, casco y hombreras (antes ~11: los
  geosets ocultos partían cada tanda). Colores de material animados por push constant.
- **Constantes por frame** en un UBO (`shaders/frame.glsl`): viewProj relativa a la cámara (+ pre-rotación),
  luz/ambiente/niebla/cielo/agua de `Light.dbc` evaluados en la posición de la cámara y hora del día
  (`world/Lighting`). Los shaders comparten el bloque con `#include`.
- **Pipelines**: `render/vk/Pipeline` (descripción compacta) y `VkPipelineCache` persistido
  (`cacheDir/pipeline_cache.bin`, validado contra GPU/driver antes de reutilizarlo).
- **Resolución de escena separada de la UI** (F10): a escala 1 el mundo va directo a la swapchain como siempre;
  por debajo (0,5–1, `Renderer::setRenderScale`) un pase de escena compatible con el principal (mismas pipelines)
  lo dibuja en la esquina de una imagen del tamaño de la swapchain, con el *render area* reducido (en una GPU de
  tiles se binnean menos tiles; cambiar la escala no reserva nada), y el pase principal lo estira con un triángulo
  (bilineal + un enfoque ligero acotado por los vecinos, `shaders/upscale.*`) bajo la UI, que conserva la resolución
  nativa. Medido en el Fold5: el coste de GPU de nuestras escenas lo marca la geometría (draws, triángulos, binning),
  no los píxeles; con la mitad de resolución la GPU siguió en las mismas frecuencias. Es la válvula para GPUs que
  sí van justas de píxeles y para el calor. La alternativa (swapchain pequeña escalada por el compositor) sigue sin
  medir: la UI perdería nitidez.
- **Pipelines finitas y precreadas**: vanilla tiene pocos modos de mezcla (M2: opaque, alpha key, alpha,
  add, mod, mod2x…; WMO: pocos shaders). Se enumeran todas las permutaciones al arrancar y se persiste el
  `VkPipelineCache`.
- **Pocas draw calls**:
  - Terreno: **1 draw por tile** (256 chunks), texturas de capa en *texture array*, alphas+sombra en una
    textura por tile, índices de capa por chunk en un SSBO.
  - Doodads M2: **instancing** por (modelo, parte); transformaciones por instancia (3×4, relativas a la cámara)
    en un vertex buffer por frame (`VK_VERTEX_INPUT_RATE_INSTANCE`).
  - WMO: un draw por edificio visible, limitado a sus grupos visibles (frustum + tamaño en pantalla; los
    interiores, además, por portales), y las partes contiguas de grupos visibles con el mismo estado fusionadas
    en un único `vkCmdDrawIndexed`. Los *push constants* solo se envían cuando cambian.
  - 10 pipelines de modelo (opaco, opaco a dos caras, alpha-test ×2, alpha, add, mod, mod2x, líquido opaco y
    translúcido) + 8 de actor (las mismas variantes con skinning) + 2 de composición, todas creadas al arrancar.
- **Orden**: opacos (front-to-back) → alpha-test → transparentes (back-to-front) → UI. El *alpha test*
  (`discard`) rompe la eliminación de superficies ocultas del Adreno (LRZ): shaders mínimos y medir; valorar
  *alpha-to-coverage* con MSAA.
- **Precisión**: `mediump` / `RelaxedPrecision` en fragment shaders → FP16 en Adreno.
- **Datos por frame**: buffer uniform/SSBO en anillo persistente mapeado; *push constants* para lo por-draw.
- **Culling** (F3, hilo principal): esferas de instancia en arrays densos (esfera, modelo, estado del modelo:
  ~20 B por instancia recorridos en secuencia, sin raíz cuadrada). Medido en Ventormenta (Mi 11X, ~17.000
  instancias): culling + listas de dibujo 0,44 ms de media por frame, grabación 0,84 ms.
  Grupos de WMO por esfera y tamaño en pantalla. **Interiores** (grupos `0x2000` sin `0x40`): solo si la cámara
  está dentro de su caja o alguno de sus portales está en el frustum a menos de 40 + 60·r yd (una puerta: ~190 yd);
  en Ventormenta es un 30 % menos de triángulos de WMO sin diferencias visibles. Queda para F6 (con el BSP de los
  grupos para saber en cuál está la cámara) el recorrido de portales completo, que recortaría también lo que
  tapan otros edificios. Si el culling llega a pesar, pasa a workers.
- **Iluminación vanilla** (barata): sol direccional + ambiente de `Light.dbc`/`LightIntBand`, colores de vértice
  en WMO, niebla lineal. Sin sombras dinámicas (vanilla usa manchas bajo los personajes). Fase 11: `LightConditions`
  elige el conjunto de parámetros de cada luz (despejado, tormenta por la intensidad del clima, bajo el agua según la
  cámara, muerte para el fantasma); bajo el agua y muerto, la niebla es absoluta (167 / 180 yd) y su inicio negativo
  tiñe lo que está junto a la cámara (no hay cáusticas en el 1.12.1: las texturas existen pero nada las usa).
- Sin post-proceso a pantalla completa salvo el escalado final (y opcionalmente un sharpen barato); los efectos
  de pantalla del 1.12 van dentro de cada shader (arriba).

## 6. Streaming y memoria

Implementado (F2): `world/Terrain` decide qué tiles quiere (distancia a la cámara), `world/TerrainStreamer`
lee + descomprime en el hilo `vd-io` (colas SPSC sin bloqueo, 4 búferes reciclados) y el hilo principal sube
como mucho 2 tiles por frame a través de `render/vk/Uploader`: un *staging ring* de 24 MB cuyas copias se
graban al principio del command buffer del propio frame (antes del render pass). Los recursos de un tile se
destruyen cuando los frames que lo usan han terminado.

- El hilo de IO descomprime en RAM normal y copia de forma secuencial a un bloque de staging (memoria
  host-visible, que en Adreno se lee como no cacheada); la GPU copia desde ahí, así que el hilo principal solo
  crea recursos y graba copias (≤ 0,75 ms por tile medido en Adreno 650). El bloque se recicla cuando termina el
  frame que contiene la copia (`Renderer::completedFrames()`).
- Tiles cargados dentro de la distancia de visión, pedidos del más cercano al más lejano, con presupuesto de GPU
  (320 MB por defecto; ¾ o ½ con `onTrimMemory` RUNNING_MODERATE / LOW / CRITICAL, descargando los más lejanos).
- LOD: niveles de índices (pasos 1/2/4/8) sobre los mismos vértices, elegidos por distancia; más allá de la
  distancia de visión, siluetas del WDL (`horizon`) en color de niebla.
- Objetos (F3, `world/WorldObjects` + `world/AssetStreamer`): al cargar un tile se crean sus instancias; los
  modelos y texturas se piden por hash con conteo de referencias. El hilo `vd-assets` lee y descomprime en un
  *staging ring* propio de 48 MB (cabe el WMO más grande, Ventormenta, 29 MB) y el hilo principal sube como mucho
  32 recursos / 16 MB / 2 ms por frame. Un WMO que cruza varios tiles es una sola instancia (por `uniqueId`); su
  doodad set se instancia cuando llega su modelo, por tandas de ≤ 1 ms por frame (una ciudad tiene miles de
  doodads: de golpe costaba 8,7 ms en un frame en la Bahía del Botín). Culling por frame: esfera en frustum y distancia (doodads:
  80 + 30·r yd; grupos de WMO: 150 + 150·r yd, es decir, ~6 px de radio; interiores, además, por portales).
- Actores (F5, `world/Actors` + su `AssetStreamer` en `vd-actors`, staging de 16 MB): modelos y texturas por hash
  con conteo de referencias; el modelo conserva su copia en CPU (el esqueleto se muestrea de ella). Un aspecto nuevo
  no sustituye al mostrado hasta que todo lo suyo ha llegado; entonces se compone la piel en GPU (≤ 4 por frame;
  las idénticas se comparten) y se empaqueta la malla de sus geosets (compartida igual).
- Por frame: se avanzan todas las animaciones (barato), se descarta por frustum y distancia (60 + 40·r yd, máx. 150),
  y los visibles se muestrean con LOD: cada frame a < 30 yd, cada 2 a < 60, cada 4 a < 120 y cada 8 más lejos; en
  medio se reutiliza la última pose. Fuera de pantalla no se muestrea nada.
- Colisión (F6, `physics/CollisionWorld` + `pack/AsyncReader` en el hilo `vd-collision`): los tiles que toca un
  cuadrado de ±96 yd alrededor del jugador (el suyo primero), leídos, descomprimidos y validados fuera del hilo
  principal a un búfer propio, y soltados a +64 yd de ese radio. En una ciudad son 1–4 tiles (hasta ~15 MB). Tras
  un teletransporte el personaje espera (sin enviar nada) a que el tile bajo sus pies esté cargado.
- Recursos compartidos (texturas, modelos) con conteo de referencias; se liberan cuando desaparece la última
  referencia y los frames que los usaban han terminado.
- GPU: VMA con bloques de 32 MB (por defecto crece hasta 256 MB, y un bloque nuevo a mitad de partida tardó
  hasta 62 ms en el Adreno 650). Lo que se crea jugando (tiles, modelos, texturas) sale de pools con 384 MB
  reservados al arrancar en bloques de 48 MB (`Device::createStreamedBuffer/Image`; cabe el modelo más grande,
  Ventormenta, 32 MB); lo que no cabe va por la ruta normal de VMA. Cada bloque nuevo queda en el log con lo que
  tardó.
- Presupuesto total objetivo ≤ 2 GB (ver PERFORMANCE.md). Medido en el Fold5 en la multitud de Orgrimmar: PSS 1,23 GB
  (705 MB de gráficos, 297 MB de páginas de los packs mapeados, 122 MB de heap nativo).
- `onTrimMemory` con su nivel (F10: `MainActivity` lo pasa al motor, GameActivity solo manda `APP_CMD_LOW_MEMORY`
  sin él): en primer plano bajo presión (niveles 5–15, hasta Android 13) el terreno deja de crecer y se vacía la
  caché de modelos de efecto; en la lista de segundo plano (40+) se sueltan las páginas de todos los packs mapeados
  (`MappedFile::releaseResidentPages`, `madvise(MADV_DONTNEED)`: limpias, se releen del fichero al tocarlas): de 648
  a 358 MB residentes. La memoria de GPU se queda a propósito: casi toda es la reserva de streaming, cuyos bloques
  no vuelven al driver, y descargar el mundo no liberaría nada y costaría recargarlo al volver.

## 7. Red

Implementado en F4 (`vd_net`, `vd_game`; ninguna depende de Vulkan, así que los tests las usan directamente):

- **Hilo `vd-net`** (`net/NetClient`) con sockets bloqueantes: login en realmd (SRP6 con el bignum y el SHA-1 de
  Mbed TLS), elección de reino, sesión de mundo (`net/WorldConnection`: cabeceras, digest, cifrado vanilla con la
  session key K) y `CMSG_PING` cada 30 s. Infla ahí mismo `SMSG_COMPRESSED_UPDATE_OBJECT` y desempaqueta
  `SMSG_COMPRESSED_MOVES`, así que el hilo principal solo ve paquetes planos.
- Con el hilo principal habla **solo por dos anillos SPSC de registros de tamaño variable** (`net/PacketRing`:
  4 MB de entrada, 256 KB de salida): sin mutex ni asignaciones por paquete. Si el hilo principal se retrasa
  (cargando un mapa), el de red espera y TCP frena al servidor.
- **Credenciales**: la contraseña solo entra por la línea de órdenes o el intent y se convierte al momento en
  `SHA1(CUENTA:CONTRASEÑA)`, que vive en memoria; nunca se escribe en disco (`client.cfg` guarda solo la cuenta y
  la dirección).
- **Sesión en el hilo principal** (`app/GameSession`): lista de personajes, entrada al mundo (o automática con
  `character`), almacén de entidades, hora del servidor, reconexión automática (5 s; doblando hasta 1 min si falla
  antes de entrar) y el panel "Sesión".
- Opcodes, update fields y códigos de respuesta se **generan** desde vMaNGOS con `scripts/gen-protocol.ps1`.
  `tools/netcheck` (`vd_netcheck`) prueba login, mundo y parseo contra un servidor sin arrancar el juego.
  Detalle del protocolo en [PROTOCOL.md](PROTOCOL.md).

## 8. Modelo de juego

- El servidor es autoritativo **excepto en el movimiento** del jugador (cliente autoritativo en vanilla):
  el cliente necesita física y colisión propias y envía `MSG_MOVE_*` con timestamps coherentes.
- Almacén de entidades por GUID con los *update fields* como arrays planos `u32` (como en el protocolo)
  (`game/Entities`, F4). Pendiente: máscara de cambios por frame → los sistemas (render, UI) reaccionan solo a lo
  que cambió.
- Entidades remotas (F4): posición del bloque de movimiento, de los `MSG_MOVE_*` y de los splines de
  `SMSG_MONSTER_MOVE` (recta del punto inicial al destino en su duración); desde F5 los jugadores que se mueven se
  extrapolan hasta 1 s desde su último paquete. Queda para F6 la interpolación con búfer y el camino completo del
  spline. Coordenadas del servidor = coordenadas del mundo del motor.
- **Unidades como actores** (F5, `app/Units`): cada criatura o jugador en rango es un actor. Su aspecto sale de sus
  update fields con `game/Appearance` (DBC de `db.vdpk`): criaturas por display (con `CreatureDisplayInfoExtra` y
  piel horneada para los PNJ humanoides), jugadores por raza y personalización más el equipo, cuyo display se pide
  al servidor (`CMSG_ITEM_QUERY_SINGLE`, en caché); se reconstruye solo cuando cambian esos campos. La animación
  sale del estado: muerto, montado (la montura es otro actor con el jinete en su attachment 0), andar/correr/nadar
  según la velocidad del spline o los flags de movimiento (el ciclo se acelera para que los pies no patinen),
  sentado/dormido/de rodillas, emotes de PNJ en bucle, o de pie con sus variaciones. Nombres (consultas de nombre
  y de criatura) y vida en placas sobre la cabeza (de momento con ImGui; la UI de verdad es F7).

## 9. Interfaz táctil

- **UI propia en C++** (no se reimplementa FrameXML/Lua del cliente original: es enorme y pensada para ratón).
  Se reutilizan las **texturas y fuentes originales** para conservar el aspecto.
- Layout por anclas + escala en dp + *safe areas*; dos perfiles: **pantalla exterior** (23:9, estrecha) e
  **interior** (~6:5, casi cuadrada). Cambio en caliente al plegar/desplegar sin recrear el dispositivo Vulkan.
- Controles: joystick virtual (izq.), cámara por arrastre y pellizco (der.), toque para seleccionar
  (raycast contra cajas de entidades), botón de ciclo de objetivo, barras de acción paginadas y grandes,
  pulsación larga = menú contextual, auto-correr. Mando físico vía games-controller (Paddleboat).
- Texto: campos de distancia (SDF) **horneados en el PC** con las fuentes del cliente (ADR-0002, sin FreeType en el
  móvil); sprites del cliente en un atlas horneado y los iconos en un array de GPU que se llena bajo demanda. UI
  inmediata (`ui::Context` + `ui::DrawList`): un pipeline y normalmente **1 draw call** para toda la UI (solo se
  parte por recortes o por textura externa, como el minimapa).
- Alfa premultiplicado: los quads aditivos del cliente (`alphaMode="ADD"`) van en el mismo pipeline (ADR-0002).
- Ventanas (`app/Dialogs`): vistas puras sobre *view-models* con widgets táctiles (`ui/Widgets`: botones, campos,
  casillas, deslizadores, listas con desplazamiento por arrastre e inercia, huecos de objeto, dinero) y dos
  perfiles, `Wide` (dos columnas) y `Square` (una). Las modales llevan un bloqueador que no deja pasar dedos al
  mundo ni al HUD.
- Entrada al mundo: las pantallas del GlueXML del 1.12 (fase 18, §9.7) sobre su escena 3D; el campo editado sube
  sobre el teclado del sistema cuando no deja sitio.
- Texto con el IME del sistema (GameTextInput, incluido en GameActivity) a través de `ui::TextInput`; el campo
  dice su tipo y el teclado se adapta (contraseña enmascarada, sin sugerencias, "enviar" en el chat).
- Estado de juego y HUD: `game::Gameplay` (`vd_game`) parsea los paquetes de juego y mantiene combate, hechizos,
  auras, PNJ, misiones, botín, chat, experiencia y muerte; `app/GameScreens` lo lee cada frame para rellenar el HUD y
  las ventanas y aplica lo que toca el jugador (botón de contexto: atacar, hablar, saquear o usar el objeto cercano;
  el personaje encara solo a su objetivo); `app/ServerObjects` dibuja los objetos del servidor (cofres, objetos de
  misión, puertas) como instancias dinámicas de `WorldObjects`.
- Overlay de depuración con Dear ImGui (solo builds de desarrollo).

### 9.1 Jugabilidad completa (fase 8)

Cada área de juego es un trío **estado + vistas + pegamento**, en ficheros propios para que se puedan trabajar en
paralelo:

| Área | Estado y protocolo (`vd_game`, `vd_net`) | Vistas puras (`app/*Windows`) | Pegamento (`app/*Screens`) |
|---|---|---|---|
| Objetos | `game/Inventory` (modelo desde los update fields, acciones, vendedor, banco, lectura), `ItemActions` (qué ofrece un toque y adónde va lo que se suelta), `ItemTooltip` (el GameTooltip), `ItemData`; `net/ItemMessages` | bolsas, personaje con pestañas y estadísticas, tooltip con fila de acciones, vendedor, banco, lectura, cantidad, durabilidad; arrastre con pulsación larga (`ItemDrag`) | `InventoryScreens` (`showItemTooltip`, `addCharacterTab`) |
| Combate | `game/Abilities` (instructor, talentos, mascota, reputación, auto-repetición), `AbilityData`, `SpellText` (descripciones), `SpellBookTabs`, `Progress` (talentos, habilidades, combos), `CombatText` (registro de combate); `net/CombatMessages` | libro de hechizos, instructor, talentos, marco y barra de mascota, editor de botones, habilidades, reputación, registro | `CombatScreens` (12 páginas de 6 botones en el HUD, edición con pulsación larga, barras de forma) |
| Social | `game/Social` (grupo, canales, amigos, quién, hermandad, duelo; su propio anillo de eventos), `SocialData`, `CharCreate`, `ChatCommands`; `net/SocialMessages` | creación de personajes, marcos de grupo, chat con pestañas, ficha de jugador, emociones, ventana Social | `SocialScreens` (el chat vive aquí), `CharacterPreview` (el personaje en creación como actor delante de la cámara) |
| Viajes | `game/Travel` (zona, disparadores de área, exploración, vuelos, posadero, transferencias), `AreaData`, `TaxiData`, `WorldMapData`, `Transports` (rutas de barcos, zepelines y ascensores); `net/TravelMessages` | texto de zona, mapa del mundo, mapa de vuelo, pantalla de carga | `TravelScreens`; `app/Transports` mueve los WMO de los transportes y su colisión |

- `game::Gameplay` es dueño de los cuatro estados y les delega sus opcodes; `app/GameScreens` es dueño de los cuatro
  pegamentos y los llama en orden (HUD, ventanas modales encadenadas, superposiciones). Las ventanas comparten los
  bloques de `app/DialogKit` (métricas por perfil, panel modal, pergamino con desplazamiento, filas, casillas de
  objeto). Cada área tiene sus páginas de muestra en `VD_UI_GALLERY` y sus tablas DBC en su propio índice.
- Los PNJ se reparten por sus flags en `Gameplay::interact()`: gossip, misiones, vendedor, banquero, instructor,
  maestro de vuelo, sanador espiritual; el botón de contexto dice "Hablar" para todos ellos.
- Vuelos: el personaje sigue la spline del servidor sin simular ni enviar movimiento (`PlayerController`), la cámara
  se pone detrás y al acabar se envía `CMSG_MOVE_SPLINE_DONE`. Transportes: objetos móviles en `CollisionWorld`
  (`collision_objects.vdpk`) y WMO dinámicos en `WorldObjects`; el personaje que pisa la cubierta va con ella y
  envía su posición relativa (`MOVEFLAG_ONTRANSPORT`).
- **Textos** (ADR-0003): ningún texto visible está en el código; `core/Text` los busca por clave en la tabla del
  idioma (`ui.vdpk`): claves del cliente (`GlobalStrings.lua` / `GlueStrings.lua`: el registro de combate usa
  `SPELLLOG*` / `COMBATLOG_*` en sus cuatro formas yo/otro, los tooltips `ITEM_*`, los errores `ERR_*` /
  `SPELL_FAILED_*`, el acceso `LOGIN_*` / `CHAR_*`) o propias `VD_<ÁREA>_*` en `locale/enUS/<área>.lang`; traducciones
  en `locale/<idioma>/<área>.lang` (`esES` con el castellano de las fases 7 y 8). Cada área elige sus claves en
  funciones puras probadas (`game/SocialText`, `CombatText`, `ItemTooltip`, `Travel::taxiReplyText`...). Razas y
  clases salen de `ChrRaces` / `ChrClasses.dbc` (`game/CharacterNames`), como los demás nombres: en el idioma del
  cliente. El idioma: `locale` en `client.cfg` o `VD_LOCALE`. **Fase 21** (ADR-0004): con un cliente de ese idioma
  horneado (`vd_baker bake --only locale`) el juego abre sus packs de idioma (DBC, mapas del mundo, voces), toma los
  textos de ese cliente, reconoce sus órdenes `/` y emotes, y dice el idioma al servidor (misiones, objetos y PNJ en
  él); los canales de zona van por la red con los nombres del cliente base y se muestran traducidos.
- **Sonido** (fase 9, `app/GameAudio` sobre `audio/AudioSystem`): música y ambiente por zona y hora (AreaTable →
  ZoneMusic / SoundAmbience / ZoneIntroMusicTable, silencios entre pistas, introducción de ciudades), sonidos de
  interfaz con los nombres de PlaySound de FrameXML (las ventanas avisan por `UiSoundSink`), combate
  (WeaponImpactSounds por arma y víctima, golpes críticos, esquivas, paradas, quejidos, muertes), hechizos (kits de
  SpellVisual: precast, lanzamiento, misil a la velocidad del hechizo, impacto), PNJ (NPCSounds), emotes y pasos del
  jugador (cadencia de la zancada y superficie del suelo horneada). Desktop: `--audio off|null|wav:<fichero>|device`
  (por defecto apagado); Android: foco de audio y pausa en segundo plano.
- **Efectos de hechizo y animaciones de combate** (fase 9): `game/SpellVisualData` indexa SpellVisual, SpellVisualKit,
  SpellVisualEffectName, la velocidad de los misiles, emotes y brillos de encantamiento; `world/SpellVisuals` programa
  precast, lanzamiento, misiles (mismo tiempo de vuelo que el sonido), impactos, canalizaciones, estados de aura y áreas
  contra una interfaz (`SpellVisualWorld`, probada sin GPU) con un presupuesto de 256 efectos; `app/SpellEffects` lo
  alimenta con `Gameplay::fx()` y los campos de las unidades. Las animaciones de combate van en una **capa de acción**
  sobre la locomoción: el torso (máscara desde el hueso SpineLow, prioridades y flags de cuerpo de AnimationData) golpea
  mientras las piernas siguen corriendo (`game/CombatAnimation` elige la animación por arma y evento). Los modelos de
  efecto se guardan en una caché LRU (160 modelos, ~1 min) y se precargan: soltarlos al acabar cada efecto costaba
  tirones al recargarlos.
- Para la fase 9, `Gameplay::fx()` es un anillo con los inicios, golpes e interrupciones de hechizos, kits visuales,
  sonidos, música y emotes de **cualquier** unidad (no solo del jugador); sonido y efectos lo leerán con sus cursores.

### 9.2 Paridad con el cliente de escritorio

Misma regla que en la fase 8 (estado en `game/`, vistas puras, pegamento en `*Screens`), ahora con la interfaz
original como referencia. `app/Hud.cpp` se partió en varias unidades de la misma clase para trabajar en paralelo
(`Hud.cpp` núcleo, `HudUnitFrames.cpp`, `HudActionBar.cpp`, `HudAuras.cpp`, `HudWorld.cpp`, constantes y arte en
`HudShared.h`).

| Área | Estado / reglas | Vistas | Pegamento |
|---|---|---|---|
| Retratos | `world/PortraitCache` (LRU por aspecto, sin Vulkan), `Actors::appendStill` (pose de Stand propia) | `DrawList::portrait` (círculo) | `app/Portraits`; `render/PortraitRenderer` |
| Minimapa | `game/Minimap` (zoom, puntos, aviso, reloj), `game/MinimapData` (Lock.dbc, hechizos de rastreo); `net/MinimapMessages` | `app/MinimapView` | `app/MinimapScreens` |
| Mensajes | `game/Notices` (ChatTypeInfo → General / registro de combate / UIErrorsFrame), `game/MirrorTimers`; `net/MirrorTimerMessages` | `ui/MessageFrame`, `app/MessageHud` | `GameScreens::readNotices` |
| Marcos de unidad | `game/UnitText` (nivel, calavera, dificultad, paletas, tooltip), `game/UnitInfo` (`unitNameColor`, `mayFight` = `UnitCanAttack`; `canAttack` = la comprobación estricta del servidor) | `app/UnitFrames`, `app/UnitTooltip` | `app/UnitScreens` |
| Mundo | `app/WorldTap` (qué se puede tocar), `ServerObjects::collectViews`, `WorldObjects::instanceSphere` | `app/WorldLabels`, `app/ChatBubbles` | `HudWorld.cpp`; `app/SelectionCircle` dibuja en el pase de partículas (sin pipeline nueva) |
| Barras de acción | `game/ActionBar` (páginas, barras de forma, uso, alcance como `Spell::CheckRange`), `SpellBook` (enfriamientos de objetos) | `HudActionBar.cpp`, `CombatWindows` (posturas) | `CombatScreens` |
| Auras | `game/WeaponEnchants`, `Auras::canCancelAura` (reglas de `HandleCancelAuraOpcode`), auras del grupo desde `SMSG_PARTY_MEMBER_STATS` | `app/AuraLayout` (posiciones de BuffFrame / TargetFrame, probadas), `app/AuraWindows` | `app/AuraScreens` |
| Ventanas emergentes | `app/PopupStack` (reglas y tabla de StaticPopup, sin dibujo), `game/Logout`, `game/Summon`, `game/BindConfirm`; `net/PopupMessages` | `app/PopupWindows` (no modales, se dibujan las últimas) | `app/PopupScreens` |
| Paneles | `app/UiPanels` (reglas de ShowUIPanel y lugares por perfil, sin dibujo, probadas) | `DialogContext::panel` (el diálogo de siempre, sin bloqueador y del tamaño de su lugar), `app/MicroMenu` | `GameScreensPanels.cpp` (`syncPanels`: todo lo que abre un módulo o el servidor pasa por las reglas) |
| Chat | `game/ChatLinks` (enlaces de objetos y jugadores), `ui/ChatDock` (desvanecido, destellos, anchos de pestaña) | `app/ChatFrame` | `SocialScreens` (con el registro de combate de `CombatScreens` como pestaña) |
| Sonido | `audio/SoundRules` (errores con voz, zancada, aterrizaje), tablas nuevas en `audio/SoundData` | — | `app/GameAudioWorld.cpp` (cuerpo, entorno, agua, fantasma, objetos, game objects, criaturas ociosas) |

- Lo que en el original vive en WoW.exe y no en FrameXML (círculo de selección, bocadillos, placas, retratos,
  puntos del minimapa) se decidió con los datos del cliente y vMaNGOS; cada decisión está anotada en su código.
- Ventanas: paneles (no modales) para todo lo que en 1.12 es un UIPanel; pasos modales solo para menús y editores
  dentro de una ventana; las preguntas son la pila de StaticPopup (`app/PopupScreens`), que se dibuja la última.
- Adaptaciones táctiles comunes: pulsación larga = pasar el ratón por encima (tooltips de unidades, auras, objetos,
  botones) o el menú del clic derecho; segundo toque sobre el objetivo = clic derecho; tocar un objeto del mundo lo
  usa; los tooltips se colocan donde el dedo no los tapa.

### 9.3 Jugabilidad que faltaba (fase 13)

Cinco paquetes paralelos con la misma regla (estado en `game/`, protocolo en `net/`, vistas puras, pegamento en
`*Screens`) y una pieza común previa: `app/ChatLinkInsert` (con el cuadro del chat abierto, tocar un objeto, una
receta o una misión inserta su enlace, el Mayús + clic del escritorio; las ventanas lo encuentran en
`DialogContext::chatLink`).

| Área | Estado / reglas | Vistas | Pegamento |
|---|---|---|---|
| Hechizos con objetivo | `game/SpellTargeting` (esperar un lugar o un objeto, las preguntas BIND / REPLACE / TRADE_REPLACE_ENCHANT con las reglas de WoW.exe), `game/SpellMods` (modificadores planos y porcentuales por bit de familia); `net/SpellMessages` | `app/TargetingWindows` (barra Lanzar / Cancelar), círculo con `SelectionCircle` (`setCells`, sin pipeline nueva) | `app/SpellTargetingScreens`; `Game::groundAlongRay` |
| Control del movimiento | `game/MoverControl` (`SMSG_CLIENT_CONTROL_UPDATE`, `PLAYER_FARSIGHT`, orden NOT_ACTIVE_MOVER → SET_ACTIVE_MOVER → SPLINE_DONE, tiempo saltado); `net/ControlMessages` | `app/StatusFlash` (OutOfControl / LowHealth de UIParent) | `PlayerController::switchMover` / `followUnit`, `GameSession` (órdenes al mover activo) |
| Registro de combate | `game/Combat` + `game/CombatText` (líneas de `UnitCombatLog_C`); `net/SpellLogMessages` | registro de `CombatScreens` | `FxEventKind::ChainTarget` para F14.4 |
| Oficios | `game/TradeSkillData` (tablas al cargar), `game/TradeSkills` (listas, cabeceras, colores, Crear todo con el relanzado de WoW.exe) | `app/TradeSkillWindows` | `app/TradeSkillScreens`; `UiPanel::TradeSkill` / `Craft` |
| Misiones | `game/QuestJournal` (orden de WoW.exe, cabeceras, 5 seguidas, AUTO_QUEST_WATCH, temporizadores), `game/QuestData`; `net/QuestMessages` | `app/QuestWindows` (QuestLogFrame, QuestWatchFrame, QuestTimerFrame) | `app/QuestScreens` |
| Objetos vivos | `game/GameObjectAnim` (estado → secuencia, Custom0..3, Spawn / Despawn, sonidos de GameObjectDisplayInfo) | modelos de `actors.vdpk` | `app/ServerObjects` (actores para los que tienen animación; colisión de puertas cerradas en el hilo `vd-doors`) |
| Menús y órdenes | `game/UnitPopup` (UnitPopup.lua como reglas puras), `game/PlayerCommands` (iconos de banda, inspeccionar, /played, /pvp, hermandad, moderación de canales); `net/PlayerCommandMessages` | `app/UnitMenuWindows` (UIDropDownMenu, iconos) | `app/UnitMenuScreens` (seguir, inspeccionar, `UiPanel::Inspect`) |
| Mensajes sueltos | `net/MiscMessages`; `PlayerStanding` en `game/Reactions` (reacciones forzadas, guerra con facciones); punto de los guardias en `game/Minimap` | minimapa (icono y flecha guía) | `GameScreens::readEvents`, `GameAudio` (alerta / aggro) |

### 9.4 Interfaz que faltaba (fase 15)

Cinco paquetes paralelos con la misma regla y una pieza común previa, los **marcos de modelo 3D** de la interfaz
(PlayerModel / DressUpModel / TabardModel): `app/ModelViews` (actores fuera de escena, `Actors::setOffscreen`, posados
cada frame con `appendPosed` y dibujados por `render/ModelViewRenderer` en imágenes de 512 × 640 con fondo
transparente, compatibles con el pase principal como los retratos; encuadre en `world/PortraitCache`:
`modelViewCamera`) y el widget `app/ModelFrame` (botones de girar de ModelFrames.lua y arrastre).

| Área | Estado / reglas | Vistas | Pegamento |
|---|---|---|---|
| Bolsas y objetos | `game/ItemCompare`, `game/TryOn` (también el «aspecto del jugador con estas piezas»), `game/MasterLoot` | `app/ContainerLayout` + `ContainerWindows` (una ContainerFrame por bolsa, `updateContainerFrameAnchors`), `CompareTooltips`, `TooltipLayout`, `DressUpWindows` | `InventoryScreens` (+ `InventoryContainers.cpp`, `InventoryCompare.cpp`), `DressUpScreens`, `ExchangeScreens` (GroupLootDropDown) |
| Personaje | `game/CharacterSheet` (honor, facción vigilada, inactiva, olvidar), `game/PaperDoll` (las cifras y tooltips de PaperDollFrame.lua), `game/FactionList`, `game/SkillList` (listas de WoW.exe), `game/CharacterData`; `net/CharacterMessages` | `app/CharacterWindows` (PaperDollFrame, HonorFrame, ReputationFrame, SkillFrame, InspectFrame, ReputationWatchBar) | `app/CharacterScreens`; pestañas `InventoryScreens::registerCharacterTab` |
| Mascotas | `game/PetStable`, `game/PetData`, `game/CombatFeedback`; libro y barra de la mascota en `game/Abilities`; `net/PetMessages` | `app/PetWindows` (PetStableFrame, PetPaperDollFrame, «Group N», HitIndicator), `partyPetFrame` | `app/PetScreens` |
| Hermandad y ayuda | `game/GuildControl`, `game/Petitions`, `game/TabardDesigner`, `game/GuildEmblem` (el emblema compuesto en `AppearanceDb`), `game/HelpDesk`, `game/HelpData`; `net/GuildMessages`, `net/HelpMessages` | `app/GuildWindows`, `app/HelpWindows` (HelpFrame, TicketStatusFrame, GMSurveyFrame) | `app/GuildScreens`, `app/HelpScreens`, `app/PlayerLook` |
| Opciones y macros | `game/Macros` (36 macros, ficheros del cliente escritos en `vd-monitor`), `game/GameplayOptions` (casco, capa, barras, bloquear comercio, autolanzamiento) | `app/OptionsWindows` (UIOptionsFrame, SoundOptionsFrame), `app/MacroWindows`, `app/CombatTextScreens` (Blizzard_CombatText) | `app/OptionsScreens`, `app/MacroScreens`; barras extra en `app/Hud*` |
| Mapas (15.10) | `game/WorldMapData` (nivel World con `WorldMapContinent`) | `TravelWindows` (iconos del grupo y sus nombres) | `TravelScreens`, `MinimapScreens::interiorTiles` (minimapa del edificio) |

`Game` y su estado son grandes (ventanas con arrays fijos para no reservar memoria por frame): el ejecutable de
escritorio y el de Android lo crean en el montón (en la pila desbordaba el 1 MB del hilo).

### 9.5 Bandas (fase 16)

Tres paquetes paralelos sobre una base común en `game::Social`: la lista de la banda como `GetRaidRosterInfo`
(`raidMembers`: el jugador incluido, al final, con subgrupo, rango y clase; la clase de los que no se ven sale de la
consulta de nombre, `QueryCache::playerInfo`), `isRaidLeader` / `isRaidOfficer`, mover e intercambiar subgrupos, y
el «grupo» de 1.12 dentro de una banda (`partyMembers`: los de tu subgrupo, como las casillas party1..4 de WoW.exe;
`groupMembers`: todos los demás).

| Área | Estado / reglas | Vistas | Pegamento |
|---|---|---|---|
| Pestaña de banda (16.1) | `game/RaidRoster` (casillas de RaidGroupFrame_Update, soltar = mover o intercambiar, RAID_CLASS_COLORS, isDead) | `app/RaidWindows` (RaidFrame: descripción, 8 grupos de 5, botones) | `app/RaidScreens` (dentro de `SocialScreens`, pestaña RAID) |
| Marcos sueltos (16.1, 16.3) | `game/RaidAuras` (los filtros de UnitBuff / UnitDebuff: visibles, lanzables, disipables) | `app/RaidPulloutWindows`, `app/RaidPulloutLayout` (geometría de Blizzard_RaidUI.xml, colores, sitio libre de los mandos) | `app/RaidPullouts` (de `GameScreens`; los abre la pestaña al arrastrar un grupo o una clase fuera) |
| Avisos (16.2, 16.4) | `game/ReadyCheck` (DoReadyCheck, respuestas, CheckReadyCheckTime), `game/MeetingStone` (la cola y sus líneas), `game/RaidAlertLines`; `net/MeetingStoneMessages` | `app/RaidAlertWindows` (ReadyCheckFrame), `app/ObjectTooltip` (tooltip de un objeto del mundo) | `app/RaidAlertScreens` (RaidWarningFrame y RaidBossEmoteFrame del anillo del chat, la ventana de listos con el retrato del líder), botón MiniMapMeetingStoneFrame en `MinimapScreens` |

### 9.6 JcJ (fase 17)

Tres paquetes paralelos sobre una base común en `game/`: `game::Battlegrounds` (las tres colas como las guarda
WoW.exe: estado none / queued / confirm / active, mapa, instancia, espera, caducidad de la invitación, la cola activa
y los tiempos de la instancia; `AcceptBattlefieldPort`, `LeaveBattlefield`, el ganador y el final de la partida) y
`game::WorldStates` (los estados del mundo del mapa y la zona: `SMSG_INIT_WORLD_STATES` sustituye el conjunto,
`SMSG_UPDATE_WORLD_STATE` cambia uno), ambos de `Gameplay`, con `net/BattlegroundMessages`.

| Área | Estado / reglas | Vistas | Pegamento |
|---|---|---|---|
| Cola (17.1) | `game/BattlefieldQueue` (`BattlefieldQueueing`: la lista del maestro de batalla o del portal, apuntarse solo o en grupo, alejarse cierra la lista, sonidos e invitaciones de cada estado, avisos de cierre de la instancia), `game/BattlefieldLines`; `net/BattlefieldListMessages`; Map.dbc ampliado en `AreaData` (niveles, descripciones por bando, tramo) | `app/BattlefieldWindows` (BattlefieldFrame, el botón MiniMapBattlefieldFrame), `app/BattlefieldText` (su tooltip y su menú) | `app/BattlefieldScreens` (de `GameScreens`; panel `UiPanel::Battlefield`, ventana CONFIRM_BATTLEFIELD_ENTRY, una por cola) |
| Dentro (17.2) | `game/WorldStateUi` (las filas de WorldStateUI.dbc que se ven, con su texto y estado; las columnas de la puntuación), `game/BattlefieldScore` (`MSG_PVP_LOG_DATA` cada 5 s, orden y bandos, posiciones de los portadores de bandera, entradas y salidas); `net/PvpLogMessages` | `app/WorldStateWindows` (WorldStateAlwaysUpFrame, WorldStateCaptureBar, WorldStateScoreFrame, BattlefieldMinimap) | `app/WorldStateScreens` (de `GameScreens`; panel `UiPanel::WorldStateScore` sobre las dos casillas); el mapa del mundo muestra a los del campo y la bandera |
| Honor y cementerios (17.3, 17.4) | `game/AreaSpiritHealer` (el guía espiritual del fantasma: consulta, cola, cuenta atrás), `game/Honor` (muertes con honor, insignia, LocalDefense / WorldDefense), `game/GameplayPvp` (quitar la insignia), `game/PvpLines`; `net/PvpMessages` | ventana AREA_SPIRIT_HEAL en `PopupStack` | `app/PvpScreens` («HK: rango» sobre el jugador, «Honor +N» en el texto de combate flotante) |

### 9.7 Pantallas de acceso y cierre (fase 18)

Las pantallas antes del mundo son las del GlueXML del 1.12 sobre su **escena 3D**: `app/GlueScene.h` define lo que
piden (`GlueSceneRequest`: el fondo, `GlueBackground`, de `Interface\Glues\Models`; el personaje de pie y su giro) y
`GameScreens::drawLogin` lo rellena cada frame; `app/GlueStage` (del `Game`) lo dibuja en lugar del mundo: el modelo
de fondo como actor (su secuencia 0 con huesos, partículas y UV animadas), la cámara 0 del modelo
(`world/CinematicCamera` sobre el blob de `cinematics.vdpk`), la niebla de GlueParent y sus luces reducidas a la clave
y el ambiente del renderer, y el personaje en el punto de enganche 0 (`CharacterPreview`). La escena está a 4.000 yd de
altura para que nada del mapa abierto la ilumine ni la tape; mientras se ve, el terreno no se actualiza. En pantallas
más anchas que 16:9 se conserva el ancho de 16:9 y se pierde alto (un cuarto arriba, el resto abajo); en las más
estrechas que 4:3, el alto de 4:3. La música (`wow_main_theme.mp3`) la pone `GameAudio` mientras `login`.

| Área | Estado / reglas | Vistas | Pegamento |
|---|---|---|---|
| Acceso, reinos, personajes (18.4) | `app/GlueModel` (versión, diálogo de estado de cada paso, carga y orden de los reinos, qué personaje se elige, `WTF/Config.wtf`); `net/RealmListMessages`, `net/CharRenameMessages`; `NetClient` con `ChoosingRealm` y «Change Realm»; `GameSession` (reinos, renombrar, Warden) | `app/GlueWindows` (AccountLogin, GlueDialog, RealmList, CharacterSelect, borrar y renombrar; `GlueLayout`: las unidades de 768 del XML a dp) | `app/GlueScreens` (pantalla actual, fundidos de GlueParent, sonidos, la escena) |
| Creación | `game/CharCreate` (`selectRace`, `selectGender`, `classesOf`, `NameGenerator` de NameGen.dbc) | `app/CharCreateLayout` (dónde va cada marco de CharacterCreate.xml en cada pantalla, ≥ 44 dp), `app/CharCreateWindows` | `app/CharCreateScreens` (de `SocialScreens`: `startCreation`, `creating`) |
| Movimiento de otros (18.5, 18.6) | `game/UnitMotion` (modos, animación por velocidad como 0x5FD100, nado de criaturas); `game::EntityStore` con **máscara de cambios por frame** (`nextFrame`, `changed()`, `changedWords`: un bit por palabra de 32 campos) y el **resolutor de transportes** (`positionAt` siempre en el mundo; `localAt` en el marco propio; `Transports::placementAt` al milisegundo); `net/SplineMessages` | — | `app/Units` (relee aspecto y fundidos solo si cambian sus campos: −28 a −32 % de CPU), `app/FarSightView` (la cámara de Visión lejana / Ojo de águila), `app/MotionDemo` |
| Rendimiento (18.6) | `world/TerrainMapLoader` (hilo `vd-map`: abrir y validar el pack, horizonte, disposición y copia de las texturas a un bloque de staging; `Terrain` pasa por Closed → Preparing → Filling → Open y el mapa viejo se descarga con 2 ms por frame), `world/WmoBsp` (grupo de la cámara y portales como WoW.exe), `CollisionWorld::beginOpenMap` (índice en un hilo) | — | `Game::openMap` nunca bloquea; `Renderer::applyQuality` (anisótropo y muestras en un punto seguro, con la `VkPipelineCache`), `render/vk/WorldPass` (pases compatibles con MSAA transitorio resuelto en el tile), `app/MapTravelTest` (`VD_WORLD_TRAVEL`) |
| P3 (18.5) | `game/Tutorials`, `game/AccountData` (tipos 4, 5, 7 con la regla MD5 de WoW.exe), `game/ChatWindows` (`chat-cache.txt`), `game/ListSort` (`SortWho`, `SortAuctionItems`); `net/AccountDataMessages` | `app/TutorialWindows`, `app/SortHeaders` (cabeceras que ordenan) | `app/TutorialScreens`, `app/ChatConfigScreens` (menú de la pestaña del chat), `app/AccountDataFiles` |

### 9.8 Controles móviles (fase 20)

El **racimo táctil** sustituye a las barras del 1.12 en el HUD: botones redondos alrededor del pulgar derecho, con
plantillas por personaje, capas por postura, forma o sigilo y huecos que deciden solos. La opción de interfaz «Botones
de acción táctiles» lo apaga (vuelven las barras clásicas); «Disposición para zurdos» lo refleja. El diseño, sus reglas y
lo que cambió al implementarlo están en `docs/MOBILE_CONTROLS.md`. Las 120 casillas del servidor siguen siendo las
barras del 1.12 (el cajón y el modo clásico); el racimo es del cliente (`touch-layout.txt` por personaje, copia en los
datos de cuenta tipo 6).

| Pieza | Qué | Dónde |
|---|---|---|
| Modelo | plantillas (6), geometría en mm por perfil (cuadrado / ancho), capas por contexto que heredan de la base, grupos, el fichero `VDTOUCH 1` | `game/TouchLayout`, `game/TouchLayoutEdit` (las ediciones, puras), `game/TouchContext` (qué capa) |
| Rangos | la línea de un hechizo por su nombre: siempre el rango más alto conocido salvo «rango fijo» | `game/SpellLines` |
| Presets | 9 clases, 16 plantillas por papel, candidatos por hueco (el primero conocido que nadie cogió), grupos automáticos, «Nuevo» al aprender | `game/ClassPresets`, `game/ClassPresetData` |
| Usabilidad | sigilo, reactivos y tótems, objeto equipado, fuera de combate, objetivos pacíficos, tipo de criatura, munición (también las barras clásicas) | `game/ActionBar` |
| Huecos inteligentes | consumibles por su hechizo de uso, montura o forma de viaje, botón reactivo (estados de aura), resucitar / despellejar / recoger, siguiente objetivo sin romper controles | `game/Consumables`, `game/Mounts`, `game/ReactiveSpells`, `app/TouchSmart` |
| Dibujo y gestos | `ui::Context::widgetCircle` (toques redondos, el más cercano), `UiMode::IconCircle` y `UiMode::Ring` (icono redondo, barrido radial, aros) en el mismo pipeline; abanicos, barra de clase, puntos de combo, glifos propios | `app/HudTouchBar.cpp` (+ `HudTouch.h`), `app/HudTouchEdit.cpp` |
| Pegamento | la capa del momento, las vistas con el estado de las barras (`CombatScreens::touchView`), lo que se pulsa (`useButton`), repetir, guardar | `app/TouchBarScreens` |
| Ventanas | lista de plantillas, modo edición (posiciones y acciones), selector, cajón (las 6 barras del 1.12), tooltips, colocar desde el libro | `app/TouchEditScreens`, `app/TouchEditWindows` |
| Persistencia | `WTF\Account\<CUENTA>\<reino>\<personaje>\touch-layout.txt` y los datos de cuenta tipo 6 (un texto que no empieza por `VDTOUCH` no se adopta) | `app/AccountDataFiles` |
| Arte propio | SVG de un color (`art/touch`) rasterizados por el baker en el atlas de `ui.vdpk` | `art/README.md`, `bake::rasterizeSvgIcon` |
| Densidad real | xdpi / ydpi por JNI (con límites de cordura) → `HudFrame::mm` | `apps/android/native/main_android.cpp`, `Game::onPixelsPerMm` |
| Mascota y control | botón de mascota y su panel, Feed Pet con memoria por familia, acciones de mascota por hechizo, controlar otra unidad, sin control, muerte | `game/TouchPet`, `app/TouchPetScreens`, `app/TouchPetWindows` |
| Asistencias | arrastrar a un marco, apuntar en el suelo, objetivo automático, iniciar el ataque, posturas y formas, repetir, cola, plantilla por sitio, marcos de sanador | `game/TouchAssistRules`, `app/TouchAssists`; `platform/Haptics` (hilo `vd-haptics`) |

### 9.9 Los recursos del 1.12 en la interfaz (retoques de la fase 20)

Regla del proyecto: donde el 1.12 tiene un recurso (textura, modelo, fuente, sonido) se usa el suyo, y fuera lo que el
1.12 no tiene. El arte propio de cada ventana (MerchantFrame, el libro, la subasta...) es una decisión pendiente:
de momento todas comparten `ui::dialogPanel`.

| Pieza | Qué | Dónde |
|---|---|---|
| Modelos de la interfaz | recarga, autolanzamiento y ping del minimapa con sus M2 horneados (`ui/models`), evaluados y dibujados con el pipeline de la UI (sin render 3D por botón); el destello de cada recarga por id de widget | `ui/UiModels`, `pack/UiModelFormat.h`, `tools/baker/UiModels.cpp` |
| Racimo | icono redondo sin aro, con los tintes del 1.12 y el borde verde de equipado; por diseño siguen los nuestros: encoger al pulsar, aro dorado que late mientras está activa, parpadeo rojo del ataque automático y la recarga (barrido oscuro con los segundos, aro fino del GCD) | `app/TouchRoundIcon` |
| Desaturar | `UiMode::AtlasDesaturated` (14) e `IconDesaturated` (15), copia de `Shaders\Pixel\Desaturate.bls` (el RGB del vértice se ignora, como en el cliente): talentos fuera de alcance, correo leído, objetos bloqueados | `shaders/ui.frag`, `DrawList::spriteDesaturated` / `iconDesaturated` |
| Arte de ventana | pestañas de CharacterFrame y del libro, radios, flechas de página, barras de Skills, resalte del comercio, factura de subastas, popups de contra reembolso con dinero | `app/WindowArt`, `app/ExchangeWindows`, `app/PopupScreens` |
| Talentos | ramas y flechas de `UI-TalentBranches` / `UI-TalentArrows` con las reglas de `TalentFrame.lua`, `UI-EmptySlot-White` teñido, `TalentFrame-RankBorder` | `app/TalentBranches`, `app/CombatWindows` |
| Dinero | `ui::money(..., color)`: en rojo lo que no se puede pagar (`SetMoneyFrameColor`) | `ui/Widgets` |
| Icono que carga | `INV_Misc_QuestionMark` (`ui::iconOrPlaceholder`) | `ui/Widgets` |
| Mundo | la sombra de los hechizos de lugar (`Spell-Shadow-*`), placas con BarFill, calavera y Nameplate-Glow, números de daño en FRIZQT; marcas de misión `TalkToMe*.m2` (§2.24) | `app/Game`, `app/WorldLabels`, `app/QuestMarks` |
| Quitado | segundos de recarga, bordes de calidad en las casillas, raya de durabilidad, barra de lanzamiento del objetivo, barrita de vida bajo los nombres | — |

## 10. Especificidades Android / Fold5

- `configChanges` completo en el manifest → plegar/desplegar no reinicia la Activity; solo cambia la
  superficie (`APP_CMD_TERM_WINDOW`/`INIT_WINDOW` o `WINDOW_RESIZED`) → se recrea el swapchain.
- 120 Hz nativos: se fija 60 Hz con Swappy (`swap interval` de 16,6 ms) y `ANativeWindow_setFrameRate(60)`,
  lo que además permite a la pantalla bajar a 60 Hz y ahorrar batería.
- ADPF: sesión de *performance hint* con la duración real de cada frame (F3) y de su GPU (F10, Android 15+);
  *thermal headroom* y estado térmico → calidad dinámica (F10, ver §3). El Fold5 (Android 16) da los umbrales de
  cada estado (`AThermal_getThermalHeadroomThresholds`): ligero 0,83, moderado 0,90, severo 1,00.
- Samsung Game Optimizing Service puede limitar el rendimiento; declarar `appCategory="game"` y medir con y
  sin él.
- Builds `release` firmadas con la clave debug (uso personal), con `<profileable android:shell="true"/>`
  para poder perfilar con AGI/Perfetto/simpleperf sin ser `debuggable`.
