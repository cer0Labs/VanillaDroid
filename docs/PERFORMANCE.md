# Rendimiento: presupuestos, reglas y medición

Objetivo: **60 fps sostenidos** (16,6 ms) en Galaxy Z Fold5, pantalla interior (2176×1812), build `release`.
Adreno 740 · CPU 1×X3 + 2×A715 + 2×A710 + 3×A510 · 12 GB RAM.

## 1. Presupuestos por frame

| Métrica | Presupuesto | Notas |
|---|---|---|
| Frame (p99) | ≤ 16,6 ms | Ningún frame > 33 ms en 5 min de benchmark |
| CPU hilo principal | ≤ 6 ms | Deja margen para picos (red, streaming) |
| CPU workers | ≤ 8 ms de trabajo total repartido | Culling, animación, partículas |
| GPU | ≤ 11 ms | ~35 % de margen para throttling térmico |
| Draw calls | ≤ 1000 (objetivo 500) | Terreno 1/tile, doodads instanciados |
| Cambios de pipeline | ≤ 100 | Ordenar por pipeline → material |
| Triángulos | ≤ 1 M | Vanilla es low-poly: si se supera, algo no está haciendo culling/LOD |
| Memoria total (CPU + GPU) | ≤ 2 GB | Android mata antes a las apps grandes en segundo plano |
| Tirón al cruzar tile | 0 frames > 20 ms | El streaming nunca bloquea al hilo principal |
| Login → mundo | ≤ 5 s | Pack en almacenamiento interno |
| UI táctil (CPU) | ≤ 1,5 ms | Fase 7, medido en el Mi 11X en combate en línea: 0,30 ms de media (máx. 0,74), ~600 quads en 3 draws |
| Mundo vivo (F14) | ≤ 0,5 ms de CPU y ≤ 0,5 ms de GPU sumando todo | Mi 11X: +0,1–0,35 ms de CPU y +0,02–0,2 ms de GPU en las rutas de ciudad, campo y combate. Hierba ≤ 850 matas / 5k triángulos / 14 draws (6 chunks colocados por frame, 0,5 ms); doodads animados una pose por modelo y fase (≤ 11 en Ventormenta); manchas del suelo +1–4 draws; haces ≤ 100 quads; audio 0,008 ms |
| Sesión sostenida | 30 min a 60 fps sin bajar calidad visible | F10, Fold5: 30 min en Orgrimmar con el combate de 40 a 60,0 fps, p99 17,5 ms, headroom térmico ≤ 0,67 (la calidad dinámica no tuvo que actuar); batería ~10 %/h (423 mAh/h, 1,6 W). `scripts/soak.ps1` |

## 2. Escenas de benchmark (reproducibles)

Recorridos de cámara grabados (posición + orientación por tiempo) que se reproducen igual cada vez:

1. **Orgrimmar** (`orgrimmar`) — puertas → Valle de la Fuerza → Calle Mayor → Valle del Honor → Valle de la
   Sabiduría → Bastión Grito Infernal → Valle de los Espíritus (WMO + terreno, a pie de calle).
2. **Ventormenta** (`stormwind`) — puente → Distrito de Mercaderes → Barrio de los Magos → Parque → Plaza de la
   Catedral → Distrito de los Enanos → Castillo → Casco Antiguo (un WMO de 306 grupos y 341 portales).
3. **Forjaz** (`ironforge`) — puertas → la Explanada → Barrio Místico → Sala de los Exploradores → Ciudad
   Manitas → Barrio Militar → la Gran Fundición (interiores bajo la montaña; la cámara ignora el terreno).
4. **Vega de Tuercespina** (`stranglethorn`) — Campamento Rebelde → Nesingwary → Grom'gol → Bahía del Botín
   (vegetación densa: alpha test y overdraw). Elwynn (`elwynn`) cubre el bosque templado.
