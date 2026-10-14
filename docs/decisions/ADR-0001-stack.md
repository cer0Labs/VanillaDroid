# ADR-0001 — Lenguaje y stack: C++20 + Vulkan + NDK, motor propio

- **Estado:** aceptada
- **Contexto:** queremos un cliente WoW 1.12.1 para Android, con interfaz y jugabilidad adaptadas al móvil,
  que alcance **60 fps sostenidos** en un Galaxy Z Fold5. Se empieza desde cero (no se reutiliza ningún
  cliente existente).

## Decisión

| Capa | Elección |
|---|---|
| Lenguaje del motor | **C++20** (NDK r28, Clang/libc++) |
| API gráfica | **Vulkan 1.1** como base (el Adreno 740 soporta 1.3) |
| Entrada Android | **GameActivity** (AGDK), Java mínimo |
| Frame pacing / térmica | **Swappy** + **ADPF** (Performance Hint API, Thermal API) |
| Assets | **Conversión offline en PC** (baker) a un formato propio optimizado para móvil |
| Plataforma de desarrollo | Windows + Vulkan (mismo motor) para iterar; medición siempre en el dispositivo |

## Por qué C++

1. **Control total de memoria y latencia**: sin recolector de basura → sin pausas imprevisibles. En un
   presupuesto de 16,6 ms, una pausa de GC de 5–10 ms ya es un frame perdido.
2. **Acceso directo a NDK/AGDK**: GameActivity, Swappy, ADPF, AAudio y los perfiladores (AGI, Snapdragon
   Profiler, simpleperf, Perfetto) están pensados para código nativo C/C++.
3. **Ecosistema del dominio**: StormLib (MPQ), astc-encoder, VMA, volk, Tracy, Dear ImGui, miniaudio… todo
   C/C++. La documentación de formatos de WoW (wowdev.wiki) y el propio servidor (vMaNGOS) también son C++.
   Compartir estructuras con el código del servidor simplifica mucho el protocolo.
4. **SIMD (NEON)** para culling, animación y partículas sin capas intermedias.

## Alternativas descartadas

| Alternativa | Motivo del descarte |
|---|---|
| **Rust** (ash/wgpu) | Rendimiento equivalente y más seguro, pero el ecosistema del dominio (MPQ, formatos WoW, AGDK) es C/C++ y habría que envolverlo todo con FFI. Segunda opción válida si algún día se reescribe. |
| **Kotlin/Java** | GC, coste de JNI, sin control de SIMD ni de layout de memoria. Solo para la Activity. |
| **Unity / Unreal / Godot** | Habría que convertir todos los formatos de WoW a los del motor y pelear contra su streaming, su UI y su overhead (C# GC en Unity, peso de Unreal en móvil). Un motor genérico no conoce las particularidades de WoW (terreno ADT, portales WMO, M2 vanilla) que son justo donde está el rendimiento. |
| **OpenGL ES 3.x** | Driver más pesado en CPU (validación implícita, estado global), sin control de load/store ops en GPUs de tiles, compilación de shaders impredecible. Vulkan permite precrear pipelines y controlar el ancho de banda. |
| **Emular el WoW.exe original** (Box64/Wine) | Traducción x86→ARM y D3D/OpenGL→Vulkan; no permite rediseñar la interfaz ni el control táctil de verdad. |
| **Leer MPQ en el dispositivo en tiempo real** | Descomprimir MPQ, decodificar BLP (DXT, que muchas GPUs móviles no soportan) y parsear M2/WMO en caliente cuesta CPU, memoria y provoca tirones. Se hace una vez en PC. |

## Consecuencias

- Hay que mantener un **baker** en PC y un **formato de pack** versionado; cambiar el formato obliga a
  re-hornear (aceptable: es local).
- Más código propio que con un motor comercial, pero cada milisegundo es controlable y perfilable.
- El proyecto compila para Windows y Android desde el mismo CMake; Gradle solo empaqueta.
