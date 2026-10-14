# Protocolo 1.12.1 (build 5875) — notas de implementación

> **Fuente de verdad: el código de [vMaNGOS](https://github.com/vmangos/core) (commit `4641790`).**
> Todo lo de aquí está implementado en `engine/src/net` y `engine/src/game` y **verificado contra un vMaNGOS de ese commit**
> (fase 4: login, entrada al mundo y 0 errores de parseo con el tráfico real). Opcodes, update fields y códigos de
> respuesta se **generan** con `scripts/gen-protocol.ps1` (la carpeta de vmangos-core en `-VmangosDir` o
> `VD_VMANGOS_DIR`), nunca a mano.

| Tema | Fichero en vmangos-core | En el cliente |
|---|---|---|
| Login, SRP6, lista de reinos | `src/realmd/AuthSocket.cpp`, `AuthPackets.h`, `AuthCodes.h` | `net/RealmClient` |
| SRP6 y big numbers | `src/shared/Crypto/Authentication/SRP6.cpp`, `Crypto/BigNumber.cpp` | `net/Crypto` |
| Cifrado de cabecera | `src/shared/Auth/AuthCrypt.cpp` | `net/Crypto` (`HeaderCipher`) |
| Sesión de mundo, cabeceras | `src/game/Server/WorldSocket.cpp`, `Handlers/AddonHandler.cpp` | `net/WorldConnection` |
| Opcodes | `src/game/Server/Protocol/Opcodes_1_12_1.h` | `net/Opcodes.gen.*` |
| Update fields | `src/game/Objects/UpdateFields_1_12_1.h` | `net/UpdateFields.gen.*` |
| Códigos de respuesta | `src/game/SharedDefines.h` (`ResponseCodes`) | `net/ResponseCodes.gen.*` |
| Actualizaciones de objetos | `Objects/Object.cpp` (`BuildMovementUpdate`), `Server/Packets/ObjectUpdate.cpp` | `game/Entities` |
| Splines | `src/game/Movement/spline/packet_builder.cpp`, `MoveSplineFlag.h` | `game/Entities` |
| MovementInfo | `Objects/Object.cpp` (`MovementInfo::Write`), `Objects/MovementInfo.h` | `net/Messages` |
| Personajes | `Objects/Player.cpp` (`BuildEnumData`) | `net/Messages` |

## 0. Servidor y requisitos para un cliente propio

Configuración del servidor de desarrollo: **Warden desactivado** (`Warden.WinEnabled = 0`, `Warden.OSXEnabled = 0`;
imprescindible, ver abajo) y `StrictVersionCheck = 0` en realmd. El anticheat de movimiento (`Anticheat.Enable`) puede
estar activado: el movimiento de VanillaDroid pasa sus comprobaciones (validado en la fase 6).

Requisitos que impone el código (no son configurables):

- **Misma IP en realmd y world** (`WorldSocket.cpp:309`): la IP que conecta al mundo debe ser la del último login
  (`account.last_ip`), salvo que sea una IP del propio servidor (127.0.0.1 o las de su nombre de host). El cliente
  hace login y mundo por la misma dirección: si realmd está en loopback (`adb reverse`), usa loopback también para el
  mundo aunque el reino anuncie la IP LAN (`net/NetClient.cpp`).
- **OS `Win` y plataforma `x86`** en `AUTH_LOGON_CHALLENGE`: se guardan en la cuenta y `WorldSocket.cpp:398-418`
  corta la sesión con cualquier otro valor.
- **Fallos de contraseña**: `WrongPass.MaxAttempts = 10` en `WrongPass.ThrottleWindowDurationSec = 60` s por IP;
  después realmd responde `WOW_FAIL_DB_BUSY` ("servidor ocupado") hasta que pasa la ventana.
- `MinRealmListDelay = 1`: pedir la lista de reinos más de una vez por segundo cierra la conexión.
- **Warden** (decidido en la fase 18): con `Warden.WinEnabled = 1` vMaNGOS crea un `WardenWin` para toda sesión que diga
  `Win` (`Anticheat.cpp`, `CreateWardenForInternal`; solo `Win` y `OSX` entran al mundo) y manda `SMSG_WARDEN_DATA`: el
  antitrampas del cliente de Windows, que lee la memoria de ese cliente. VanillaDroid **no lo emula** (no es WoW.exe y
  hacerse pasar por él sería engañar al antitrampas): al recibir `SMSG_WARDEN_DATA` cierra la sesión y dice por qué
  (`VD_LOGIN_WARDEN`: el servidor tiene que desactivarlo para este cliente) en vez de esperar a que el servidor lo eche.
- `mangosd` con `Console.Enable = 1` lee órdenes de su entrada estándar: lanzado sin terminal (desde un script no
  interactivo) recibe fin de fichero y se apaga a los pocos segundos. Desde una consola propia (ventana
  minimizada) funciona.

Conexión desde el móvil en desarrollo (`scripts/android.ps1 -Run -Account … -Password … -Realmd …`):
- **Vía Wi-Fi**: `-Realmd 192.168.1.20:3724` (la IP del PC en la red local). Es lo más parecido al uso real.
- **Vía USB/ADB**: `-Reverse` y `-Realmd 127.0.0.1:3724`.

## 1. Autenticación (realmd, TCP 3724)

Mensajes sin cifrado, little-endian salvo que se indique.

1. **C→S `AUTH_LOGON_CHALLENGE` (0x00)**: `cmd u8`, `error u8` (3), `size u16` (= 30 + longitud de la cuenta),
   `"WoW\0"`, versión `1,12,1` (3 × u8), `build u16 = 5875`, `platform`, `os` y `locale` como 4CC **invertidos**
   (`"68x\0"`, `"niW\0"`, `"SUne"`), `timezone u32`, `ip u32`, `accountLen u8` (≤ 16), cuenta en MAYÚSCULAS.
   Fase 21: el `locale` es el configurado (`"SEse"` para esES): vMaNGOS guarda el `LocaleConstant` de la sesión y
   contesta objetos, criaturas, misiones, diálogos, páginas y sus `mangos_string` en ese idioma donde tiene
   traducción (`locales_*`); sus DBC son enUS, así que los canales de zona se piden con los nombres ingleses
   (`GetChannelEntryFor` busca el patrón en las columnas de idioma que cargó).
2. **S→C**: `cmd`, `u8 0`, `result u8`; si es 0: `B[32]`, `gLen u8 = 1`, `g = 7`, `NLen u8 = 32`, `N[32]`,
   `salt[32]`, `versionChallenge[16]`, `securityFlags u8` (build ≥ 5428; `0x01` = PIN, no soportado).
3. **SRP6** (SHA-1, k = 3), exactamente como `SRP6.cpp`:
   ```
   x  = H(s | H(USER ":" PASS))            (mayúsculas; el cliente solo guarda H(USER:PASS), en memoria)
   A  = g^a mod N                          (a: 32 bytes del generador del sistema)
   u  = H(A | B)
   S  = (B - k·g^x)^(a + u·x) mod N
   K  = SHA1 de los bytes pares de S (32 bytes) intercalado con SHA1 de los impares → 40 bytes
   M1 = H( (H(N) xor H(g)) | H(USER) | s | A | B | K )
   M2 = H(A | M1 | K)                      (lo manda el servidor; el cliente lo comprueba)
   ```
   **Detalle crítico**: el servidor pasa cada número a bytes con `BigNumber::AsByteArray()`, que es
   **little-endian de longitud mínima** (sin los ceros de arriba): un `B` o una sal cuyo último byte es 0 se hashea
   con 31 bytes, `K` con 39, etc. Solo `S` va rellenado a 32. El cliente hace lo mismo (`net::minimalLe`).
4. **C→S `AUTH_LOGON_PROOF` (0x01)**: `A[32]`, `M1[20]`, `crcHash[20]` (ceros: solo se mira con
   `StrictVersionCheck`), `numKeys u8 = 0`, `securityFlags u8 = 0`.
5. **S→C**: `cmd`, `error u8`; si es 0, `M2[20]` y `surveyId u32` (26 bytes). Errores: `AuthCodes.h`
   (`0x04` cuenta o contraseña incorrectas, `0x06` ya conectada, `0x08` ocupado/bloqueado por intentos…).
6. **C→S `REALM_LIST` (0x10)** + `u32 0` → **S→C**: `cmd u8`, `size u16`, `u32 0`, `count u8`, y por reino
   `icon u32`, `flags u8` (`0x02` = offline), nombre y `"ip:puerto"` (cstrings), `population f32`, `chars u8`,
   `category u8`, `u8 0`; al final `u16 2`.

7. **Fase 18, elegir reino** (`net/RealmListMessages`, `NetClient::ChoosingRealm`): tras la prueba se pide la lista
   (`REALM_LIST`, como mucho una vez por segundo: `MinRealmListDelay`) y se toma el reino guardado (`realmName` de
   `WTF/Config.wtf` o `realm.name` de `client.cfg`) o, si solo hay uno, ese; si no, el jugador elige en RealmList con
   realmd abierto (la lista se refresca). Carga del reino como `GetRealmInfo` (0x46EE40): flags `0x20` Recomendado,
   `0x40` Nuevo, `0x80` Lleno, si no la población frente a la media ± 0,6 σ (0x46E66D); orden de `SortRealms`
   (0x46F330). «Change Realm» vuelve a hacer login en realmd (clave de sesión nueva; realmd corta a los 300 s,
   `MaxSessionDuration`). Un login fallido ya no se reintenta solo: solo se reconecta tras una sesión que estuvo dentro.

Tests: `tests/net/test_crypto.cpp` (vectores SHA-1; intercambio SRP6 contra una simulación del servidor con su mismo
algoritmo, 13 cuentas sintéticas; contraseña errónea rechazada). Verificado en vivo con la cuenta de pruebas.

## 2. Mundo (world, TCP 8085)

### 2.1 Cabeceras

| Dirección | Formato de cabecera |
|---|---|
| S→C | `size u16` **big-endian** (incluye los 2 bytes de opcode) + `opcode u16` LE → 4 bytes |
| C→S | `size u16` **big-endian** (incluye los 4 bytes de opcode) + `opcode u32` LE → 6 bytes; el servidor corta si `size` > 0x2800 |

### 2.2 Autenticación de sesión

1. S→C `SMSG_AUTH_CHALLENGE`: `serverSeed u32` (sin cifrar).
2. C→S `CMSG_AUTH_SESSION` (sin cifrar): `build u32`, `serverId u32 = 0`, cuenta (cstring, MAYÚSCULAS),
   `clientSeed u32`, `digest[20] = SHA1(cuenta | u32 0 | clientSeed | serverSeed | K)` (K en longitud mínima) y el
   bloque de addons. Un bloque con **tamaño `u32 0`** es válido: el servidor simplemente no manda
   `SMSG_ADDON_INFO` (`AddonHandler::BuildAddonPacket`).
3. En cuanto el digest cuadra, el servidor activa el cifrado: desde `SMSG_AUTH_RESPONSE` **todas las cabeceras van
   cifradas** en ambos sentidos.
4. S→C `SMSG_AUTH_RESPONSE`: `AUTH_OK` (12), o `AUTH_WAIT_QUEUE` (27) + posición `u32`.

### 2.3 Cifrado de cabecera vanilla (no es RC4)

Clave = session key `K` en **su longitud mínima** (`m_Crypt.SetKey(K.AsByteArray())`: 39 bytes si su byte alto es
0). Solo se cifran las cabeceras (6 bytes al enviar, 4 al recibir), con índices que duran toda la conexión:

```cpp
// Enviar (cliente → servidor), sobre los 6 bytes de cabecera
for (t = 0; t < 6; ++t) { i %= keySize; x = (h[t] ^ K[i]) + sendLast; ++i; h[t] = sendLast = x; }
// Recibir (servidor → cliente), sobre los 4 bytes de cabecera
for (t = 0; t < 4; ++t) { i %= keySize; x = (h[t] - recvLast) ^ K[i]; ++i; recvLast = h[t]; h[t] = x; }
```
Test contra el `AuthCrypt` del servidor copiado literalmente (claves de 40 y 39 bytes, 50 paquetes por sentido).

### 2.4 Flujo de entrada al mundo

`CMSG_CHAR_ENUM` → `SMSG_CHAR_ENUM` → `CMSG_PLAYER_LOGIN(guid u64)` → `SMSG_LOGIN_VERIFY_WORLD`
(`map u32, x y z o`) + ráfaga inicial (en tu servidor: `SMSG_LOGIN_SETTIMESPEED`, `SMSG_TUTORIAL_FLAGS`,
`SMSG_INITIAL_SPELLS`, `SMSG_ACTION_BUTTONS`, `SMSG_INITIALIZE_FACTIONS`,
`SMSG_INIT_WORLD_STATES`, `SMSG_WEATHER`, `SMSG_COMPRESSED_UPDATE_OBJECT`…). `SMSG_SET_PROFICIENCY` y los
modificadores de hechizos llegan **antes** de `SMSG_LOGIN_VERIFY_WORLD` (§2.30).

- **`SMSG_CHAR_ENUM`** (`Player::BuildEnumData`): `count u8`; por personaje `guid u64`, nombre, raza, clase, sexo,
  piel, cara, peinado, color de pelo, vello facial, nivel (u8), `zone u32`, `map u32`, `x y z`, `guild u32`,
  `flags u32`, `firstLogin u8`, mascota (`display`, `level`, `family`: u32) y 20 × (`displayId u32`, `invType u8`).
- **`SMSG_LOGIN_SETTIMESPEED`**: hora empaquetada (`minuto` 6 bits, `hora` 5 bits, día, mes, año) + velocidad f32;
  la hora del día del motor sigue la del servidor.
- **Mantener viva la sesión**: `CMSG_PING` (`seq u32`, `latencia u32`) cada 30 s → `SMSG_PONG` (`seq`). El servidor
  cuenta como "overspeed" los pings a menos de 27 s y expulsa a los jugadores que abusan.
- **`SMSG_NEW_WORLD`** (cambio de mapa: mismo formato que `LOGIN_VERIFY_WORLD`) → el cliente responde
  `MSG_MOVE_WORLDPORT_ACK` vacío.

### 2.5 Objetos

- `SMSG_UPDATE_OBJECT`: `blockCount u32`, `hasTransport u8`, y bloques `u8 tipo`:
  `VALUES (0)` guid empaquetado + valores · `MOVEMENT (1)` guid + movimiento · `CREATE_OBJECT (2)` y
  `CREATE_OBJECT2 (3)` guid + `typeId u8` + movimiento + valores · `OUT_OF_RANGE_OBJECTS (4)` y `NEAR_OBJECTS (5)`:
  `count u32` + guids empaquetados.
- `SMSG_COMPRESSED_UPDATE_OBJECT`: `u32` tamaño inflado + zlib del mismo contenido (el servidor comprime por encima de
  `CompressionUpdateSize`; en tu servidor, 5 de cada 6 paquetes de actualización al entrar).
- **Bloque de movimiento** (`BuildMovementUpdate`, build > 1.8.4): `updateFlags u8`; si `LIVING (0x20)`:
  `MovementInfo`, 6 velocidades f32 (andar, correr, correr atrás, nadar, nadar atrás, giro) y, si el movimiento
  tiene `SPLINE_ENABLED`, el spline (`flags u32`; ángulo f32 / objetivo u64 / punto 3×f32 según `Final_*`;
  `timePassed u32`, `duration u32`, `id u32`, `nodes u32` + nodos 3×f32, destino 3×f32); si no es LIVING y tiene
  `HAS_POSITION (0x40)`: `x y z o`. Después: `HIGHGUID (0x08)` → u32, `ALL (0x10)` → u32,
  `MELEE_ATTACKING (0x04)` → guid empaquetado, `TRANSPORT (0x02)` → u32.
- **Bloque de valores**: `u8` palabras de máscara, la máscara (`u32[]`) y un `u32` por bit activo, en orden. Todos
  los campos viajan como `u32` (los float, por sus bits).
- `SMSG_DESTROY_OBJECT`: `guid u64` (completo, no empaquetado).
- GUIDs empaquetados: máscara u8 + un byte por bit activo (los bytes no nulos del u64).

Tests: `tests/net/test_session.cpp` (paquete sintético con creación, spline, valores y fuera de rango; paquetes
truncados). Con el tráfico real de 25 s en Villanorte: 97 entidades, 0 errores (`vd_netcheck --enter`).

### 2.6 Movimiento (fase 6)

**Formato.** `MovementInfo` (`MovementInfo::Read/Write`, `Object.cpp`): `flags u32`, `time u32`, `x y z o`,
[`ONTRANSPORT`: `guid u64` + `x y z o`], [`SWIMMING`: pitch f32], `fallTime u32`, [`JUMPING`: `zspeed, cos, sin,
xyspeed`], [`SPLINE_ELEVATION`: f32]. `zspeed` negativo = hacia arriba (un salto: −7,958; una caída desde un borde:
0). Los paquetes del cliente son solo el `MovementInfo` (sin guid); los que reenvía el servidor llevan el guid
empaquetado delante y en `time` su propio reloj.

**El cliente es autoritativo sobre su movimiento**, pero el servidor lo comprueba. Lo que hace nuestro cliente
(`app/PlayerController`, contrastado con `MovementHandler.cpp` y `MovementAnticheat.cpp`):

- **`CMSG_SET_ACTIVE_MOVER` (guid u64) al entrar en el mundo**: sin él `Player::GetConfirmedMover()` es nulo y el
  servidor **descarta todo el movimiento** en silencio.
- `time` = reloj del cliente en ms, nunca 0 (`NullTime`) ni hacia atrás (`ReverseTime`), avanzando al ritmo real
  (`TimeDesync`: la diferencia entre los relojes de dos paquetes no puede alejarse > 1 s de la del servidor). La
  posición enviada es la de la simulación en ese instante: la distancia entre dos paquetes no pasa de velocidad ×
  Δtime (`SpeedHack`, con un 5 % de margen en la comparación de cuadrados).
- **Un `MSG_MOVE_HEARTBEAT` cada 500 ms** mientras haya algún flag de `MASK_MOVING` (direcciones, `JUMPING`,
  `FALLINGFAR`): un hueco de más de 1 s cuenta como `SkippedHeartbeats`.
- **Cada flag de dirección aparece con su propio opcode** (`BadMoveStart`): `MSG_MOVE_START_FORWARD` con
  `FORWARD`, luego `..._START_STRAFE_LEFT` con `STRAFE_LEFT`... (varios en el mismo frame, en orden). Al soltar:
  `MSG_MOVE_STOP` / `MSG_MOVE_STOP_STRAFE`. Andar/correr: `MSG_MOVE_SET_WALK_MODE` / `SET_RUN_MODE`. Girar parado:
  `MSG_MOVE_SET_FACING` (≤ 10 por segundo); cabeceo nadando: `MSG_MOVE_SET_PITCH`.
- **Saltos y caídas**: `MSG_MOVE_JUMP` con `JUMPING` y `xyspeed` ≤ la velocidad del paquete anterior
  (`OverspeedJump`); durante el vuelo `xyspeed` no puede crecer (`JumpSpeedChange`) y `fallTime` debe contar los ms
  en el aire (`NoFallTime` avisa si tras 1 s sigue a 0). Salir de un borde: un latido inmediato con `JUMPING`
  (`zspeed` 0). Un vuelo solo termina con `MSG_MOVE_FALL_LAND` o `MSG_MOVE_START_SWIM` (`BadFallStop`); el daño de
  caída sale de `fallTime` del aterrizaje (≥ 1.229 ms, 14,57 yd) si el paquete anterior llevaba `FALLINGFAR`
  (lo ponemos al bajar por debajo de la altura de despegue). Un solo salto por vuelo (`MultiJump`).
- Sin flags de movimiento no se desplaza nada: > 1 yd en XY o > 2 yd en Z entre dos paquetes es `Teleport`.
- Subir: el servidor mide pendiente entre paquetes (`WallClimb`: Δz > tan(57°)·Δxy, solo si Δz ≥ 1 yd y Δxy ≥
  0,5 yd); el controlador no sube más de 50° a lo largo de la marcha y sus escalones (0,73 yd para un humano, como
  el cliente) quedan por debajo de ese metro.
- Agua: `MSG_MOVE_START_SWIM` / `MSG_MOVE_STOP_SWIM` (este último sin `SWIMMING`); se nada con el agua a
  `0,75 × altura de colisión` sobre los pies (`Unit::GetMinSwimDepth`).

**Órdenes del servidor** (paquetes con guid empaquetado + contador; la respuesta lleva guid u64 + el contador +
`MovementInfo` actual y se procesa con las mismas comprobaciones):

| Orden | Respuesta |
|---|---|
| `SMSG_FORCE_{WALK,RUN,RUN_BACK,SWIM,SWIM_BACK}_SPEED_CHANGE`, `SMSG_FORCE_TURN_RATE_CHANGE` (+ f32) | `CMSG_FORCE_*_ACK` + f32 velocidad |
| `SMSG_FORCE_MOVE_ROOT` / `UNROOT` | `CMSG_FORCE_MOVE_ROOT_ACK` / `UNROOT_ACK` (con / sin `ROOT`, sin direcciones) |
| `SMSG_MOVE_WATER_WALK`/`LAND_WALK`, `FEATHER_FALL`/`NORMAL_FALL`, `SET_HOVER`/`UNSET_HOVER` | `CMSG_MOVE_WATER_WALK_ACK` / `FEATHER_FALL_ACK` / `HOVER_ACK` + u32 aplicar |
| `SMSG_MOVE_KNOCK_BACK` (+ cos, sin, xy, z) | `CMSG_MOVE_KNOCK_BACK_ACK` (con `JUMPING` y esos 4 valores exactos) |
| `MSG_MOVE_TELEPORT_ACK` (+ `MovementInfo` destino): teletransporte en el mismo mapa | `MSG_MOVE_TELEPORT_ACK`: guid u64, contador, time u32 |
| `SMSG_TRANSFER_PENDING` (mapa) → `SMSG_NEW_WORLD` (mapa, x, y, z, o): cambio de mapa | `MSG_MOVE_WORLDPORT_ACK` (vacío) y de nuevo `CMSG_SET_ACTIVE_MOVER` |

Un `MSG_MOVE_*` con **nuestro propio guid** solo llega cuando el servidor corrige la posición (un movimiento
rechazado, `SendHeartBeat(true)`): se coloca ahí al personaje y lo que siga pulsado se vuelve a empezar con sus
`START`. El panel "Personaje" cuenta esas correcciones.

**Otras entidades**: los mismos `MSG_MOVE_*` (y `MSG_MOVE_TELEPORT`, `ROOT`, `KNOCK_BACK`... de otros jugadores),
`MSG_MOVE_SET_*_SPEED` (+ velocidad) y `SMSG_SPLINE_SET_*_SPEED` (guid + velocidad), `SMSG_COMPRESSED_MOVES` y
`SMSG_MONSTER_MOVE`: guid, punto inicial, `id u32`, tipo (`0` normal, `1` parada — no sigue nada más —, `2` mira a un
punto, `3` a un objetivo, `4` a un ángulo) con su dato, `flags u32`, `duration u32`, `count u32` y el camino:
- lineal (`WriteLinearPath`): el destino y después los puntos anteriores (desde el inicio) como desplazamientos
  **respecto al destino** empaquetados en u32 (x 11 bits, y 11, z 10, con signo, cuartos de yarda). Con 2 puntos no
  hay desplazamientos aunque `count` sea 2: se leen `min(count − 1, lo que queda)`;
- Catmull-Rom (`flags & 0x200`, Flying): `count` puntos f32×3; con `0x200000` (Enter_Cycle) el primero es falso.
- `0x100` = Runmode (sin él el PNJ camina), `0x100000` = cíclico (se repite). Se recorre a velocidad constante
  (longitud / duración); en los bloques de creación el camino llega completo con el tiempo ya transcurrido.

Nuestro cliente extrapola a los jugadores entre latidos (dirección y velocidad de sus flags; saltos y caídas con
la gravedad del cliente, 19,29 yd/s², tope 60,15; nadando, con su cabeceo) y no deja que un jugador en el aire quede
bajo el suelo; cuando llega un paquete, la diferencia con lo mostrado se funde en 200 ms en lugar de saltar.

### 2.7 Apariencia de unidades y consultas (fase 5)

Lo que decide cómo se ve una unidad llega en sus update fields (`net/UpdateFields.gen.h`):

| Campo | Uso |
|---|---|
| `UNIT_FIELD_DISPLAYID` / `NATIVEDISPLAYID` | `CreatureDisplayInfo`; en jugadores, si difieren (forma de druida, polimorfia) se dibuja el display |
| `UNIT_FIELD_MOUNTDISPLAYID` | montura (otro actor que lleva al jinete en su attachment 0) |
| `UNIT_FIELD_BYTES_0` | byte 0 raza, byte 2 sexo |
| `UNIT_FIELD_BYTES_1` byte 0 | estado de pie: 1 sentado, 2/4/5/6 sillas, 3 dormido, 7 muerto, 8 de rodillas |
| `UNIT_FIELD_BYTES_2` byte 0 | `SheathState`: 0 armas envainadas, 1 cuerpo a cuerpo en mano, 2 a distancia |
| `PLAYER_BYTES` | piel, cara, peinado, color de pelo (bytes 0–3); `PLAYER_BYTES_2` byte 0: vello facial |
| `PLAYER_VISIBLE_ITEM_1_0 + slot·12` | **entrada** del objeto equipado en cada ranura (no su display: hay que preguntarlo) |
| `UNIT_VIRTUAL_ITEM_SLOT_DISPLAY` (×3) / `INFO` (×2 por ranura) | armas de los PNJ: display directo; `INFO` byte 3 = `InventoryType`, siguiente palabra byte 0 = tipo de vaina |
| `UNIT_NPC_EMOTESTATE` | id de `Emotes.dbc` en bucle (trabajar, bailar…) |
| `OBJECT_FIELD_SCALE_X` (f32), `UNIT_FIELD_HEALTH` (0 = muerto) | escala; animación de muerte |

Consultas (`Query.cpp`, `ItemHandler.cpp`); el cliente las cachea toda la sesión y rehace la apariencia al llegar:

- `CMSG_ITEM_QUERY_SINGLE (entry u32, guid u64 = 0)` → `SMSG_ITEM_QUERY_SINGLE_RESPONSE`: `entry`, `class`,
  `subclass`, nombre + 3 cadenas vacías, `displayId`, `quality`, `flags`, compra, venta, `InventoryType`, 14 u32 de
  requisitos y pila, 10 × (tipo, valor), 5 × daño (min f32, max f32, tipo), armadura + 6 resistencias, `delay`,
  `ammoType`, `rangedModRange f32`, 5 × 6 u32 de hechizos, `bonding`, descripción, 6 u32 y **`sheath`**. Si el servidor
  no conoce la entrada (o nadie la ha "descubierto" y la cuenta no es GM) responde `entry | 0x80000000`.
- `CMSG_NAME_QUERY (guid u64)` → `SMSG_NAME_QUERY_RESPONSE`: `guid u64`, nombre, nombre del reino (vacío), `race`,
  `gender`, `class` (**u32 cada uno** en 5875).
- `CMSG_CREATURE_QUERY (entry u32, guid u64)` → `SMSG_CREATURE_QUERY_RESPONSE`: `entry` (o `| 0x80000000`), nombre +
  3 vacías, subtítulo ("Banquero"), `typeFlags`, `type`, `family`, `rank`, `u32 0`, `petSpellList`, `displayId`,
  `civilian`, `racialLeader u8`.

Tests: `tests/net/test_session.cpp` (respuestas construidas campo a campo como las escribe vMaNGOS).

### 2.8 Jugabilidad (fase 7)

`net/GameMessages.*` lee y escribe unos 60 paquetes (combate, hechizos, auras, PNJ, misiones, botín, chat,
experiencia y muerte) y `game::Gameplay` (`vd_game`) guarda su estado; `GameSession` solo reparte los paquetes, el HUD
lee con cursores propios sobre anillos de eventos (`game/EventRing.h`) y las acciones salen por un `PacketSink`. Tests:
`tests/net/test_game_messages.cpp` (cada paquete escrito campo a campo como en el servidor, más todos sus
truncamientos) y `tests/game/test_gameplay.cpp` (flujos completos contra un sink que captura lo enviado).

| Tema | Paquetes |
|---|---|
| Objetivo y cuerpo a cuerpo | `CMSG_SET_SELECTION`, `CMSG_ATTACKSWING` / `ATTACKSTOP`; `SMSG_ATTACKSTART` / `STOP`, `SMSG_ATTACKERSTATEUPDATE`, `SMSG_ATTACKSWING_*` (lejos, de espaldas, muerto…) |
| Hechizos | `SMSG_INITIAL_SPELLS`, `SMSG_ACTION_BUTTONS`, `CMSG_CAST_SPELL` (con `SpellCastTargets`), `SMSG_SPELL_START` / `GO`, `SMSG_CAST_RESULT`, `SMSG_SPELL_COOLDOWN`, `SMSG_COOLDOWN_EVENT`, registros de daño y curación |
| Auras | campos `UNIT_FIELD_AURA*` + `SMSG_UPDATE_AURA_DURATION` (solo las propias) |
| PNJ y misiones | `CMSG_GOSSIP_HELLO` / `SELECT_OPTION`, `SMSG_GOSSIP_MESSAGE`, `CMSG_QUESTGIVER_*`, `SMSG_QUESTGIVER_QUEST_LIST` / `DETAILS` / `REQUEST_ITEMS` / `OFFER_REWARD`, `SMSG_QUESTGIVER_STATUS`, `SMSG_QUESTUPDATE_ADD_KILL` / `ADD_ITEM` / `COMPLETE`, `CMSG_QUEST_QUERY`, `SMSG_NPC_TEXT_UPDATE` |
| Botín y objetos del mundo | `CMSG_LOOT` / `LOOT_MONEY` / `AUTOSTORE_LOOT_ITEM` / `LOOT_RELEASE`, `SMSG_LOOT_RESPONSE`, `CMSG_GAMEOBJ_USE`, `CMSG_GAMEOBJECT_QUERY` |
| Chat | `CMSG_MESSAGECHAT` (en el idioma de la raza), `SMSG_MESSAGECHAT`, `SMSG_NOTIFICATION` |
| Experiencia y muerte | `SMSG_LOG_XPGAIN`, `SMSG_LEVELUP_INFO`, `CMSG_REPOP_REQUEST`, `MSG_CORPSE_QUERY`, `CMSG_RECLAIM_CORPSE`, `SMSG_SPIRIT_HEALER_CONFIRM`, `CMSG_SPIRIT_HEALER_ACTIVATE`, `SMSG_RESURRECT_REQUEST` |

Trampas comprobadas:

- **GUID empaquetado o completo** según el paquete (en `SMSG_SPELL_GO` se mezclan); cada parser sigue al servidor.
- `SMSG_SPELL_COOLDOWN` no trae contador; `SMSG_CAST_RESULT` lleva estado 0/2 y sus argumentos dependen de la longitud.
- El chat se envía en el idioma de la raza del jugador (idioma 0 lo descarta el servidor).
- `SMSG_QUESTUPDATE_ADD_KILL` da el **total**; `ADD_ITEM` un **incremento**. Los objetos de misión se cuentan desde el
  inventario: los contadores de los campos no son fiables para objetos. vMaNGOS manda `ADD_ITEM` **antes** que el objeto
  (`ItemAddedQuestCheck` y luego `StoreItem`; el objeto llega con la siguiente actualización), así que el mensaje
  «Linen Cloth: n/m» es lo de las bolsas **más** lo añadido, con tope en m, de la primera misión del registro que lo
  pide (sin fallar); sin mensaje si las bolsas ya tenían bastante (WoW.exe 0x5E5AD0 → 0x5DD060). Contar solo las
  bolsas daba uno de menos («0/8» con el primero).
- **Barra de acción por forma**: actitudes de guerrero, felino, oso y sigilo cambian la barra principal por la de su
  forma (`ActionButton.lua`: hueco = id + (6 + desplazamiento − 1)·12). La forma está en `UNIT_FIELD_BYTES_1`, byte 2
  (actitud de batalla 0x11 → huecos 72–83, donde vMaNGOS pone Golpe heroico).
- **Cofres y objetos de misión con cerradura** (`gameobject_template.data0` = `Lock.dbc`): el cliente no envía
  `CMSG_GAMEOBJ_USE`, lanza con `CMSG_CAST_SPELL` sobre el objeto el hechizo de apertura que conoce todo personaje
  (efecto `OPEN_LOCK` = 33 con `EffectMiscValue` = el `LockType` de la cerradura; p. ej. "Opening").
- En modo GM (`.gm on`) el servidor corta el autoataque tras el primer golpe: probar el combate con `.gm off`.
- Dos clientes con la misma cuenta se echan el uno al otro al reconectar (10054 en bucle): cerrar el del móvil.

### 2.9 Jugabilidad completa (fase 8)

Cada área tiene sus parsers y builders en un fichero propio (`net/ItemMessages`, `net/CombatMessages`,
`net/SocialMessages`, `net/TravelMessages`) y su estado en `game/` (`Inventory`, `Abilities`, `Social`, `Travel`),
propiedad de `game::Gameplay`, que les delega los opcodes. Todos sacados de `Server/Packets/*` y de los handlers de
vMaNGOS 4641790, con tests byte a byte y de truncado (`tests/net/test_*_messages.cpp`) y de flujo completo contra un
sink que captura lo enviado (`tests/game/test_inventory.cpp`, `test_abilities.cpp`, `test_social.cpp`,
`test_travel.cpp`).

| Tema | Paquetes |
|---|---|
| Plantillas de objeto | `SMSG_ITEM_QUERY_SINGLE_RESPONSE` completo de 5875 (estadísticas, daño, armadura, resistencias, hechizos, ligado, conjunto, durabilidad, precios, requisitos…) |
| Inventario | campos `PLAYER_FIELD_INV_SLOT_*` (equipo 0–18, bolsas 19–22, mochila 23–38, banco 39–62, bolsas del banco 63–68, recompra 69–80, llavero 81–96), `CONTAINER_FIELD_*`, `ITEM_FIELD_*`; `CMSG_SWAP_INV_ITEM` / `SWAP_ITEM`, `CMSG_AUTOEQUIP_ITEM`, `CMSG_AUTOSTORE_BAG_ITEM`, `CMSG_SPLIT_ITEM`, `CMSG_DESTROYITEM`, `CMSG_OPEN_ITEM`, `CMSG_SET_AMMO`, `CMSG_READ_ITEM`, `CMSG_ITEM_TEXT_QUERY` / `CMSG_PAGE_TEXT_QUERY`, `SMSG_DURABILITY_DAMAGE_DEATH` |
| Vendedores y banco | `CMSG_LIST_INVENTORY` / `SMSG_LIST_INVENTORY`, `CMSG_BUY_ITEM` / `BUY_ITEM_IN_SLOT`, `CMSG_SELL_ITEM`, `CMSG_BUYBACK_ITEM`, `CMSG_REPAIR_ITEM`, `SMSG_BUY_ITEM` / `BUY_FAILED` / `SELL_ITEM`; `CMSG_BANKER_ACTIVATE` / `SMSG_SHOW_BANK`, `CMSG_AUTOBANK_ITEM` / `AUTOSTORE_BANK_ITEM`, `CMSG_BUY_BANK_SLOT` |
| Instructores y talentos | `CMSG_TRAINER_LIST` / `SMSG_TRAINER_LIST` (38 bytes por servicio), `CMSG_TRAINER_BUY_SPELL`, `SMSG_TRAINER_BUY_SUCCEEDED` / `FAILED`, `MSG_TALENT_WIPE_CONFIRM`, `CMSG_LEARN_TALENT` |
| Mascotas | `SMSG_PET_SPELLS` (tres formas y la de quitar, con solo el GUID), `SMSG_PET_MODE`, `CMSG_PET_ACTION` / `SET_ACTION` / `SPELL_AUTOCAST`, `SMSG_PET_ACTION_FEEDBACK`, `SMSG_PET_CAST_FAILED`, `SMSG_PET_TAME_FAILURE`, `CMSG_PET_NAME_QUERY` |
| Reputación y habilidades | `SMSG_INITIALIZE_FACTIONS` (siempre 64 entradas; `standing` sin la base de raza), `SMSG_SET_FACTION_STANDING` / `VISIBLE`, `CMSG_SET_FACTION_ATWAR`; habilidades en `PLAYER_SKILL_INFO_1_1` |
| Personajes | `CMSG_CHAR_CREATE` / `SMSG_CHAR_CREATE`, `CMSG_CHAR_DELETE` / `SMSG_CHAR_DELETE` (tras un éxito se pide la lista otra vez) |
| Grupo | `CMSG_GROUP_INVITE` / `ACCEPT` / `DECLINE` / `UNINVITE(_GUID)` / `DISBAND`, `SMSG_GROUP_INVITE`, `SMSG_GROUP_LIST` (lista vacía = sin grupo), `SMSG_PARTY_MEMBER_STATS(_FULL)` (GUID empaquetado + máscara), `SMSG_PARTY_COMMAND_RESULT`, `CMSG_LOOT_METHOD`, `CMSG_GROUP_RAID_CONVERT` |
| Chat y social | `CMSG_JOIN_CHANNEL` / `LEAVE_CHANNEL`, `SMSG_CHANNEL_NOTIFY`, `SMSG_CHANNEL_LIST`, `CMSG_TEXT_EMOTE`, `MSG_RANDOM_ROLL`; amigos e ignorados (`SMSG_FRIEND_LIST` / `FRIEND_STATUS` / `IGNORE_LIST`, `CMSG_ADD_*` / `DEL_*`), `CMSG_WHO` / `SMSG_WHO`; hermandad (`CMSG_GUILD_QUERY`, `GUILD_ROSTER`, `SMSG_GUILD_EVENT` con GUID opcional al final, invitaciones, ascensos, MOTD); duelos (`SMSG_DUEL_REQUESTED` / `COUNTDOWN` / `COMPLETE` / `WINNER` / `OUT_OF_BOUNDS`, `CMSG_DUEL_ACCEPTED` / `CANCELLED`) |
| Viajes | `CMSG_AREATRIGGER`, `SMSG_EXPLORATION_EXPERIENCE`, `CMSG_TAXINODE_STATUS_QUERY` / `SMSG_TAXINODE_STATUS`, `CMSG_TAXIQUERYAVAILABLENODES` / `SMSG_SHOWTAXINODES`, `CMSG_ACTIVATETAXI` / `ACTIVATETAXIEXPRESS`, `SMSG_ACTIVATETAXIREPLY`, `SMSG_NEW_TAXI_PATH`, `CMSG_MOVE_SPLINE_DONE`, `SMSG_BINDER_CONFIRM` / `CMSG_BINDER_ACTIVATE` / `SMSG_BINDPOINTUPDATE` / `SMSG_PLAYERBOUND`, `SMSG_TRANSFER_PENDING` / `ABORTED` |
| Correo | `CMSG_GET_MAIL_LIST` / `SMSG_MAIL_LIST_RESULT`, `CMSG_MAIL_MARK_AS_READ` / `TAKE_MONEY` / `TAKE_ITEM` / `DELETE` / `RETURN_TO_SENDER` / `CREATE_TEXT_ITEM`, `CMSG_SEND_MAIL` / `SMSG_SEND_MAIL_RESULT`, `MSG_QUERY_NEXT_MAIL_TIME`, `SMSG_RECEIVED_MAIL` |
| Subasta | `MSG_AUCTION_HELLO`, `CMSG_AUCTION_LIST_ITEMS` / `LIST_BIDDER_ITEMS` / `LIST_OWNER_ITEMS` y sus `SMSG_*_RESULT`, `CMSG_AUCTION_PLACE_BID`, `CMSG_AUCTION_SELL_ITEM`, `CMSG_AUCTION_REMOVE_ITEM`, `SMSG_AUCTION_COMMAND_RESULT`, avisos de puja superada, ganada, vendida y retirada |
| Comercio | `CMSG_INITIATE_TRADE` / `BEGIN_TRADE` / `ACCEPT_TRADE` / `UNACCEPT_TRADE` / `CANCEL_TRADE`, `CMSG_SET_TRADE_ITEM` / `CLEAR_TRADE_ITEM` / `SET_TRADE_GOLD`, `SMSG_TRADE_STATUS` (formas condicionales) y `_EXTENDED` (7 huecos × 15 palabras) |
| Tiradas de botín | `SMSG_LOOT_START_ROLL`, `CMSG_LOOT_ROLL`, `SMSG_LOOT_ROLL`, `SMSG_LOOT_ROLL_WON`, `SMSG_LOOT_ALL_PASSED`, `SMSG_LOOT_MASTER_LIST`, `CMSG_LOOT_MASTER_GIVE` |
| Visuales y sonidos (para la fase 9) | `SMSG_PLAY_SPELL_VISUAL` / `IMPACT`, `SMSG_PLAY_SOUND` / `MUSIC` / `OBJECT_SOUND`, `SMSG_EMOTE` y los inicios, golpes e interrupciones de hechizos de cualquier unidad, en el anillo `Gameplay::fx()` |

Trampas comprobadas (en línea contra el vMaNGOS local salvo donde se indica):

- **Cinemática de los personajes nuevos**: en su primera entrada el servidor envía `SMSG_TRIGGER_CINEMATIC` y mueve el
  punto de vista del jugador por el recorrido de la cinemática (`Player::CinematicStart`, `GetCamera().SetView`):
  hasta recibir `CMSG_COMPLETE_CINEMATIC` no envía nada de lo que rodea al jugador (un `.go` lejano dejaba al
  personaje sin su propia entidad). Fase 14: el cliente la reproduce (`app/Cinematics`: `SMSG_TRIGGER_CINEMATIC` lleva
  el id de `CinematicSequences.dbc` en un u32), envía `CMSG_NEXT_CINEMATIC_CAMERA` (cuerpo vacío; vMaNGOS no hace nada
  con él) al empezar cada cámara después de la primera y `CMSG_COMPLETE_CINEMATIC` (vacío) al terminar o al saltarla;
  los objetos de cámara (tipo 13) llegan por el mismo camino (`GameObject::Use` → `SendCinematicStart`).
- **`CMSG_MOVE_SPLINE_DONE`** es obligatorio al acabar cualquier spline del servidor sobre el jugador (vuelos, miedo):
  sin él vMaNGOS ignora todo el movimiento posterior (`HasPendingSplineDone`). Comprobado con un vuelo Ventormenta →
  Colina del Centinela: aterriza, envía el `SPLINE_DONE` y sigue moviéndose sin correcciones.
- **Canales de zona**: el servidor no une al jugador a General / Comercio / Defensa local; lo hace el cliente
  (`Player::UpdateLocalChannels` está vacío: «Updated client-side»), con los nombres de `ChatChannels.dbc` y la zona.
  Comprobado: al entrar en Ventormenta deja los de Elwynn y entra en «General - Stormwind City» y «Trade - City».
- **Disparadores de área**: el cliente comprueba las esferas y cajas de `AreaTrigger.dbc` con la misma prueba que
  `IsPointInAreaTriggerZone` y envía `CMSG_AREATRIGGER` una vez por entrada (posada de Villadorada: 562).
- El servidor **no reenvía los botones de la barra** al aprender un rango nuevo (`SMSG_SUPERCEDED_SPELL`): el cliente
  los cambia y los guarda con `CMSG_SET_ACTION_BUTTON`.
- Vendedores y banqueros con gossip: la ventana se abre cuando llega `SMSG_LIST_INVENTORY` / `SMSG_SHOW_BANK` tras
  elegir la opción; los que no tienen gossip se abren directamente con `CMSG_LIST_INVENTORY` / `BANKER_ACTIVATE`.
- `CMSG_BUYBACK_ITEM` usa el hueco 69..80; `CMSG_AUTOSTORE_*` usa `NULL_BAG` = 0; `SMSG_READ_ITEM_OK` lleva el GUID dos
  veces y `READ_ITEM_FAILED` GUID, motivo, GUID; el texto de página llega como la cadena entera de páginas.
- Los transportes `MO_TRANSPORT` tienen GUID `0x1FC0 << 48 | entrada`; su posición se calcula como
  `TransportMgr::GenerateWaypoints` (keyframes, aceleración, paradas) y se resincroniza con cada bloque de creación;
  `SMSG_NEW_WORLD` trae la posición sobre el transporte cuando `TRANSFER_PENDING` trae uno (sin probar en línea).
- `CMSG_SEND_MAIL` termina en u64 0 + u8 0 (builds > 1.9.4); el buzón se abre con `CMSG_GET_MAIL_LIST`, sin
  `CMSG_GAMEOBJ_USE`; franqueo de 30 c. El depósito de una subasta es precio de venta × cantidad × (minutos / 120) ×
  el porcentaje de la casa (5 % en las de facción, 25 % en la neutral); el servidor atiende una sola petición de lista
  a la vez. `CMSG_ACCEPT_TRADE` lleva un u32 que el servidor salta; el servidor no reenvía los objetos propios del
  comercio (los guarda el cliente) y `BACK_TO_TRADE` retira las dos aceptaciones (sin probar en línea).
- El duelo se pide lanzando el hechizo 7266 (efecto 83) sobre el rival; mientras dura, el rival es atacable aunque
  sea amistoso (`PLAYER_DUEL_ARBITER` / `PLAYER_DUEL_TEAM`).

### 2.10 Pasada de paridad (lo que hace el cliente original y vMaNGOS no)

| Comportamiento | Mensajes |
|---|---|
| Desenvainar al atacar o disparar | `CMSG_SETSHEATHED` (u32: 1 cuerpo a cuerpo, 2 a distancia): vMaNGOS nunca cambia el estado de envainado de un jugador (`HandleSetSheathedOpcode` es el único sitio); el cliente lo pide al empezar el autoataque, al recibir su propio `SMSG_ATTACKSTART` (Carga) y al disparar. Además, cada vez que elige la animación de una unidad (WoW.exe 0x5FDF80, al final de 0x5FE2F0) recalcula dónde van sus armas con el campo 2 (WeaponFlags) de `AnimationData.dbc`: 0x4 (hechizos, canalizar, nadar, despojar, montar, silla, dormir) o ir montado → envainadas; con un arma a distancia en la mano, solo sus animaciones (0x5FE180) o las que no llevan 0x10 la mantienen; con el autoataque puesto y una animación de combate cuerpo a cuerpo (0x5FCC10) → desenvainadas; 0x10 (posturas y golpes sin arma, emotes, sentarse) → envainadas; 0x20 (golpes y posturas con arma) o el autoataque → desenvainadas; si no, el jugador conserva el suyo y las demás unidades siguen el campo del servidor. El jugador manda el cambio al servidor: **un druida con el bastón en la mano lo envaina al lanzar y lo vuelve a sacar al acabar si sigue el autoataque** (`game::sheathAfterAnimation`, `app/Units`) |
| Enfriamiento de objetos | vMaNGOS no envía `SMSG_ITEM_COOLDOWN` al usar un objeto: el cliente aplica los valores del propio objeto (consulta del objeto; −1 = los del hechizo) cuando `SMSG_SPELL_GO` trae como lanzador uno de sus objetos |
| Encantamientos temporales de arma | `SMSG_ITEM_ENCHANT_TIME_UPDATE` (24 B) es el único temporizador vivo: el campo del objeto solo se escribe al guardar; el cliente cuenta hacia atrás |
| Auras de los miembros del grupo | máscaras de `SMSG_PARTY_MEMBER_STATS(_FULL)`: el completo lista todas las ranuras ocupadas, los incrementales solo las que cambian (0 = vaciada) |
| Cancelar un beneficio | `CMSG_CANCEL_AURA` con las reglas de `HandleCancelAuraOpcode` (ranura positiva, AFLAG_CANCELABLE, no pasiva...) |
| Barras de respiración / fatiga | `SMSG_START_MIRROR_TIMER` (21 B), `SMSG_PAUSE_MIRROR_TIMER` (5 B), `SMSG_STOP_MIRROR_TIMER` (4 B); vMaNGOS reenvía START en lugar de PAUSE |
| Aviso en el minimapa | `MSG_MINIMAP_PING` con coordenadas de mundo; el servidor solo lo reenvía dentro del grupo |
| Orientación tras una spline del servidor | la orientación de un `SMSG_MONSTER_MOVE` (ángulo u objetivo) dura hasta que la unidad se mueve por sí misma: sin borrarla, el jugador seguía mirando a la víctima de la Carga |
| Escala de las unidades | `OBJECT_FIELD_SCALE_X` ya es la escala completa (CreatureDisplayInfo × CreatureModelData, o la de la plantilla): el cliente 1.12 no vuelve a aplicar la del modelo |
| Distancias | la `position` de una entidad es el inicio de su última spline: despojar, alcance y ventanas usan la posición interpolada (`game::distanceAt`) |
| Desconexión | `CMSG_LOGOUT_REQUEST` → `SMSG_LOGOUT_RESPONSE` (u32 motivo: 1 en combate, 2 congelado, 3 saltando o cayendo; u8 inmediato: zona de descanso, taxi o GM), 20 s sentado y enraizado por el servidor, `CMSG_LOGOUT_CANCEL` / `SMSG_LOGOUT_CANCEL_ACK` (vacíos); tras `SMSG_LOGOUT_COMPLETE` se pide `CMSG_CHAR_ENUM` en la misma conexión (la sesión sigue autenticada) |
| Muerte | el temporizador de liberación es solo el flag 0x08 del byte 0 de `PLAYER_FIELD_BYTES` (y solo fuera de instancias): los 6 min (`CORPSE_REPOP_TIME`) los cuenta el cliente; `PLAYER_SELF_RES_SPELL` ≠ 0 ofrece la autorresurrección, que es `CMSG_SELF_RES` vacío |
| Invocación | `SMSG_SUMMON_REQUEST` = guid u64, zona u32, ms u32; `CMSG_SUMMON_RESPONSE` = guid (en 1.12 solo existe aceptar) |
| Ligado | vMaNGOS liga al despojar, equipar o usar sin preguntar: LOOT_BIND / EQUIP_BIND / AUTOEQUIP_BIND / USE_BIND los decide el cliente con el `bonding` de la plantilla y el flag de ligado del objeto |
| Cuentas | `.account create` solo existe en la consola de mangosd (RA y SOAP apagados): una cuenta se crea como `AccountMgr::CreateAccount` (verificador SRP6 `v` / `s` en `account` + `realmcharacters`) |


### 2.11 Clima (fase 11)

| Mensaje | Formato y reglas |
|---|---|
| `SMSG_WEATHER` | 13 B: u32 tipo (0 despejado, 1 lluvia, 2 nieve, 3 tormenta = tormenta de arena en 1.12), f32 grado 0..1 (vMaNGOS lo deja en 0,0001..0,9999), u32 sonido (`SoundEntries`: 8533–8535 lluvia leve/media/fuerte, 8536–8538 nieve, 8556–8558 arena; 0 por debajo de grado 0,27), u8 instantáneo (vMaNGOS siempre manda 0). Se envía al entrar en una zona (`Player::UpdateZone`), cada `ChangeWeatherInterval` (10 min) si cambia y con `.wchange <tipo> <grado>` (GM, la zona entera; no es permanente) |
| Reglas del cliente | el primer paquete tras entrar en un mundo (login o `SMSG_NEW_WORLD`) se aplica al momento (lo tapa la pantalla de carga); los siguientes se funden a 0,1/s, y un cambio de tipo primero desvanece el anterior. El sonido es el bucle que manda el servidor, en la categoría de ambiente |

### 2.12 Instancias (fase 12)

Leído en vMaNGOS (`Player::SendSavedInstances`, `SendRaidInfo`, `SendInstanceResetWarning`, `SendResetInstanceSuccess`
/ `Failed`, `SendRaidGroupOnlyError`, `Group::ResetInstances`, `HandleAreaTriggerOpcode`) y en WoW.exe (solo lectura:
sus manejadores 0x49E1C0, 0x49E470, 0x49E660, 0x49E6C0 y 0x5E48F1, y `CanShowResetInstances` 0x48A720 → 0x495C90).
Estado en `game::Travel`, líneas del cliente en `game::NoticeFeed`, ventanas en `app/PopupScreens` y `app/SocialScreens`.

| Mensaje | Formato y reglas |
|---|---|
| `SMSG_INSTANCE_SAVE_CREATED` | u32 (siempre 0). Al quedar guardado a una banda (al entrar con guardado permanente o al morir un jefe que liga): `INSTANCE_SAVED` en el chat de sistema |
| `SMSG_UPDATE_INSTANCE_OWNERSHIP` | u32: 1 si el jugador tiene algún guardado **permanente** (de banda). Solo tras `SMSG_NEW_WORLD`, nunca al entrar al juego |
| `SMSG_UPDATE_LAST_INSTANCE` | u32 mapa, uno por guardado permanente, tras el anterior |
| `SMSG_RAID_INSTANCE_MESSAGE` | u32 tipo (1 `RAID_INSTANCE_WARNING_HOURS`, 2 `_MIN`, 3 `_MIN_SOON`, 4 `RAID_INSTANCE_WELCOME`), u32 mapa, u32 segundos hasta el reinicio. La bienvenida al llegar a un mapa de banda; los avisos a 60, 15 y 5 min. WoW.exe trunca a horas o minutos enteros, elige el plural (`_P1`) si no es 1 y la bienvenida va en días, horas y minutos (`%dd %dh %dm`). Chat de sistema |
| `SMSG_INSTANCE_RESET` | u32 mapa: `INSTANCE_RESET_SUCCESS` con el nombre de `Map.dbc` (o su número). WoW.exe además olvida "la última instancia" |
| `SMSG_INSTANCE_RESET_FAILED` | **u32 razón primero**, u32 mapa. 0 `INSTANCE_RESET_FAILED` (quedan jugadores dentro), 1 `_OFFLINE`, 2 `_ZONING` (vMaNGOS nunca la manda), 3 o más: nada |
| `CMSG_RESET_INSTANCES` | vacío. Solo el líder (o un jugador solo); las bandas y los guardados permanentes no se reinician; solo, el mapa en el que está tampoco. Una respuesta por mapa |
| `CMSG_REQUEST_RAID_INFO` → `SMSG_RAID_INSTANCE_INFO` | vacío → u32 n, n × (u32 mapa, u32 segundos hasta el reinicio, u32 id de instancia): solo los guardados permanentes. El cliente lo pide en cada `PLAYER_ENTERING_WORLD` (RaidFrame) y con `/raidinfo` |
| `SMSG_RAID_GROUP_ONLY` | u32 tiempo (ms), u32 error (1 `ERR_RAID_GROUP_ONLY`, 2 `ERR_RAID_GROUP_FULL`). Con tiempo: la cuenta atrás de expulsión (`INSTANCE_BOOT`, 60 s; se va a la piedra de hogar). Con 0: entrada rechazada o cuenta atrás cancelada; WoW.exe muestra el error en los dos casos (vMaNGOS cancela con error 1, así que el texto sale también al cancelar) |
| `SMSG_TRANSFER_ABORTED` | en vMaNGOS solo u8 razón (sin el mapa delante): 1 instancia llena, 2 no encontrada (reiniciándose), 3 demasiadas instancias (`Instance.PerHourLimit`, 5 por hora en este servidor), 4 en silencio, 5 encuentro en curso |
| Fantasma y portal | `MSG_CORPSE_QUERY` da, si el cadáver está en otra instancia, la **entrada** (`ghostEntranceMap/X/Y` de `Map.dbc`) y el mapa real del cadáver en el último campo. Allí el cliente muestra `RECOVER_CORPSE_INSTANCE` (sin botones) en vez de `RECOVER_CORPSE`. Al entrar el fantasma en el disparador del portal, `Player::TeleportTo` lo resucita (50 %) antes de comprobar si puede entrar; no hay paquete propio |
| "Reset all instances" | `CanShowResetInstances` de WoW.exe: fuera de una mazmorra de grupo (`Map.dbc` tipo 1), habiendo salido de una (o recibido `UPDATE_LAST_INSTANCE`) hace ≤ 90.000 s, **y** con el indicador de `UPDATE_INSTANCE_OWNERSHIP`. Con vMaNGOS ese indicador solo vale 1 con guardados de banda, así que el cliente original nunca ofrecería reiniciar las mazmorras de 5: este cliente no lo exige. En grupo, solo para el líder (UnitPopup) |

**Carrera con los disparadores:** vMaNGOS procesa `CMSG_AREATRIGGER` en la cola `PACKET_PROCESS_SELF_ITEMS` y
`MSG_MOVE_TELEPORT_ACK` en `PACKET_PROCESS_MOVEMENT`; entre colas no hay orden. Si el disparador sale justo tras el ACK
de un teletransporte corto (con la colisión ya cargada), el servidor lo comprueba con la posición anterior y lo
descarta por lejano, sin respuesta. El cliente espera 500 ms tras un teletransporte antes de mirar disparadores
(`PlayerController::triggersAllowed`).

### 2.13 Mensajes que se perdían (fase 13, paso 13.9)

Leído en vMaNGOS (`Server/Packets/Misc`, `World::SendServerMessage`, `Player::SendMountResult`,
`Creature::SendAIReaction`, `PlayerMenu::SendPointOfInterest`, `ReputationMgr::SendForceReactions`,
`Player::SendFactionAtWar`, `Item::SendTimeUpdate`, `WorldObject::GetReactionTo`) y en WoW.exe (solo lectura: sus
manejadores 0x49DF80, 0x5E38C0, 0x604060 → 0x6056E0 y 0x4E2840 → 0x6DAC10). Parsers en `net/MiscMessages`.

| Mensaje | Formato y reglas |
|---|---|
| `SMSG_SERVER_MESSAGE` | u32 tipo (1 apagado, 2 reinicio, 3 texto propio, 4 apagado cancelado, 5 reinicio cancelado), cadena (el tiempo que queda, `secsToTimeString`). WoW.exe toma la fila de **`ServerMessages.dbc`** de ese tipo (`[SERVER] Shutdown in %s`...) con la cadena como argumento (la fila sola si la cadena está vacía; `[%d]: %s` si no conoce el tipo) y la añade al chat como línea de sistema, en ningún otro sitio. La cuenta atrás: cada hora por debajo de 12 h, cada 5 min por debajo de 30, cada minuto por debajo de 5, cada 5 s por debajo de 30 s y cada segundo los últimos 10 |
| `SMSG_MOUNTRESULT` / `SMSG_DISMOUNTRESULT` | u32 resultado (`UnitMountResult` 0–9, 10 bien; `UnitDismountResult` 0–2, 3 bien). Los fallos son las líneas `ERR_MOUNT_*` / `ERR_DISMOUNT_*` de UIErrorsFrame en ese orden; los aciertos no dicen nada |
| `SMSG_AI_REACTION` | u64 GUID completo, u32 reacción. WoW.exe solo usa dos: 0 alerta (la criatura vio al jugador: su `SoundAlert` de `CreatureSoundData`) y 2 hostil (ataca: su `SoundAggro`; el servidor la manda en cada `Unit::Attack`). Sustituye a la suposición anterior (aggro al ver `SMSG_ATTACKSTART`) |
| `SMSG_GOSSIP_POI` | u32 flags, f32 x, f32 y, u32 icono (celda de `Interface\Minimap\POIIcons`, rejilla de 8 × 8; todas las filas de `points_of_interest` de vMaNGOS traen flags 99 e icono 6, la bandera roja), u32 dato, cadena (≤ 63 B). Las indicaciones de los guardias. El cliente guarda **un** punto (el segundo hueco de PDI del minimapa; el primero es el cadáver): uno nuevo lo sustituye, no suena. Se dibuja en el minimapa con su icono y, fuera del radio, con la flecha guía (`ROTATING-MINIMAPGUIDEARROW`) en el borde; tocarlo dice su nombre. Se borra al cambiar de mundo |
| `SMSG_SET_FORCED_REACTIONS` | u32 n, n × (u32 facción de `Faction.dbc`, u32 rango 0 odiado – 7 exaltado): todas las reacciones forzadas (`SPELL_AURA_FORCE_REACTION`); vacía las borra |
| `SMSG_SET_FACTION_ATWAR` | u32 id de la lista de reputación, u8 flags (`FACTION_FLAG_AT_WAR` = 2 o 0). Cuando una criatura con reputación ataca al jugador (`Creature::SetInCombatWith`: así puede devolver el golpe) y cuando esa guerra temporal acaba |
| Reacción con el jugador | `game::Reactions` + `PlayerStanding` (de `Gameplay::standing()`), como `GetReactionTo`: primero la reacción forzada (rango ≤ hostil → hostil, hasta neutral → neutral, si no amistosa); si la facción tiene reputación (`reputationListID` ≥ 0), hostil en guerra y amistosa si no; si no, las máscaras de `FactionTemplate.dbc`. Del lado de la criatura hacia el jugador, el rango real solo se usa para colorear la ventana de reputación: aquí basta con la guerra |
| `SMSG_ITEM_TIME_UPDATE` | u64 GUID del objeto, u32 segundos. El tiempo que le queda a un objeto que caduca, más reciente que su `ITEM_FIELD_DURATION`: el tooltip lo descuenta desde que llegó |

### 2.14 Hechizos con objetivo y modificadores (fase 13, pasos 13.1 y 13.3)

| Mensaje / regla | Formato y reglas |
|---|---|
| `SMSG_SET_FLAT_SPELL_MODIFIER` / `_PCT_` | u8 bit (de `SpellFamilyFlags`), u8 operación (`SpellModOp`), i32 valor: el **total** de ese bit, no el cambio. Llegan **antes** de `SMSG_LOGIN_VERIFY_WORLD`, así que se borran al salir del mundo, no al entrar. Se aplican a los hechizos de la familia de la clase (`ChrClasses.dbc`) que no ignoran modificadores: (valor + plano) × (100 + %) / 100 (WoW.exe 0x6E6B30 / 0x6E6AF0). El 1.12 los aplica también en las descripciones (`$s`, `$m`, `$o`, `$d`, `$a`, `$r`, `$x`, `$t`, `$h`, `$n`, `$e`) |
| Lugar | `SpellCastTargets` con `TARGET_FLAG_DEST_LOCATION` (0x40) y la posición. El servidor mide la distancia 3D menos el radio del lanzador contra el alcance (con su modificador) + 1,25 yd. Los objetos que lanzan en un lugar (bombas) van por `CMSG_USE_ITEM` con el destino |
| Objeto | `TARGET_FLAG_ITEM` (0x10) con el GUID del objeto; la ranura «no se comerciará» del otro, `TARGET_FLAG_TRADE_ITEM` (0x1000) con la ranura 6 como GUID empaquetado. Sin el objetivo que pide `Targets`, el servidor responde «Invalid target» (`ValidateExplicitTargetMask`) |
| Preguntas de encantamiento | WoW.exe 0x495D60: `BIND_ENCHANT` si el efecto es de encantamiento (permanente o temporal), el encantamiento tiene `ENCHANTMENT_CAN_SOULBOUND` y el objeto equipable aún no está ligado; `REPLACE_ENCHANT` si ya hay algo en la ranura que escribe; `TRADE_REPLACE_ENCHANT` solo para permanentes en el comercio. «No» deja el hechizo esperando otro objeto |
| Lugar en el 1.12 | solo cambia el cursor (`CAST_CURSOR` / `CAST_ERROR_CURSOR`): el círculo en el suelo, su posición inicial a 10 yd y la barra Lanzar / Cancelar son la adaptación táctil |

### 2.15 Control del movimiento y registro de combate (fase 13, pasos 13.2, 13.10 y 13.11)

| Mensaje / regla | Formato y reglas |
|---|---|
| `SMSG_CLIENT_CONTROL_UPDATE` | GUID empaquetado, u8 permitido. WoW.exe 0x603EA0 → 0x5FA600: marca por unidad; si la cámara está en esa unidad, permitido = pasa a ser el mover activo, no permitido = el cliente se queda sin mover. Para el jugador dispara `PLAYER_CONTROL_LOST` / `_GAINED` (UIParent cierra ventanas y hace parpadear `OutOfControlFrame`; como `UIFrameFlash` ignora la llamada que lo pararía, el destello dura sus 100 s) |
| `PLAYER_FARSIGHT` | 0x5EE290: la cámara va a ese objeto y el cliente manda `CMSG_FAR_SIGHT` 1 (0 al volver). Ojos de la bestia termina **sin** paquete de control: el campo vuelve a 0 |
| Cambio de mover | 0x6006E0, en orden: `CMSG_MOVE_NOT_ACTIVE_MOVER` (u64 + MovementInfo; solo si el viejo era el jugador y no seguía un spline), `CMSG_SET_ACTIVE_MOVER` (no para «ningún mover») y `CMSG_MOVE_SPLINE_DONE` del spline del nuevo mover que acabó entretanto: sin él vMaNGOS conserva `HasPendingSplineDone` y descarta todo el movimiento tras un miedo |
| `CMSG_MOVE_TIME_SKIPPED` | u64 + u32 ms. 0x616620: el cliente avanza el movimiento como mucho 250 ms por frame y manda el resto (pausa de Android, tirón); el servidor corre su ctime/stime. El de otras unidades (`MSG_MOVE_TIME_SKIPPED`) pausa su estimación |
| `SMSG_SPELLLOGEXECUTE` | GUIDs completos; por efecto su propio cuerpo (robo de poder, ataques extra, crear objeto, interrumpir, durabilidad, abrir cerradura, alimentar mascota...), algunos sin bytes. Líneas como `UnitCombatLog_C` de WoW.exe (`SPELLPOWERLEECH`, `EXTRAATTACKS`, `TRADESKILL_LOG`, `OPEN_LOCK`, `SPELLDURABILITYDAMAGE`...); la rabia se divide entre 10 |
| Otros del registro | `SPELLDAMAGESHIELD` (Espinas, aura de reprensión), `SPELLINSTAKILLLOG`, `SPELLDISPELLOG`, `DISPEL_FAILED`, `ENCHANTMENTLOG`; `SMSG_FEIGN_DEATH_RESISTED` va vacío (error 421 de UIErrorsFrame); `SMSG_SPELL_UPDATE_CHAIN_TARGETS` da los saltos de Cadena de relámpagos / sanación (para los haces de F14.4) |
| Armas a distancia | 0x47A070: el tipo de vaina 0 no se cuelga, y todas las armas a distancia de la base de datos de vMaNGOS tienen vaina 0: **el 1.12 no muestra el arma a distancia enfundada**. Desenfundada (estado 2, 0x47A0C0): el arco en la mano izquierda; armas de fuego, ballestas, varitas y arrojadizas en la derecha. Los bastones (vaina 2) van en los puntos 30/31 |

### 2.16 Misiones y objetos del mundo (fase 13, pasos 13.5 y 13.8)

| Mensaje / regla | Formato y reglas |
|---|---|
| `CMSG_PUSHQUESTTOPARTY` | u32 misión |
| `MSG_QUEST_PUSH_RESULT` | u64 + u8 en los dos sentidos: los avisos `ERR_QUEST_PUSH_*_S`. Cerrar la ventana de una misión compartida manda el rechazo (3) con el GUID del que la compartió; sin él vMaNGOS deja al jugador «ocupado» |
| Registro | orden de WoW.exe (0x4DE510, zonas por nombre con strcmp 0x4DE8F0, líneas por zona, cabecera, nivel y título 0x4DEAC0); las misiones sin plantilla todavía no se listan; el temporizador del registro es la hora unix del servidor |
| `SMSG_GAMEOBJECT_CUSTOM_ANIM` | u64 + u32 animación (12 B): Custom0..3 del modelo (el corcho al picar). `SPAWN_ANIM`, `DESPAWN_ANIM`, `RESET_STATE` y `PAGETEXT` llevan solo el GUID; `SMSG_FISH_NOT_HOOKED` / `_ESCAPED` van vacíos |
| Estado de los objetos | `GAMEOBJECT_STATE` 0 activo (abierto), 1 listo (cerrado), 2 destruido. Las puertas chocan en «listo» (regla `UpdateCollisionState` de vMaNGOS) |
| Lectura | los objetos TEXT (tipo 9) se leen en el cliente sin `CMSG_GAMEOBJ_USE` (vMaNGOS no los trata); un GOOBER con página llega por `SMSG_GAMEOBJECT_PAGETEXT`. El corcho se usa hasta a 100 yd (GetInteractionDistance) |

### 2.17 Menús de jugador, iconos de banda y chat (fase 13, pasos 13.6 y 13.7)

| Mensaje / regla | Formato y reglas |
|---|---|
| `MSG_RAID_TARGET_UPDATE` | el cliente manda (u8 icono, u64 GUID) para poner o quitar y u8 0xFF para pedir la lista; el servidor manda modo 0 + un cambio o modo 1 + la lista entera. vMaNGOS reenvía la lista tras cada `SMSG_GROUP_LIST` de un grupo |
| Distancias de `CheckInteractDistance` | 1 inspeccionar 10 yd, 2 comerciar 11,11 yd, 3 duelo 10 yd, 4 seguir 30 yd |
| Varios | `CMSG_TOGGLE_PVP` va vacío; el cliente lee e ignora `SMSG_INSPECT`; `SMSG_PET_NAME_INVALID` da `ERR_INVALID_PETNAME`; `SMSG_GUILD_INFO` imprime `GUILD_NAME_TEMPLATE` y `GUILD_INFO_TEMPLATE` (mes-día-año); `SMSG_PLAYED_TIME` da `TIME_PLAYED_TOTAL` / `_LEVEL` |
| Enlaces de objeto | `"%s\|Hitem:%d:%d:%d:%d\|h[%s]\|h%s"` (WoW.exe), con encantamiento, propiedad aleatoria y sufijo. Con `ChatStrictLinkChecking.Severity = 2` vMaNGOS descarta en silencio las líneas con una barra escrita a mano: el cliente escapa las barras tecleadas (`\|\|`) y deja enteros los enlaces insertados |

### 2.18 Interfaz que faltaba (fase 15)

| Mensaje / regla | Formato y reglas |
|---|---|
| `SMSG_OPEN_CONTAINER` | u64: la bolsa recién puesta en una ranura de bolsa (BAG_OPEN). vMaNGOS solo lo manda tras un `CMSG_SWAP_INV_ITEM` a una ranura de bolsa, y antes de los campos de la bolsa (el cliente espera hasta 3 s) |
| Probador | `TryOn` de WoW.exe (0x504540, emparejamiento 0x504BC0): la armadura a su ranura; un arma de una mano a la mano izquierda solo si la derecha ya tiene una y el personaje puede usar dos (efecto de hechizo 40, 0xC4D770); lo que no puede ir junto se quita |
| Maestro despojador | `SMSG_LOOT_MASTER_LIST` (u8 n + n × u64); los candidatos se numeran como `GetMasterLootCandidate` (0x61C550): en grupo, el orden de la lista; en banda, cinco por subgrupo. `CMSG_LOOT_MASTER_GIVE` (u64 botín, u8 ranura, u64 destino). CONFIRM_LOOT_DISTRIBUTION solo desde épico (MASTER_LOOT_THREHOLD 4); CONFIRM_LOOT_ROLL lo pregunta el cliente con el `bonding` de la plantilla |
| Honor | `MSG_INSPECT_HONOR_STATS`: petición u64; respuesta de 50 B (u64, rango más alto u8, hoy HK/DK u16+u16, ayer HK u32, … posición, progreso del rango u8). Los campos del jugador como los escribe `HonorMgr` (cuentas empaquetadas por bytes) |
| Reputación y habilidades | `CMSG_SET_WATCHED_FACTION` (i32 índice de la lista, −1 ninguna), `CMSG_SET_FACTION_INACTIVE` (u32 + u8), `CMSG_SET_FACTION_ATWAR` (rechazado en combate como el cliente), `CMSG_UNLEARN_SKILL` (u32). Listas de WoW.exe: reputación 0x4D5430–0x4D6C00 (tabla de rangos 0x80928C), habilidades 0x4D2CB0 / 0x4D3070 / 0x4D3610 |
| Establos | `MSG_LIST_STABLED_PETS` (petición u64; respuesta u64, u8 n, u8 huecos, n × mascota), `CMSG_STABLE_PET` / `CMSG_UNSTABLE_PET` / `CMSG_STABLE_SWAP_PET` (u64 + número), `CMSG_BUY_STABLE_SLOT`, `SMSG_STABLE_RESULT` (u8: 1 sin dinero, 8 y 9 piden la lista otra vez, 10 un hueco más). Reglas de `StableInfo.cpp`: la mascota actual en el registro 0, precio `StableSlotPrices[huecos + 1]` |
| Mascota | `SMSG_PET_UNLEARN_CONFIRM` (u64, u32 coste) → CONFIRM_PET_UNLEARN → `CMSG_PET_UNLEARN`; `CMSG_PET_SET_ACTION` con dos pares (24 B: intercambiar); autolanzamiento: 0x80 permitido, 0x40 activo; puntos de adiestramiento `(campo >> 16) − (campo & 0xFFFF)`; `CreatureQueryInfo` lleva el displayId (el modelo del establo) |
| `UNIT_COMBAT` | sus fuentes en WoW.exe (0x494600 y los paquetes de combate y de hechizo: 0x6243E0, 0x5E85E0, 0x626F87, 0x5E84E0, 0x624F30, 0x5E89C0, 0x627017, 0x5E7E00, 0x6E7A70); energizar, drenajes, SPELLORDAMAGE_IMMUNE y PROCRESIST no lo disparan |
| Hermandad | permisos de `GuildControlSetRankFlag` {0x1, 0x2, 0x4, 0x8, 0x80, 0x100, 0x10, 0x20, 0x1000, 0x2000, 0x4000, 0x8000, 0x10000}; entre 5 y 10 rangos; `CMSG_GUILD_SET_PUBLIC_NOTE` / `_OFFICER_NOTE`, `CMSG_GUILD_INFO_TEXT`, `CMSG_GUILD_RANK` / `_ADD_RANK` / `_DEL_RANK`; la lista del 1.12.1 solo muestra a los conectados (orden de `SortGuildRoster` de WoW.exe) |
| Cédulas | `SMSG_PETITION_SHOWLIST`, `CMSG_PETITION_BUY` (nombre; 10 s), `CMSG_PETITION_SHOW_SIGNATURES` / `SMSG_PETITION_SHOW_SIGNATURES`, `CMSG_PETITION_QUERY`, `CMSG_OFFER_PETITION`, `CMSG_PETITION_SIGN` / `MSG_PETITION_DECLINE` / `SMSG_PETITION_SIGN_RESULTS`, `MSG_PETITION_RENAME`, `CMSG_TURN_IN_PETITION` / `SMSG_TURN_IN_PETITION_RESULTS`; firmas mínimas `MinPetitionSigns` (9) |
| Tabardo | `MSG_TABARDVENDOR_ACTIVATE` (u64), `MSG_SAVE_GUILD_EMBLEM` (u64 + 5 × u32; resultado u32 0–4, coste 10 g); el emblema de cada hermandad llega en `SMSG_GUILD_QUERY_RESPONSE` |
| Ayuda | `CMSG_GMTICKET_CREATE` / `_UPDATETEXT` / `_DELETETICKET` / `_GETTICKET` / `_SYSTEMSTATUS` y sus respuestas; «Atascado» lanza el hechizo de efecto 84 que conozca el jugador. vMaNGOS ignora `CMSG_BUG` (`STATUS_NEVER`), nunca ofrece la encuesta de MJ ni manda `SMSG_GM_TICKET_STATUS_UPDATE` |
| Opciones | `CMSG_TOGGLE_HELM` / `CMSG_TOGGLE_CLOAK` (vacíos; el servidor pone `PLAYER_FLAGS` 0x400 / 0x800), las barras extra en `PLAYER_FIELD_BYTES` byte 2 (`CMSG_SET_ACTIONBAR_TOGGLES` u8), BLOCK_TRADES responde con `CMSG_BUSY_TRADE`; los botones de macro son el tipo 0x40 de `CMSG_SET_ACTION_BUTTON` (las macros viven en el cliente: `WTF\Account\<cuenta>\macros-cache.txt` y `…\<reino>\<personaje>\macros-local.txt`) |
| Mapa del mundo | nivel World (zoom 0, `GetPlayerMapPosition` 0x4A7360): u = offsetX · 16/1002 + 0,5 − y · escala / 33400, v = offsetY · 16/668 + 0,5 − x · escala / 22266,7 (`WorldMapContinent`) |
| Minimapa de interiores | dentro de un grupo de interior, WoW.exe (0x6D95F8) dibuja las teselas del edificio cuyos grupos tocan [p − r, p + r] en planta y [z − 1,5 r, z + 0,5 r] en altura (r = radio del minimapa de interior: 150, 120, 90, 60, 40, 25 yd, tabla 0x8116E8) |

### 2.19 Bandas (fase 16)

| Mensaje / regla | Formato y reglas |
|---|---|
| Lista de la banda | `SMSG_GROUP_LIST` (0x5E6A40): los demás en el orden del paquete (el de entrada al grupo, `Group::SendUpdate`), el jugador al final; 0x4BA5F0 la copia a la lista de la banda (0xB712A8, 40 entradas, cuenta en 0xB713E0): rango 2 el líder (0xBC75F8), 1 la bandera 0x80, subgrupo = bandera & 0x7F. `GetRaidRosterInfo` 0x4BB560 (sin el nombre en caché: nil). La clase de los que no se ven sale de `SMSG_NAME_QUERY_RESPONSE` |
| «party1..4» en una banda | el mismo manejador vacía las 4 casillas del grupo y añade, en el orden de la lista, a los de tu subgrupo ((banderas ^ propio) & 0x7F == 0; 0x4E82D0 añadir, 0x4E84A0 vaciar, 0x4E81A0 `GetPartyMember`, 0x4E86D0 `GetNumPartyMembers`). `UnitInParty` 0x4E7F70, `UnitInRaid` 0x4BAEE0. Los puntos del minimapa recorren solo esas 4 casillas (0x6DAD60); el rastreo de humanoides excluye a los del grupo (0x4EABD8) |
| Mensajes de la lista | tabla de mensajes del sistema 0xB4B498 (20 B por entrada): ERR_RAID_YOU_JOINED al pasar de 0, ERR_RAID_MEMBER_ADDED_S / _REMOVED_S solo si la banda ya existía, ERR_RAID_YOU_LEFT (0x4BA550); en banda no salen ERR_JOINED_GROUP_S / ERR_LEFT_GROUP_S; nada al mover o ascender |
| Subgrupos | `CMSG_GROUP_CHANGE_SUB_GROUP` (nombre + u8 grupo 0..7; el servidor ignora un grupo lleno), `CMSG_GROUP_SWAP_SUB_GROUP` (dos nombres). vMaNGOS obedece al líder y a los ayudantes; el cliente 1.12 solo deja arrastrar al líder (`RaidGroupButton_OnDragStart`: `IsRaidLeader`, 0x4BB8C0, compara GUID sin mirar si hay banda). `IsRaidOfficer` 0x4BB910 (rango > 0, incluye al líder); `SetRaidSubgroup` 0x4BB990 y `SwapRaidSubgroup` 0x4BBB00 solo envían si el subgrupo cambia |
| Comprobación de listos | `MSG_RAID_READY_CHECK`: petición vacía (el líder; vMaNGOS acepta también ayudantes pero `DoReadyCheck` 0x4BB1D0 solo deja al líder, con ERR_READY_CHECK_IN_PROGRESS dentro de los 30 s); el servidor la reenvía a todos, también al líder. Respuesta del miembro: u8 (1 listo, 0 no; el «No» de ReadyCheckFrame llama a `ConfirmReadyCheck()` sin argumento = 0; 0x4BC090 solo envía durante los 30 s); al líder le llega u64 GUID + u8. Manejador 0x4BA360: el miembro imprime ERR_RAID_LEADER_READY_CHECK_START_S y abre ReadyCheckFrame (evento READY_CHECK, 30 s → READY_CHECK_YOU_WERE_AFK); el líder imprime READY_CHECK_START, RAID_MEMBER_NOT_READY por cada «no» y, al acabar (todos los conectados respondieron o 30 s: `CheckReadyCheckTime` 0x4BB310), RAID_MEMBERS_AFK con los que no respondieron (los desconectados incluidos, en orden de la lista) o READY_CHECK_NO_AFK |
| Piedras de encuentro | usar un objeto de tipo 23 manda `CMSG_MEETINGSTONE_JOIN` (u64 la piedra) en vez de `CMSG_GAMEOBJ_USE` (0x5F69D0), tras comprobar en este orden: en grupo sin ser el líder → ERR_MEETING_STONE_MUST_BE_LEADER, grupo de 5 → _GROUP_FULL, nivel fuera de data0..data1 → _INVALID_LEVEL (vMaNGOS no lo mira), en banda → _NO_RAID_GROUP. `CMSG_MEETINGSTONE_INFO` (vacío) en cada entrada al mundo; `CMSG_MEETINGSTONE_LEAVE` (vacío; un miembro que no es el líder recibe _NOT_LEADER). `SMSG_MEETINGSTONE_SETQUEUE` (u32 zona + u8 estado: 0 salió, 1 en cola (solo si la zona cambia; efecto Bind_Impact_Base a los pies), 2–4 sus líneas, 5 nada), `_COMPLETE`, `_IN_PROGRESS`, `_MEMBER_ADDED` (u64), `_JOINFAILED` (u8 1–3). Las líneas van al chat salvo los cinco rechazos, al UIErrorsFrame (tabla de errores 0x488410, entradas 422–434 y 454–455). Tooltip de la piedra (0x5F6B10): la zona de data2 y MEETINGSTONE_LEVEL |
| Avisos y colores | RaidWarningFrame y RaidBossEmoteFrame: MessageFrame de 1.12 (10 s visibles + 3 s de fundido, constructor 0x81CC2C); colores por defecto del chat (tabla 0x805DD5): RAID_LEADER, RAID_WARNING y RAID_BOSS_EMOTE 255 219 183. Tipos fijos del chat (`ChatTypeInfo.sticky`): solo SAY, PARTY, RAID, GUILD y BATTLEGROUND; al abrir el chat, uno fijo de grupo o banda sin grupo o banda habla en SAY |
| Auras de los marcos sueltos | `UnitBuff` 0x519500 / `UnitDebuff` 0x5198F0; listadas (0x519860): sin SPELL_ATTR_DO_NOT_DISPLAY ni SPELL_ATTR_EX_NO_AURA_ICON, no de rastreo (44, 45, 151); SHOW_CASTABLE_BUFFS (0x4B3870): conocido, no pasivo, sin SPELL_ATTR_EX3 de filtro de banda, primer efecto no sobre el lanzador, sin aura de área (27, 35, 119, 128, 129); SHOW_DISPELLABLE_DEBUFFS: los tipos de disipación de los hechizos conocidos con efecto DISPEL no hostil (0x4B25B0 / 0x4B3920). `DropItemOnUnit` 0x48D960 (comerciar) |
| `SMSG_RAID_GROUP_ONLY` | (fase 12, comparado con 0x5E48F1): con tiempo 0, error 1 → ERR_RAID_GROUP_ONLY y 2 → ERR_RAID_GROUP_FULL (rojos); tiempo > 0 empieza la expulsión (INSTANCE_BOOT), ≤ 0 la para |

### 2.20 JcJ: campos de batalla y honor (fase 17)

Formatos de vmangos-core (`Server/Packets/Battleground.cpp`, `Misc.cpp`, `BattleGroundMgr.cpp`, `HonorMgr`); reglas del
desensamblado de WoW.exe 1.12.1 (solo lectura). El módulo de campos de batalla registra sus manejadores en 0x4A9C40.

| Mensaje / regla | Formato y reglas |
|---|---|
| Entrada al mundo | cada `PLAYER_ENTERING_WORLD` (0x4908C0 → 0x4A9DB0) vacía la lista del maestro de batalla y la puntuación y manda `CMSG_BATTLEFIELD_STATUS` (vacío); el servidor responde un `SMSG_BATTLEFIELD_STATUS` por cola ocupada. vMaNGOS conserva la cola tras cerrar sesión |
| `SMSG_BATTLEFIELD_STATUS` | u32 cola (0..2; WoW.exe descarta ≥ 3), u32 mapa (0 = cola vacía, nada más); u8 tramo, u32 instancia, u32 estado (1 queued, 2 confirm, 3 active; otro = "error"), u32 time1 y, solo en 1 y 3, u32 time2. Manejador 0x4AA850: colas de 32 B en 0xB6E9D0; confirm guarda ahora + time1 (caducidad de la invitación); queued la espera media y ahora − time2; active marca la cola activa (0x8457CC) y guarda la caducidad (ahora + time1, 0 mientras dura) y el inicio (ahora − time2) de la instancia (0xB6EBB8 / BC); cualquier otro estado borra esos dos tiempos. Siempre UPDATE_BATTLEFIELD_STATUS (0x1A3). Con una cola activa, `MSG_BATTLEGROUND_PLAYER_POSITIONS` (vacío) cada 5 s (0x4AA5C0) |
| Puertos y salida | `CMSG_BATTLEFIELD_PORT` (u32 mapa de la cola, u8 1 entrar / 0 salir de la cola; «Leave Queue» no pide confirmación en 1.12). `LeaveBattlefield` (0x4ABE60) solo envía `CMSG_LEAVE_BATTLEFIELD` (u32 mapa de la cola activa) cuando la puntuación dice que la partida acabó (0xB6EBCC, 0x4AAB92); vMaNGOS además lo ignora en combate. Antes del final no hay forma de salir desde la interfaz (piedra de hogar o cerrar sesión; Desertor lo pone el servidor) |
| Maestro de batalla | tocar una PNJ: el diálogo primero (0x5F0130; todos los maestros de vMaNGOS tienen diálogo, npcflag 2049, y su opción hace que el servidor mande la lista); sin diálogo, el bit de maestro manda `CMSG_BATTLEMASTER_HELLO` (u64, 0x5E01A0). `SMSG_BATTLEFIELD_LIST` (u64 maestro o el propio jugador si viene de un portal o de `.go warsong`, u32 mapa, u8 tramo, u32 n + n × u32 instancia; 0x4AA6C0 → BATTLEFIELDS_SHOW 0x1A1). Alejarse 5,56 yd (0x806574 al cuadrado) del punto donde llegó cierra la lista (0x4AA090 → BATTLEFIELDS_CLOSED 0x1A2) |
| Apuntarse | `CMSG_BATTLEMASTER_JOIN` (u64 maestro o jugador, u32 mapa, u32 instancia (0 = la primera libre), u8 en grupo); 0x4A9F60 rechaza en local un grupo demasiado grande (error 0x1BA). `GetBattlefieldInfo` (0x4AB0B0) lee de Map.dbc los niveles (13/14), los jugadores (15), la descripción de la Horda (20) o de la Alianza (29), el tamaño del tramo (39) y si se admite grupo (40). `ShowBattlefieldList` (0x4AB8C0, solo en cola) manda `CMSG_BATTLEFIELD_LIST` (u32 mapa); vMaNGOS responde con el GUID del jugador, así que «Change Instance» no hace nada en este servidor |
| `SMSG_GROUP_JOINED_BATTLEGROUND` | i32: −2 desertores, −1 falló, si no el mapa; entradas 439–441 de la tabla de errores (0xB4B498 + 20·i, tipo en +4: 0 chat, 1 UI_INFO_MESSAGE amarillo, 2 UI_ERROR_MESSAGE rojo) |
| Avisos de cierre | BattlefieldFrame_OnUpdate: con la instancia caducando, INSTANCE_COMPLETE_MESSAGE (con ganador) o INSTANCE_SHUTDOWN_MESSAGE al cruzar 600 / 300 / 60 / 15 s, mirado cada 3 s, al chat como SYSTEM |
| Chat del campo | tipos 0x52–0x54 (BG_SYSTEM_NEUTRAL / _ALLIANCE / _HORDE): `$n` en el texto es el nombre del remitente (0x49D961 → 0x506F70). Colores de la tabla de WoW.exe (64 B de nombre + R G B): BATTLEGROUND 255 127 0, BATTLEGROUND_LEADER 255 219 183, BG_SYSTEM_NEUTRAL 255 120 10, _ALLIANCE 0 174 239, _HORDE 255 0 0, COMBAT_HONOR_GAIN 224 202 10. Los jugadores del campo van a una banda propia del servidor |
| Estados del mundo | `SMSG_INIT_WORLD_STATES` (u32 mapa, u32 zona, u16 n, n × (u32 estado, u32 valor); vMaNGOS cuenta y añade un (0, 0) final) y `SMSG_UPDATE_WORLD_STATE` (u32, u32), en el despachador 0x48F690 (0x2C2 / 0x2C3): INIT libera todos los valores, guarda mapa y zona, rehace la lista de WorldStateUI (0x4C5650) y pone los pares; ambos disparan UPDATE_WORLD_STATES (0x20E). Un estado ausente vale 0 (0x4C5810). El mundo viejo se los lleva también en un teletransporte lejano (0x491222 → 0x4C55B0): hasta el INIT de la zona nueva `GetNumWorldStateUI` es 0 |
| WorldStateUI.dbc | `GetNumWorldStateUI` / `GetWorldStateUIInfo` (0x4C56E0 / 0x4C5A70): una fila entra si su mapa es el del INIT o −1, su área la zona del INIT o 0, y su tipo 0 (siempre) o 1 (solo si el jugador está en un canal LocalDefense, ChatChannels con las banderas 0x10000 y 0x2: 0x49BD50). Su estado es el valor de su variable (1 sin variable); el texto sustituye `%NNNNw` por el valor de ese estado (0x508560). Las columnas de la puntuación son la primera tanda de filas de tipo 2 del mapa activo (0x4AA99D). Las barras de captura son las filas con ExtendedUI "CAPTUREPOINT" |
| `MSG_PVP_LOG_DATA` | petición vacía (como mucho cada 5 s: 0x4AA170); respuesta u8 acabado (+ u8 ganador si acabó: 0 Horda, 1 Alianza), u32 n (≤ 80) × (u64, u32 rango, u32 golpes de gracia, u32 muertes con honor, u32 muertes, u32 honor extra, u32 m + m × i32 campos propios: WSG 2, AB 2, AV 7). WoW.exe (0x4AAB30) espera a tener el nombre de cada uno (0x4AA580) y saca su bando de la raza; orden 0x4AA350: el bando filtrado primero, golpes de gracia ↓, muertes ↑, honor extra ↓, nombre ↑ |
| Posiciones | `MSG_BATTLEGROUND_PLAYER_POSITIONS`: u32 n × (u64, f32 x, f32 y) de los compañeros fuera del grupo, luego u8 0/1 portador de bandera (u64, x, y): uno como mucho en 1.12.1. La bandera que se dibuja (0x4AA670) es la del bando contrario al del JUGADOR (0x468550 = el jugador activo) |
| Entradas y salidas | `SMSG_BATTLEGROUND_PLAYER_JOINED` / `_LEFT` (u64; 0x4AAE10): ERR_BG_PLAYER_JOINED_SS / ERR_BG_PLAYER_LEFT_S (entradas 444 / 445, al chat) cuando se conoce el nombre. `SMSG_BATTLEFIELD_WIN` / `_LOSE` no tienen manejador en 1.12.1; la música de victoria llega como `SMSG_PLAY_SOUND` 8454 / 8455 |
| Sanador espiritual de área | cada frame de fantasma (0x4923B0): el primer guía (npcflag 0x40) a ≤ 20 yd que el jugador pueda ayudar (0x4924C0), conservado hasta 22 yd; uno nuevo manda `CMSG_AREA_SPIRIT_HEALER_QUERY` (u64). `SMSG_AREA_SPIRIT_HEALER_TIME` (u64, u32 ms) → AREA_SPIRIT_HEALER_IN_RANGE (0x20F): AREA_SPIRIT_HEAL (la versión 1.12.1 acepta al mostrarse: `CMSG_AREA_SPIRIT_HEALER_QUEUE` (u64); su único botón cancela el aura 2584 «Esperando resurrección», 0x6E7040, que dispara _OUT_OF_RANGE 0x210). Un guía con diálogo sigue mandando `CMSG_GOSSIP_HELLO` |
| Insignia | `SMSG_PLAYER_SKINNED` (u8; 0x5E7D40) → PLAYER_SKINNED: oculta las ventanas RESURRECT y pone DEATH_CORPSE_SKINNED en rojo (SKINNED / SKINNED_REPOP están comentadas en el UIParent.lua de 1.12.1); el servidor contesta luego `MSG_CORPSE_QUERY` sin cadáver. Quitarla: el hechizo conocido de efecto 116 (22027) sobre el cuerpo enemigo con UNIT_FLAG_SKINNABLE (TARGET_FLAG_UNIT) o su cadáver con CORPSE_FLAG_LOOTABLE 0x20 (TARGET_FLAG_CORPSE_ENEMY) |
| `SMSG_PVP_CREDIT` | i32 honor, u64 víctima, i32 rango (0x625270): víctima 0 = COMBATLOG_HONORAWARD; honor > 0 COMBATLOG_HONORGAIN con el título del rango en el género del JUGADOR (0x612BF0), si no COMBATLOG_DISHONORGAIN; línea de chat tipo 0x51 (COMBAT_HONOR_GAIN), COMBAT_TEXT_UPDATE "HONOR_GAINED" y el texto «HK: rango» / «DK: Civilian» sobre el jugador en 224 202 10 (0x6072E0). Rangos de vMaNGOS: 5 = Private … 18 = Grand Marshal |
| Defensa | `SMSG_ZONE_UNDER_ATTACK` (u32 área; 0x49DCC0) y `SMSG_DEFENSE_MESSAGE` (u32 zona, u32 longitud con el NUL, texto; 0x49DE30): líneas sin remitente en los canales de defensa unidos (LocalDefense solo si el área es de la zona del jugador, WorldDefense siempre). vMaNGOS avisa al bando contrario al del atacante |

### 2.21 Pantallas de acceso, datos de cuenta y detalles (fase 18)

Formatos de vmangos-core (`CharacterHandler.cpp`, `MiscHandler.cpp`, `AuctionHouseHandler.cpp`, `WorldSession.cpp`);
reglas del GlueXML y del desensamblado de WoW.exe 1.12.1 (solo lectura).

| Mensaje / regla | Formato y reglas |
|---|---|
| Diálogos de estado | la secuencia de GlueDialog de WoW.exe: estados del login 0x46B1C9 (tabla 0x836B40), resultados 0x836B78 (LOGIN_FAILED con CONNECTION_HELP), GAME_SERVER_LOGIN 0x46B246, CHAR_LIST_RETRIEVING 0x46B880, REALM_LIST_IN_PROGRESS 0x46B8D0, la cola con CHANGE_REALM 0x46BC52, `StatusDialogClick` 0x46B720. `GetBuildInfo` (0x46CD70): «1.12.1», «5875», «Release», «Sep 19 2006» |
| Lista de personajes | al llegar se elige `lastCharacterIndex` (CVar 0x882674, 0x4724D0); tras crear, el último (SELECT_LAST_CHARACTER 0x46C12E); tras borrar, el primero (0x46C19D). `EnterWorld` (0x46B500): flag 0x4 rechazado con 0x44; flag `0x4000` abre FORCE_RENAME_CHARACTER. Seleccionar no suena (`SelectCharacter` 0x472740). Giro en grados (0x473650 / 0x473670) |
| Renombrar | `CMSG_CHAR_RENAME` (u64, nombre) → `SMSG_CHAR_RENAME` (u8 resultado; con éxito u64 + nombre nuevo): se renombra en la lista y se quita el flag 0x4000 (0x4725F0). `RenameCharacter` (0x473520): el mismo nombre da CHAR_CREATE_NAME_IN_USE; 0x31 tiene su texto, el resto CHAR_RENAME_FAILED. En GM: `.character rename` (marca el personaje para el siguiente login) |
| Crear | `ResetCharCustomize` (0x470900, 0x4702A0): sexo y raza al azar, aspecto al azar, la primera clase, orientación −15°. `SetSelectedRace` (0x470CF0): primera clase de la raza y aspecto al azar. `GetClassesForRace` (0x4706B0): solo las clases posibles, en el orden de CharBaseInfo (WoW.exe quita a mano el mago enano). `GetRandomName` (0x471980 / 0x475130 / 0x4752E0 / 0x475480): cadena de trigramas de NameGen.dbc por peso, 4–10 letras (el botón solo existe en enUS: `GlueLocalization.lua`). `CreateCharacter` (0x4711D0): comprobación local del nombre y luego `CMSG_CHAR_CREATE` |
| Escena de las pantallas | GlueXML: `SetSequence(0)` y `SetCamera(0)` de `Interface\Glues\Models\UI_*`; la niebla de AccountLogin (0,25 0,06 0,015, hasta 1.200) y `CharModelFogInfo`; gnomos en la escena enana y troles en la orca (`SetBackgroundModel`). El personaje está en el punto de enganche 0 del modelo |
| Entrar a bordo | `SMSG_LOGIN_VERIFY_WORLD` (y `SMSG_NEW_WORLD`) llevan, si el jugador está en un transporte, su posición **relativa al transporte** (`HandlePlayerLogin`: `GetTransportPos()`). El cliente espera al bloque de creación del propio jugador (justo detrás) y, si trae MOVEFLAG_ONTRANSPORT, usa su posición del mundo y lo sube a ese transporte |
| Tutoriales | `SMSG_TUTORIAL_FLAGS` (8 × u32, al entrar), `CMSG_TUTORIAL_FLAG` (u32 índice), `CMSG_TUTORIAL_CLEAR` / `_RESET` (vacíos). `TriggerTutorial` (0x4B5390): una vez por sesión; 51 llamadas en WoW.exe (entrar 0x490A41, 90 s quieto 0x482FE8, moverse 0x514A83, subir de nivel 0x5E4077, objetivo 0x493706, combate 0x629B10, botín 0x6003A0, objetos 0x491730, comida 0x5ECAC0…) |
| Datos de cuenta | `SMSG_ACCOUNT_DATA_MD5` (8 × MD5 de 16 B), `CMSG_REQUEST_ACCOUNT_DATA` (u32 tipo) → `SMSG_UPDATE_ACCOUNT_DATA` (u32 tipo, u32 tamaño sin comprimir, zlib), `CMSG_UPDATE_ACCOUNT_DATA` (igual; texto vacío = tamaño 0). Regla de WoW.exe (0x5AF6B0..0x5AFF96): se pide un tipo cuando el MD5 del servidor no es el de la caché local; si el servidor no tiene nada, se sube el local; un cambio sube 30 s después y al salir del mundo. VanillaDroid guarda los tipos 4 y 5 (macros de cuenta y del personaje) y 7 (`chat-cache.txt`) |
| Latencia | la de MainMenuBarPerformanceBar es la media de los últimos 16 viajes de ida y vuelta (0x537F20); verde < 300 ms, amarillo < 600, rojo |
| Ordenar listas | 1.12 ordena en el cliente: `SortWho` (0x5AD890, claves 0xC2817C, comparador 0x5ADA00, se reordena en cada `SMSG_WHO`) y `SortAuctionItems` (0x4CFC00; listas «list», «owner» y «bidder», comparador 0x4CD940): `CMSG_AUCTION_LIST_ITEMS` no lleva orden y cada página se ordena al llegar. Tocar la primera clave otra vez la invierte; otra clave pasa delante con su propio sentido |
| Correo | `InboxItemCanDelete` (0x4AFE00): una carta sin dinero ni objetos se puede borrar; con ellos, DELETE_MAIL / DELETE_MONEY |
| Modos de movimiento | `SMSG_SPLINE_MOVE_*` (GUID empaquetado; vMaNGOS: `SendMovementFlagChangeToAll`, `SendToggleRunWalkToAll`; `MoveSplineInit::Launch` manda UNROOT, el modo andar/correr y luego `SMSG_MONSTER_MOVE`; START_SWIM / STOP_SWIM nunca). WoW.exe (0x603C80 → 0x601420): ROOT termina el spline donde está la unidad; WATER_WALK, FEATHER_FALL, HOVER y nadar son banderas (dejar de nadar borra el pitch, 0x7C6E80); andar/correr (0x7C71C0) y las 6 `SMSG_SPLINE_SET_*_SPEED` (0x601360) siguen la estima desde la posición actual (0x7C5CD0) |
| Animación de movimiento | 0x5FD100: la decide la velocidad medida (en un spline, longitud / duración), no la bandera: corre por encima de 2 × la de andar y desde 11 yd/s es Sprint (143); StealthWalk (119) con UNIT_VIS_FLAGS_CREEP; Fly (135) en splines Flying; WalkBackwards (13); al nadar Swim / SwimBackwards / SwimLeft-Right (43 / 44) o SwimIdle al girar. Una criatura nada (0x6030C0) si la profundidad supera 0,75 × su altura de colisión − 1/36 y tiene la bandera 0x8000, 0x8 o 0x800 |
| PNJ a bordo | `SMSG_MONSTER_MOVE_TRANSPORT` = GUID de la unidad + GUID del transporte + el cuerpo de `SMSG_MONSTER_MOVE`, en el marco del transporte; un `SMSG_MONSTER_MOVE` normal la saca (0x603F00 / 0x617100). Una parada a ≥ 3 yd teletransporta la unidad en WoW.exe (0x601982); aquí se suaviza |
| Visión lejana | `CMSG_FAR_SIGHT` (u8) y PLAYER_FARSIGHT; `SMSG_CLEAR_FAR_SIGHT_IMMEDIATE` (0x5E43F8 → 0x5EE590): relee PLAYER_FARSIGHT y, si es 0, manda `CMSG_FAR_SIGHT 0` (vMaNGOS no lo envía). Alcance del hechizo 50.000 yd: la cámara puede quedar fuera de la colisión cargada |

### 2.22 Voces de los PNJ (retoques de la fase 20)

Del desensamblado de WoW.exe 1.12.1 (solo lectura). Sin paquetes: todo es del cliente (`app/GameAudio`, «NPC voices»).
NPCSounds.dbc (0xC0DA64, registros de 20 bytes) se alcanza por CreatureDisplayInfo columna 11: +4 saludo, +8 despedida,
+0xC «molesto»; todas sus filas de SoundEntries son 3D, distancia mínima 8 y corte a 45 yd.

| Regla | Comportamiento |
|---|---|
| Voz de clic | `CGUnit::PlayClickVoice` 0x60C270: clic izquierdo en una unidad (0x4925D0, también si ya es el objetivo) o clic derecho en una con la que no se puede interactuar ni atacar (`OnRightClick` 0x60BEA0). Solo criaturas vivas (no jugadores), con reacción ≥ neutral en los dos sentidos y el jugador vivo. Cinco saludos al azar y luego los ficheros de «molesto» en orden; después, un saludo y vuelta a empezar. Un solo contador para todas (último GUID 0xC4D908, cuenta 0xC4D970): otra unidad lo reinicia, y solo avanza si la frase suena. Aquí: el toque que selecciona y el toque sobre el objetivo cuando el botón de contexto no tiene nada que hacer con él |
| Saludo | `SetInteract` 0x4930D0, llamado al llegar el paquete que abre la ventana (charla, misiones, comerciante, instructor, vuelos, banco, subasta, establo, registrador de hermandades, tabardos): el PNJ anterior se suelta sin despedida y el nuevo saluda si no es el mismo (charla → comerciante: silencio). 0x60C3B0: criatura viva, jugador ni muerto ni fantasma, sin comprobar la reacción; borra el contador de clics |
| Despedida | `ClearInteract` 0x4931C0 con despedida al cerrar la ventana el jugador (`CloseGossip`, `CloseMerchant`, `CloseQuest`... desde los OnHide) o el servidor (`SMSG_GOSSIP_COMPLETE`). Sin despedida si la ventana se cierra por distancia (0x493230), si el PNJ desaparece (0x605860) o si se pasa a otro PNJ. Aquí: despedida si el PNJ sigue a menos de 9,5 yd de su esfera al cerrarse la última ventana |
| Una frase a la vez | cada unidad guarda su voz (+0xB1C): mientras suena, otro saludo, despedida o clic no suena ni cuenta (cerrar durante el saludo: sin despedida). Se reproduce en la posición del PNJ al empezar y no suena si el oyente está más allá del corte |

### 2.23 Animaciones de las unidades (retoques de la fase 20)

Del desensamblado de WoW.exe 1.12.1 (solo lectura) y de vmangos-core (`MoveSplineInit.cpp`, `Object.cpp`, `SpellAuras.cpp`).

| Regla | Comportamiento |
|---|---|
| Fin de un spline | `MoveSplineInit::Launch` pone `MOVEFLAG_FORWARD | SPLINE_ENABLED` y el servidor los quita sin avisar al llegar; una unidad vista por primera vez a mitad de ruta los trae en su bloque de creación. WoW.exe al acabar el spline (0x61A836 → 0x7C6C00): `flags &= ~0x8003` y la unidad se queda en el punto final con su orientación; el elector de animación de movimiento (0x5FD100) no mueve nada sin direcciones. Aquí las direcciones se borran al leer el spline (bloque de creación y `SMSG_MONSTER_MOVE`): sin eso, los campesinos de Villanorte (`EMOTE_STATE_WORK_CHOPWOOD`, 234 → animación 136) andaban sin moverse delante del árbol |
| Torso o cuerpo entero | 0x5FE2F0: una animación va solo al torso (desde el hueso clave SpineLow) si está en la tabla de 0x5FED90 (2, 8–36, 46–49, 51–74, 76–78, 80–90, 105–113, 117, 118, 122–125, 128–130, 133, 134, 136–138, 185, 186, 195) y la unidad se mueve **o tiene un estado de pie distinto de 0** (sentada, en silla, arrodillada, dormida); 39, 57, 58, 118 y 187 siempre al cuerpo entero. EmoteEat (61) siempre al torso (0x5FE834): comer y beber sentado. vMaNGOS manda `SMSG_EMOTE` 7 al aplicar Comida/Bebida y el kit 406 / 438 (EmoteEat) cada latido |
| Filtros de los emotes | `SMSG_EMOTE` (0x5E66B0) no anima a una unidad dormida (estado 3) ni nadando; PlayEmote (0x5FCD20) tampoco si canaliza (UNIT_CHANNEL_SPELL) o si ya reproduce esa animación |

### 2.24 Marcas sobre los PNJ (retoques de la fase 20)

Del desensamblado de WoW.exe 1.12.1 (solo lectura); `app/QuestMarks`.

| Regla | Comportamiento |
|---|---|
| Modelos | tabla de 0xC4D9D8 (0x5FA430): 1 `TalkToMe` («!» amarillo), 2 `TalkToMeQuestion_Grey`, 3 `TalkToMeGrey` («!» gris), 4 `TalkToMeGreen`, 5 `TalkToMeBlue` (solo posaderos, desde código que nadie llama), 6 `TalkToMeQuestionMark` («?» amarillo), 7 `TalkToMeQuestion_LTBlue` (`Interface\Buttons\*.mdx` → `.m2`, en `actors.vdpk`) |
| Misiones | `SMSG_QUESTGIVER_STATUS` (0x5DC9F0 → 0x607440; solo unidades con UNIT_NPC_FLAG_QUESTGIVER; los objetos no llevan marca): estado → modelo por la tabla de 0x80C454: 0 nada, 1 → 3, 2 nada, 3 → 2, 4 → 7, 5 → 1, 6 y 7 → 6. Nada si la unidad es hostil (0x607380) o muere (el servidor le borra los NPC flags) |
| Vuelos | `SMSG_TAXINODE_STATUS` (0x5ECDD0): un maestro de vuelo con la ruta sin aprender lleva el 4 (pisa la de misión: una sola por unidad) |
| Colocación | enganchada al punto 18 PlayerName del modelo de la unidad, o al 29 PlayerNameMounted si monta (0x6074C0); su tamaño no cambia con el de la unidad (0x607570 deshace la escala del punto). Animación 0 Stand, o 190 StandHigh (0,517 yd más alta) mientras se ve el nombre de la unidad (0x6076C0). El «!» gira con la unidad; el «?» es un billboard sobre Z (bandera de hueso 0x40). En el mundo 3D: con profundidad y niebla, sin luz. Aquí el nombre (2D, de tamaño fijo) baja lo justo para quedar bajo la marca elevada |

### 2.25 Recursos del cliente en el mundo (retoques de la fase 20)

Del desensamblado de WoW.exe 1.12.1 (solo lectura) y del FrameXML. Regla del proyecto: donde el 1.12 tiene un recurso
(textura, modelo, fuente, sonido) se usa el suyo, no uno dibujado por nosotros.

| Regla | Comportamiento |
|---|---|
| Sombra de un hechizo de lugar | 0x48383B: un cuadro proyectado sobre el suelo, alineado con los ejes del mundo y sin tinte. Estado en 0x83DC2C: 0 aceptable → `Interface\SpellShadow\Spell-Shadow-Acceptable.blp` (las runas verdes) con semilado = radio del hechizo (20 si no tiene); 1 inaceptable (fuera de alcance) → `Spell-Shadow-Unacceptable.blp` con semilado fijo 1,388889 (0x3FB1C71C). `Game` + `SelectionCircle::setWorldAligned` |
| Placas de nombre | las cuatro rutas juntas en las cadenas de WoW.exe: `Interface\Tooltips\Nameplate-Border.blp` (el marco), `Nameplate-Glow.blp` (aditiva, sin alfa: va en `kAdditiveTextures`; WoW.exe la enciende bajo el ratón y pone el nombre en amarillo, 0x7CB850, y lo deshace al salir, 0x7CB8B0: sin ratón, aquí la del objetivo), `Interface\TargetingFrame\UI-TargetingFrame-BarFill.blp` (la vida, teñida por la reacción) y `UI-TargetingFrame-Skull.blp` (en la caja del nivel cuando es demasiado alto para verlo, en vez de «??»). `app/WorldLabels` |
| Números de daño | `Fonts.xml`: `DAMAGE_TEXT_FONT` = `Fonts\FRIZQT__.TTF`, la de los números de daño del motor sobre las unidades. `app/HudWorld` |
| Lo que el 1.12 no tiene | barra de lanzamiento del objetivo (TargetFrame no la tiene en 1.12), barrita de vida bajo los nombres sin placa: quitadas |

### 2.26 Modelos de la interfaz (retoques de la fase 20)

Del desensamblado de WoW.exe 1.12.1 (solo lectura) y del FrameXML; `ui/UiModels`, `tools/baker/UiModels.cpp`.

| Regla | Comportamiento |
|---|---|
| Model frame sin cámara | 0x76D240: el viewport es el rectángulo del frame (recorta); 0x7AD7F0: ortográfica de su tamaño, el origen del modelo en la esquina inferior izquierda, X a la derecha, Y arriba; 0x76D1A0: escala = `NDCToDDCHeight(1) / 0,6` × `SetModelScale` × la escala efectiva del frame (0x774FC0). El `scale` del XML de un `<Model>` es la del modelo (0x76CB36 → `frame+0x3A0`). Resultado: 1 unidad de modelo = 1280 unidades de interfaz × su escala (la recarga: 0,038 × 1280 × 0,75 = 36,5 sobre botones de 36) |
| Recarga | (barras clásicas, bolsas, libro, barra de la mascota; el racimo táctil conserva la nuestra por diseño) `Cooldown.lua`: `UI-Cooldown-Indicator.m2` con escala 0,75; Stand en (1 − restante) × 1000 ms; si se acaba sola, Death una vez (la estrella de `star4.blp`, aditiva); cortarla antes no destella. Sin segundos. Cuatro cuartos de `cooldown.blp` (1-D, negro con alfa 0x99) que giran 250 ms cada uno con una animación de UV en el orden `uv' = C + R·(uv + T − C)` (la traslación antes del giro: el único en que cada cuarto se destapa entero) |
| Autolanzamiento | `UI-AutoCastButton.m2`: sin geometría; 4 emisores sobre 4 huesos que recorren el cuadrado en 2 s, 300 partículas/s, 1 s de vida, `GlowStar.blp` aditiva. Aquí se limitan a 100 por emisor (igual a la vista) |
| Ping del minimapa | `MinimapPing.m2`: Birth y Stand en bucle; `Minimap_OnClick` resta `CURSOR_OFFSET` (−7, −9), que el modelo compensa (+7,7, +8,1). Sin fundido: `SetAlpha(255·t/0,5)` se satura en 0x774E90, así que se ve entero 5,5 s y desaparece |

Pendiente de comprobar: `pack/ParticleFormat.h` y `fx/EffectSet` aplican la traslación de las UV animadas del mundo
**después** del giro (`T·C·R·S·C⁻¹`); el modelo de la recarga indica el orden contrario. Solo importa en los modelos que
animan a la vez el giro y la traslación de una textura.

### 2.27 Interiores: niebla, minimapa y salas (retoques de la fase 20)

Del desensamblado de WoW.exe 1.12.1 (solo lectura); `world/InteriorLighting` (`interiorFog`), `WorldObjects::fogAt` /
`standingAt`, `WmoAreas::inGroup`, `Game::updateLightConditions` / `roomAt`.

| Regla | Comportamiento |
|---|---|
| Niebla de la cámara | 0x69de20: con la cámara en un edificio (grupo de 0x6821f0) y alguno de sus grupos interior (`flags & 0x48 == 0`: ni exterior ni iluminado como fuera), la niebla es la **primera** del MFOG (sin mirar su radio) y sobre ella, de la más lejana a la más cercana (montículo por distancia), las que lista el grupo de la cámara (id ≠ 0) que alcanzan el punto (distancia < radio exterior, 0x69dfdc) y **no** llevan la marca 0x1 (0x69dfe6); cada una interpola fin, escala del inicio y color por su peso (0x69e1c0: 1 dentro del radio interior, lineal hasta el exterior; 0x69efd0). Ninguna otra función lee el MFOG: las nieblas con 0x1 no se usan para la cámara. Bajo agua (agua u océano en la cámara), las mitades submarinas |
| Transición | 0x6cee30: la niebla del frame (para todo lo que se dibuja, también el exterior visto por una puerta) va hacia la del edificio a 0,25 por segundo (0x8115b0, tiempo en segundos) y vuelve igual con la última que tuvo; fin = mín(fin, recorte lejano). Aquí, tras una pantalla de carga ya está puesta |
| Ejemplos | Posada del Orgullo de León (GoldshireInn.wmo): niebla 0 fin 194 yd (inicio 48), color tostado claro; la 1 (marca 0x1, fin 83, melocotón claro) era la que usábamos: el «velo blanco» al mirar la calle. Minas (MD_Goldmine.wmo): niebla 0 azul verdosa, fin 69 yd; la 1 (0x1) blanca, fin 200: desde dentro el exterior se ve azulado como en el 1.12 |
| Dónde está el jugador | el minimapa de interior, el nombre de la sala (rótulo de zona, cabecera del minimapa) y su música usan el grupo de la regla de la cámara (0x6821f0 → 0x6be250: segmento de 1760 yd hacia abajo contra el BSP y los portales, el terreno más cerca gana) desde **una yarda sobre los pies**: desde el suelo mismo el segmento puede pasar por debajo y llegar al terreno (huecos de «fuera» dentro de la posada). Para la sala se conserva un grupo exterior (las calles de Ventormenta son grupos exteriores con nombre en WMOAreaTable); el minimapa solo pasa a interior en un grupo interior con portales. La fila de WMOAreaTable sale de la colocación (MODF) y del índice del grupo; la rejilla de suelos de `worldsound.vdpk` (2 yd, solo el más bajo y el más alto por celda) queda para mundos sin BSP |

Comprobación (`VD_WORLD_CHECK_ROOMS=<yd>`: un personaje sobre muestras del suelo de cada grupo interior con portales,
12 sitios: Villadorada, Ventormenta, Villanorte y su mina, Forjaz, Entrañas, Bahía del Botín, mina de Fargodeep,
Orgrimmar y su cueva, Cima del Trueno, Darnassus, El Cruce y las Minas de la Muerte): con la regla anterior el
minimapa decía «fuera» en el 20–50 % de los suelos de cada interior (la mitad de las Minas de la Muerte, un tercio de
las minas kobold) y la sala fallaba en unos cientos; ahora aciertan todos salvo 26 de ~68.000, que son caras sin
colisión (no se pisan), suelos tapados por otra superficie a menos de una yarda (se pisa la de arriba, exterior) y
umbrales de puerta donde el suelo de fuera coincide con el de la sala.

### 2.28 Experiencia y textos de las barras (retoques de la fase 20)

Del desensamblado de WoW.exe 1.12.1 (solo lectura, `UnitCombatLogXP_C.cpp` y `PlayerName.cpp`) y del FrameXML;
`game/CombatText` (`formatExperience`), `game/Notices`, `GameScreens::readEvents`, `app/HudWorld` (`HudWorldText`),
`ui::fontStringBaseline`.

| Regla | Comportamiento |
|---|---|
| `SMSG_LOG_XPGAIN` | 0x637EC0: si la víctima es el propio jugador, `COMBATLOG_XPLOSS_FIRSTPERSON_UNNAMED` con el total cambiado de signo (0x637F70) y nada más; una muerte (tipo 0) de una unidad que el cliente conoce lleva su nombre (0x60A0C0 guarda la experiencia en la unidad y 0x6251E0 la escribe en el acto); si no (misiones, exploración, una víctima que ya no está), las formas UNNAMED (0x637F90). Línea de tipo 0x2D, CHAT_MSG_COMBAT_XP_GAIN: por defecto en el registro de combate |
| Descanso | 0x637AC0, solo en una muerte con víctima: el estado de descanso del jugador en ese momento (PLAYER_BYTES_2 byte 3) busca su fila de Exhaustion.dbc (factor en +0x08, nombre en +0x14). Si el factor no es 0 ni 1, la clave de la tabla 0x864330 (1 → EXHAUSTION1, 2 → EXHAUSTION2, 3 ninguna, 4 → EXHAUSTION4, 5 → EXHAUSTION5) con (nombre de la víctima, total, total − base con «+» delante si no es negativo, nombre del estado: «Rested»). Con factor 1 (Normal) la línea normal aunque la experiencia traiga bonificación |
| Bono de grupo | lo que da la muerte sola = trunc(base / factor del paquete) (sin descanso: total / factor): igual, sin sufijo; menor, `_GROUP` («+%d group bonus»); mayor, `_RAID` («-%d raid penalty»), con la diferencia. vMaNGOS manda siempre 1,0 |
| «XP: 45» | 0x607260: toda experiencia ganada (no la perdida) pone sobre el jugador un texto del mundo de tipo 4: «%s: %d» con el texto `XP`. Tipos (tabla 0xCE8828, 28 B: subida en yardas, fin de la entrada, inicio de la salida y vida en ms, altura inicial y final en pantalla, color): 4 experiencia (0, 500, 2000, 4500, 0,0183, 0,0183, 148 0 139) y 5 honor (igual con 224 202 10): no suben ni crecen; el alfa lo pone la vida (0x6C82E0) y la sombra va a la mitad. Hasta 4 por unidad (0x6C73F0) y ninguno sobre la unidad de la cámara a menos de 1,8315 yd (primera persona, 0x50D390). Aquí `HudWorldText::Notice` (experiencia y honor; con el contorno de los números de daño y sin el límite de 4) |
| Texto de una FontString | 0x5CA0A6: la línea mide FontHeight (12 en GameFontNormal / Highlight) y su línea base queda a FontHeight × ascender / (ascender + \|descender\|) de su borde superior (FRIZQT: 965 / 1215); justifyV MIDDLE centra las líneas en la caja (como en `CGxString`). CastingBarText: caja de 185 × 16 con su TOP 5 unidades sobre la barra, así que la línea base cae 6,5 unidades bajo el borde superior de la barra y las mayúsculas (0,695 em) tocan el marco de UI-CastingBar-Border. El fondo negro (alfa 0,5) es la barra (195 × 13); la chispa, 2 unidades sobre su centro. MirrorTimer: el texto, sin tamaño propio, cuelga del borde superior del marco |

### 2.29 Subir de nivel, saltar y la recompensa elegida (retoques de la fase 20)

| Regla | Comportamiento |
|---|---|
| Subir de nivel | vMaNGOS (`Player::GiveLevel`) manda `SMSG_LEVELUP_INFO` solo al jugador y sube `UNIT_FIELD_LEVEL` (campo público): no lanza ningún hechizo ni `SMSG_PLAY_SPELL_VISUAL`; al domesticar una mascota la crea un nivel por debajo y la sube «for the levelup visual». El brillo dorado es `SpellVisualEffectName` 21, «HARDCODED Unit Level Up» (`Spells\LevelUp\LevelUp.mdl`, que ningún kit usa): lo pone el cliente cuando una unidad que ya conocía gana un nivel (el jugador, los de alrededor, las mascotas). Aquí `SpellEffects::watchUnits` compara el nivel de cada unidad con el último visto (0: aún no visto, sin efecto) y lo pone una vez a sus pies (`Base`, 19), siguiéndola |
| Saltar | El 1.12 no suena al saltar ni al caer de un salto, salvo la pisada: las filas `Sound\Character\PlayerExertions\<Raza><Sexo>MainJump / MainLand` no las enlaza ninguna tabla, WoW.exe no las nombra (solo las animaciones JumpStart / JumpEnd / JumpLandRun) y las columnas de salto de CreatureSoundData (25–26) están vacías. Una caída que hace daño trae `SMSG_ENVIRONMENTALDAMAGELOG` (su sonido y el quejido de herida) |
| Recompensa elegida | `QuestRewardItem_OnClick` pone `QuestRewardItemHighlight` (UI-QuestItemHighlight, 256 × 64, `alphaMode="ADD"`) 8 unidades a la izquierda y 7 por encima del botón de 147 × 41: su cuadro del icono (téxeles 9..43 × 9..43) cae sobre el icono de 39 desde (1, 2) y el del nombre (49..151 × 7..44) sobre la parte visible de UI-QuestItemNameFrame (11..116 × 11..52, a 1 unidad del icono), una unidad por dentro a lo ancho |

### 2.30 Objetos que no puedes usar (retoques de la fase 20)

| Regla | Comportamiento |
|---|---|
| Competencias | vMaNGOS lanza los pasivos de competencia al cargar el personaje (`LoadFromDB` → `AddSpell` → `CastSpell` → `EffectProficiency` → `SendProficiency`): `SMSG_SET_PROFICIENCY` (u8 clase de objeto, u32 máscara de subclases; armas 2, armaduras 4) llega **antes** de `SMSG_LOGIN_VERIFY_WORLD`. Se olvidan al salir del mundo, no al entrar (como los modificadores de hechizos); borradas al entrar, nunca se conocían y todo parecía utilizable |
| `isUsable` / `canUse` | Las clases y razas del objeto, su nivel requerido y las competencias del jugador (`game::Inventory::usable`) |
| Dónde lo tiñe el 1.12 | `MerchantFrame_UpdateMerchantInfo` (y las recompras): icono 0,9, 0, 0, marco del nombre 1, 0, 0. `QuestFrameItems_Update` (ventana de misión y registro): una recompensa a elegir, icono y marco del nombre 0,9, 0, 0; una que se recibe siempre, icono 0,5, 0, 0 y marco 1, 0, 0. `TradeFrame_UpdateTargetItem` (lo que ofrece el otro): icono y marco 0,9, 0, 0, hueco 1, 0, 0. `AuctionFrame` (explorar y pujas): icono 1, 0,1, 0,1. El botín no |
| Bolsas | `ContainerFrame_Update` **no** tiñe lo que no puedes usar (solo agrisa a 0,5 lo bloqueado y a 0,4 lo que está en reutilización). Aquí sí, en rojo (desviación consciente, como en el vendedor) |

### 2.31 La pantalla de carga (retoques de la fase 20)

Del desensamblado de WoW.exe 1.12.1 (solo lectura, `LoadingScreen.cpp`); `app/LoadingLayout`, `TravelScreens::drawLoading`,
`game::AreaData::gameTip`.

| Regla | Comportamiento |
|---|---|
| Área 4:3 | 0x406A60: el viewport de la pantalla de carga es el mayor rectángulo 4:3 centrado (0x41AD70 da el aspecto frente a 4:3): bandas negras a los lados en una pantalla ancha y arriba y abajo en una alta (la interior del Fold5). El arte de `LoadingScreens.dbc` lo llena |
| Barra | Solo dos texturas: `Loading-BarFill` y encima `Loading-BarBorder` (`Loading-BarBackground`, `-BarGlass` y `-BarGlow` están en los datos pero nada las usa). Tabla de 0x7FFD34 (centro x, centro y desde abajo, ancho, alto, en fracciones del área): borde 0,5, 0,075, 0,6 × 0,05; relleno 0,5, 0,075, 0,525 × 0,025, que crece desde su borde izquierdo con el progreso, estirado (coordenadas de textura fijas 0..1, 0x406280). Nada detrás: en lo que falta por llenar se ve el arte. El borde de 512 × 64 se dibuja a la mitad de su alto respecto a su ancho |
| Consejo | 0x46B5F0, al empezar cada pantalla de carga: si la CVar `showGameTips` (por defecto "1"; la casilla 44 de UIOptionsFrame, «Show Loading Screen Tips») está activa, la fila `gameTip` (por defecto "0"; fuera de rango, la primera) de `GameTips.dbc` (74 filas, texto enUS en la columna 1, por **orden de fila**, no al azar), y `gameTip` pasa a la siguiente y se guarda. Sin la opción, ni consejo ni avance |
| Texto del consejo | 0x406640: quita los saltos de línea del final; FRIZQT (la fuente de la tabla de 0x82F018 por idioma) de alto 0,018 del área; bloque de 515/1024 del ancho centrado, alineado a la izquierda y abajo, con su borde inferior en 0,1 (el borde superior de la barra): las líneas crecen hacia arriba. Gris claro 200, 200, 200 al 84 % (`CImVector` 0xD7C8C8C8) con sombra negra desplazada 0,001 a la derecha y abajo; el «Tip:» trae su propio dorado (`\|cffffd100`) |
| Aquí | El tamaño del consejo es como mínimo el de 13,8 unidades del HUD (en la pantalla exterior del Fold5 el 1,8 % de la altura no se lee), y el bloque se ensancha lo mismo para cortar las líneas donde el 1.12. Una pantalla de carga que deja de dibujarse 2 s ha acabado (la de la lista de personajes y la del mundo son la misma). La barra y el consejo suben fuera de lo que tape el sistema. Debajo de la barra solo un aviso cuando el mapa no tiene pack |

### 2.32 La búsqueda de /who (retoques de la fase 20)

Del desensamblado de WoW.exe 1.12.1 (solo lectura): `SendWho` (tabla de funciones Lua en 0x85D8A0 → 0x5AD3B0) llama al
analizador de 0x5AEBB0, que escribe `CMSG_WHO` (0x62) en 0x5AF0A2..0x5AF168; FrameXML añade solo el valor por defecto
de `/who`. `game/WhoQuery` (con tests), usado por el campo de WhoFrame y por la orden `/who`.

| Regla | Comportamiento |
|---|---|
| Valores por defecto | 0x5AEBBB..0x5AEBEB: niveles 0..100, nombre y hermandad vacíos, máscaras de raza y clase 0xFFFFFFFF, sin zonas ni palabras |
| Texto | Se copia en un búfer de 128 bytes: solo cuentan los 127 primeros (0x5AEBF2), y se le añade un espacio al final (0x5AEC12) que cierra la última palabra. Se lee como UTF-8 (0x41AAB0); los espacios (`iswspace`) separan las palabras. Una `"` abre o cierra un tramo donde el espacio no separa y desaparece (0x5AEC90): `z-"Elwynn Forest"` y `"z-Elwynn Forest"` son la misma palabra. Una palabra dentro de unas comillas sin cerrar se pierde. Nunca pasa a minúsculas |
| `n-` / `g-` | Prefijos sin mayúsculas (0x64A4C0 → `_strnicmp`, solo A-Z): el resto es el nombre o la hermandad (como mucho 47 bytes; gana la última) |
| `z-` | 0x5AED62..0x5AEE16: si aún caben (10 como máximo), añade por orden de fila de `AreaTable` cada zona **de primer nivel** (sin padre, `+8 == 0`) cuyo nombre (columna del idioma) **contiene** el resto, sin mayúsculas (0x64B540). Si después no hay ninguna, añade la zona 0, que no casa con nadie |
| `r-` / `c-` | 0x5AEE53 y 0x5AEF00: la primera deja la máscara en 0; cada fila de `ChrRaces` / `ChrClasses` cuyo nombre contiene el resto suma `1 << id`: `c-war` son guerreros y brujos. Sin coincidencias la máscara queda en 0 (nadie) |
| Niveles | Una palabra que empieza por cifra (0x5AEF7D): mínimo = máximo = ese número; si le sigue `-`, máximo = el número siguiente o 100. `60` es 60..60, `10-20` es 10..20, `10-` es 10..100, `10abc` es 10..10. `-20` (0x5AEFCD) fija solo el máximo. Gana la última; sin comprobar mínimo ≤ máximo |
| Palabras sueltas | 0x5AF015: se mandan tal cual; el cliente no las busca como clase, raza ni zona (el servidor las busca en el nombre, la hermandad y la zona del jugador). Con la cuarta se deja de leer (0x5AF02D): lo que siga, etiquetas y niveles incluidos, se ignora. `""` es una palabra vacía, que vMaNGOS ignora |
| `/who` vacío | `SlashCmdList["WHO"]` usa `WhoFrame_GetDefaultWhoCommand`: `z-"<zona>" <max(nivel-3,1)>-<nivel+3>`, y el texto queda en `WhoFrameEditBox` |
| Aquí | El campo de WhoFrame (`WhoFrameEditBox`, sin texto de ayuda como el original) y la orden `/who` pasan por el mismo analizador; Refresh lo vuelve a mandar y quita la selección (`WhoFrame.selectedWho = nil`) |