5. **Vuelo largo** (`barrens`, `durotar`) — Barrens norte→sur a velocidad de montura: streaming.
6. **Multitud** (`crowd`) — 40 personajes sintéticos de todas las razas, con equipo de PNJ, frente al banco de
   Orgrimmar (unos andan o corren en círculo, algunos montados, otros quietos, bailan o se sientan) y la cámara
   dándoles la vuelta a 26 yd a pie de calle (65 s). `crowd0` es la misma vuelta sin personajes: la diferencia es
   lo que cuestan los actores.

7. **A pie** (fase 6: `walk_goldshire`, `walk_stormwind`, `walk_orgrimmar`, `walk_gorlach`) — no es la cámara la que
   vuela: el personaje camina con su controlador sobre la colisión horneada, siguiendo patrullas de PNJ de la base
   de datos de vMaNGOS (Villadorada → puerta de Ventormenta → Valle de los Héroes, 710 yd; la ronda de los emisarios
   por Ventormenta, 2.250 yd; las rondas del emisario, 2.480 yd, y de Gorlach, 2.080 yd, por Orgrimmar). Además de
   los frames informa de puntos alcanzados, atascos, caídas, la diferencia entre los pies y la altura del servidor
   en cada punto, y el coste del controlador (colisión en streaming + física + paquetes).

8. **Sesión larga** (`soak`, fase 10) — el recorrido de Orgrimmar y la vuelta al combate de 40 frente al banco (que
   sigue luchando mientras la cámara recorre la ciudad), uno tras otro durante 30 min (`benchminutes`). Cada minuto
   una línea `SOAK minute N` con fps, p50/p99/máx., frames lentos, GPU y CPU, lo que hizo la calidad dinámica
   (escala, distancia, efectos, niveles térmico y de carga), el headroom y el estado térmicos y la batería
   (nivel, corriente, vatios, temperatura); al final `SOAK done` con el consumo por hora. `scripts/soak.ps1` no toca
   el móvil mientras dura: el juego escribe el informe en `files/packs/bench_soak.txt`.

Las rutas de ciudad van a 14 yd/s (montura épica) con la cámara a 6 yd sobre el suelo de `game_tele` de vMaNGOS
(tercera persona); las de campo, a 28 yd/s y 30 yd (12 en Tuercespina). Se lanzan con `scripts/bench.ps1 -Route`
en el móvil y con `--bench <ruta> --bench-exit` en escritorio.

Cada fase que toca render/mundo publica: frametime medio, p99, GPU ms, draw calls, triángulos, memoria.

## 3. Reglas para GPU de tiles (Adreno/Mali)

1. **Load/store ops**: `CLEAR` o `DONT_CARE` al empezar; `DONT_CARE` en depth al terminar. Un `LOAD`/`STORE`
   innecesario sobre 2176×1812 cuesta ancho de banda real (~16 MB por adjunto y frame).
2. **Adjuntos transitorios** (`TRANSIENT_ATTACHMENT` + `LAZILY_ALLOCATED`) para depth y MSAA.
3. **No partir el render pass** sin necesidad: cada pase extra = flush de tiles a memoria.
4. **Nada de lecturas de depth/color del frame actual** en pases posteriores (sin SSAO, sin deferred).
5. **`discard` con moderación**: desactiva optimizaciones de ocultación (LRZ). Shader de alpha-test mínimo,
   dibujar opacos primero.
6. **FP16**: `mediump` en fragment shaders; los cálculos de color no necesitan 32 bits.
7. **Texturas ASTC con mips siempre**; nunca RGBA8 sin comprimir salvo render targets.
8. **Overdraw**: ordenar opacos de delante a atrás; limitar partículas grandes a pantalla completa.
9. **Sin barreras innecesarias**; sincronizar con subpass dependencies, no con `vkQueueWaitIdle`.
10. **Filtrado anisotrópico con cuidado**: en el terreno (4 capas ASTC por píxel) costaba 5,6 ms a 4× y 2,1 ms a 2×
    en un Adreno 650. Desactivado; si vuelve, será como ajuste de calidad.
    Fase 18: el anisótropo (1–16×) y el MSAA (1–8 muestras, color y profundidad transitorios resueltos en el tile, con
    alfa a cobertura en las partes con prueba de alfa de modelos, actores y doodads) son opciones de vídeo que solo
    activa Ultra (4× y 4 muestras). Escritorio: +0,03–0,04 ms el anisótropo 16×, +0,09–0,21 ms 4 muestras.
11. **Cada draw cuesta** aunque dibuje poco: en el Adreno 650 los actores salían a ~7–10 µs de GPU por draw (el
    trabajo de un draw se repite en cada bin). Empaquetar las partes visibles de un personaje por material bajó la
    multitud de 814 a 636 draws y la GPU de 8,7 a 7,4 ms; agrupar por pipeline (opacos y luego alpha-test) evita
    cambiarla en cada actor.
12. **En el Adreno 740 nuestras escenas las limita la geometría, no los píxeles** (F10, medido en el Fold5 con la
    frecuencia de `kgsl`): el combate de 40 (`fight`, ~830 draws, 600k triángulos) deja la GPU a 220–270 MHz de 719
    con un 32–45 % de carga a escala 1, 0,7 y 0,5 por igual (frecuencia × carga: 93, 99 y 87), y el preset Bajo
    tampoco la mueve: ya está en sus escalones más bajos. Lo que cuesta es procesar vértices y repartirlos por tiles;
    la resolución del mundo es la válvula para GPUs cortas de relleno y para el calor, no para esta escena.
13. **Los efectos a pantalla completa del 1.12 van dentro de cada shader** (F11, `shaders/screenfx.glsl`): el
    `ffxGlow` y el `ffxDeath` del cliente son pases sobre la imagen entera; aquí son 2–8 operaciones al final de cada
    fragment shader del mundo (+0,05 ms de GPU en la GTX 1080), sin vaciar los tiles. El clima no usa búferes de
    vértices (2 draws, ~37k vértices con la lluvia más fuerte; ~0,03 ms de CPU por frame), el cielo 4–7 draws (~8 µs
    de CPU) y la luz de interiores 8 B más por instancia (`ModelInstanceData` 56 B) y push constants de 112 B, sin
    draws nuevos. Fold5: Ventormenta con la lluvia más fuerte a 60,0 fps (p99 17,71 ms, GPU 6,65 frente a 6,58 ms
    sin lluvia); la tormenta de arena dobla el trabajo de la GPU (MHz × carga 74 → 148 en los Baldíos) por el relleno
    de sus granos y nieblas translúcidos, sin bajar de 60 fps (fase 11). Los sprites translúcidos cerca de la
    cámara se limitan en tamaño de pantalla, como los *point sprites* del cliente.

## 4. Reglas de CPU

1. Cero allocations por frame en el camino caliente (arena de frame que se resetea cada frame).
1b. **Localidad de datos por encima de las cuentas**: muestrear un esqueleto de 119 huesos cuesta 6 µs con la caché
    caliente (PC), pero 51 µs en el móvil cuando sus claves están repartidas por un blob de 2–3 MB; con las claves
    de cada secuencia contiguas (formato de actores v2) bajó a ~30 µs.
1c. **Workers solo si el sistema los pone en núcleos rápidos**: sin ADPF (Mi 11X) las ráfagas cortas de los workers
    iban a núcleos pequeños a baja frecuencia y el trabajo en paralelo tardaba más que en el hilo principal;
    fijarlos a los núcleos grandes junto al hilo de juego fue peor aún (le quitaban el núcleo al despertarlos).
1d. **Física por frame acotada**: el controlador recoge una vez por frame los triángulos de la caja que puede
    barrer (terreno reconstruido solo en los quads que toca + BVH de objetos) en un `TriangleSet` de capacidad fija
    y hace todos sus barridos contra esa lista (decenas de triángulos). Medido en el Mi 11X caminando por
    Orgrimmar: 0,035 ms de media por frame, 0,7 ms el peor (con la carga de un tile nuevo).
1e. **Nunca escribir disperso en memoria mapeada de la GPU**: construir los quads de partículas directamente en el
    búfer de vértices mapeado era ~30× más lento (0,74 → 0,023 ms en el PC) que construirlos en memoria normal y
    copiarlos con un solo `memcpy`. Igual que la lección de F2 con la memoria host-visible del Adreno.
2. Datos SoA contiguos para culling/animación; evitar punteros y `virtual` en bucles grandes.
3. Culling jerárquico (tile → chunk → objeto) con NEON; bounding boxes precalculadas en el baker.
4. Animación a frecuencia reducida según distancia (60/30/15 Hz) y congelada fuera de pantalla.
5. Nada de `std::string`/`std::map` en caminos calientes; IDs enteros y tablas hash abiertas.
6. Sin mutex en el camino caliente; colas sin bloqueo entre hilos.
7. Nada de IO síncrona en el hilo principal (ni siquiera `stat`).

## 5. Cómo medir

| Herramienta | Para qué |
|---|---|
| **Overlay propio** (ImGui) | Frametime, CPU (frame − tiempo bloqueado en fence/acquire/present), GPU (timestamps), draw calls, gráfica de 2 s. Solo desarrollo: al rasterizar glifos nuevos el backend de ImGui hace `vkQueueWaitIdle` |
| **Tracy** | Zonas de CPU por hilo; conexión por `adb forward tcp:8086 tcp:8086` |
| **Android GPU Inspector (AGI)** | Perfil de frame Vulkan y contadores del Adreno (binning, LRZ, ancho de banda) |
| **Perfetto** (`ui.perfetto.dev`) | Planificación de CPU, frecuencias, térmica, llamadas al sistema |
| **Snapdragon Profiler** | Contadores detallados del Adreno 740 |
| **simpleperf** | Muestreo de CPU nativo (requiere `profileable`, ya activado) |
| **kgsl** (`adb shell`) | Frecuencia y carga de la GPU: `/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq` y `gpu_busy_percentage` (el shell puede leerlos, la app no) |
| **`scripts/soak.ps1`** | Sesión de 30 min: rendimiento, temperatura, calidad dinámica y batería por minuto, CSV en `build\soak` |

**Ojo con el tiempo de GPU por timestamps**: con DVFS no mide carga. Con menos trabajo la GPU baja de frecuencia y
tarda lo mismo (el combate de 40 marcó 6,5–7,3 ms a escala 1, 0,85, 0,7 y 0,5): para comparar coste de GPU, la
frecuencia por la carga, o el consumo. Por eso la calidad dinámica no baja la resolución por "GPU alta", sino por
frames que de verdad llegan tarde con la GPU como cuello.

Procedimiento: build `release` → dispositivo al 100 % de batería o cargando, pantalla interior,
brillo fijo, 2 min de calentamiento, luego el recorrido de benchmark. Anotar resultado en el PR/commit.
Batería y temperatura (F10): `scripts/soak.ps1` con el móvil **desenchufado** (ADB inalámbrico), pantalla interior,
brillo como se juega (el script lo anota), sin tocarlo; los benchmarks no leen los ajustes guardados (calidad Alta
por defecto, con la calidad dinámica activa).

Recorridos grabados (`engine/src/app/Benchmark.cpp`): la cámara vuela waypoints fijos a 28 yd/s, 30 yd sobre
el suelo, tras 5 s de calentamiento, y al final escribe una línea `BENCH` (frames, fps, p50/p99/máx, frames
> 20 ms y > 33 ms, GPU, CPU, draws, triángulos):

```powershell
.\scripts\bench.ps1 -Route elwynn      # móvil: elwynn | durotar | barrens | goldshire | stormwind | orgrimmar |
                                       #        ironforge | stranglethorn | crowd | crowd0
.\build\windows\apps\desktop\RelWithDebInfo\vanilladroid_desktop.exe --bench elwynn --bench-exit   # escritorio
```

## 6. Checklist antes de dar por bueno un cambio de render/mundo

- [ ] Medido en `release` en el Fold5 (no en debug, no en escritorio)
- [ ] Sin creación de pipelines ni allocations nuevas por frame (Tracy/overlay)
- [ ] Draw calls y triángulos dentro de presupuesto en las escenas de benchmark
- [ ] Validación Vulkan limpia en escritorio
- [ ] Sin tirones al cruzar tiles
