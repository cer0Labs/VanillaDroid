# Controles para móvil: racimo de acciones, plantillas y clases

- **Estado:** aprobada con las decisiones de §11 e implementada en la **fase 20**. Lo que cambió al implementarla
  está en §12; el resto del documento es la propuesta tal como se aprobó.
- **Idea en una frase:** el pulgar derecho no se mueve casi nunca de un **racimo circular** de 9 habilidades + 3
  consumibles. Lo que muestra el racimo **cambia solo** según la postura, el sigilo, la forma, la mascota o la unidad
  que controlas. Lo rellenan **presets por clase, nivel y talentos**, y el jugador lo cambia cuando quiere y lo guarda
  en **plantillas**.

---

## 0. Resumen

1. **Racimo** en la esquina inferior derecha (como la referencia):
   - 1 botón **principal** grande.
   - 5 medianos en **anillo** y 3 pequeños en **columna**.
   - 3 **consumibles** encima.
   - **Saltar** debajo del principal y **montura/viaje** a su derecha.
   - **Objetivo** (cambiar de enemigo) y **plantillas**.
   - Dos botones que **solo aparecen cuando sirven**: **reactivo** (Overpower, Revenge, Execute, Riposte...) e
     **interacción** (atacar, hablar, despojar, usar, resucitar...).
2. **Contenido por capas (contextos):** cada plantilla tiene una capa base y capas para cada postura, forma, sigilo o
   unidad controlada.
   - Una capa **solo guarda lo que cambia**; el resto lo hereda de la base. Así, el sigilo del pícaro cambia 5
     botones y no 12.
   - Las capas del 1.12 salen de `SpellShapeshiftForm.dbc` (columna `BonusActionBar`).
   - Añadimos las que el 1.12 no tenía y que en el móvil hacen falta: acecho del druida, lechúcico, forma de las
     sombras, y cuerpo a cuerpo del cazador.
3. **Cada hueco tiene un papel fijo** (principal = relleno, columna = defensivo...). Al cambiar de postura o forma, el
   mismo hueco muestra la habilidad equivalente: **misma posición, mismo papel**.
4. **Presets por clase:** cada hueco tiene una lista de candidatos y se queda con el primero que conoces. Así funciona
   con nivel 1 y con 60, y con cualquier rama de talentos. Lo nuevo que aprendes entra solo en un hueco libre; lo
   que colocaste a mano no se toca.
5. **Huecos inteligentes:**
   - consumibles automáticos (piedra de salud → poción en combate; comida y bebida fuera);
   - montura o forma de viaje según dónde estés;
   - **grupos** (abanicos) para lo que se elige entre varios: tótems, sellos, auras, aspectos, maldiciones,
     bendiciones, venenos...;
   - **botón de mascota** (tocar = atacar / volver; mantener = sus órdenes y habilidades);
   - **barra de clase** (posturas, formas, tótems).
6. **Plantillas:** varias por personaje (Solo, Mazmorra, JcJ, Sanar...). Cada una guarda **posiciones y contenido**,
   con una geometría por tipo de pantalla (interior cuadrada / exterior y móviles 20:9).
   - Se cambian con un toque.
   - Se guardan en un fichero por personaje, con copia en el servidor (datos de cuenta tipo 6 de vMaNGOS).
7. **Gestos coherentes:** tocar = usar; mantener = repetir, o abrir el abanico si es un grupo; **arrastrar = apuntar**
   (a un marco de unidad para lanzar sobre ella, o al suelo para los hechizos de área).
8. **Principio de juego limpio:** las reglas son las del 1.12 (qué se puede lanzar, cuándo, costes). **Una pulsación
   hace como mucho lo que haría un macro del 1.12**: nada de rotaciones automáticas ni de pociones solas.
9. El **modo clásico** (las barras del 1.12 que hay hoy) sigue como opción. El **cajón** abre tus 6 barras del
   1.12, guardadas en el servidor, para todo lo que no cabe en el racimo.
10. **Coste:** unos 20 botones y ~150 quads, sin asignaciones por frame. El contexto y los presets se resuelven solo
    cuando cambia algo, no cada frame.

---

## 1. Principios

| # | Principio | Consecuencia |
|---|---|---|
| P1 | **El pulgar derecho manda.** Lo frecuente va cerca del principal y lo de una vez por pelea va lejos | Papeles por hueco (§7.1); la columna pequeña es para lo que se pulsa poco |
| P2 | **Misma posición, mismo papel.** Cambiar de postura o forma no debe obligar a buscar | Los presets ponen en el mismo hueco la habilidad equivalente de cada capa (el interrumpir del guerrero está en E en las tres posturas) |
| P3 | **El 1.12 decide qué se puede hacer; nosotros, cómo se pulsa** | Usable / alcance / costes / formas con las reglas ya portadas (`game/ActionBar`). Una pulsación ≤ un macro del 1.12 |
| P4 | **Automático por defecto, editable siempre.** Lo que el jugador toca a mano es suyo | Cada hueco recuerda si es "auto" o "manual"; los presets solo rellenan los "auto" |
| P5 | **Nada cambia sin que se vea** | Un giro corto (~120 ms) en los botones que cambian; un aviso al cambiar de plantilla |
| P6 | **Tamaño físico, no píxeles** | Tamaños en mm convertidos con la densidad real; dos geometrías (cuadrada / ancha) |
| P7 | **Barato** | Nada por frame salvo los estados (enfriamiento, alcance), como hoy; arrays fijos |

---

## 2. Lo que hay hoy

El racimo actual ya es radial; le faltan forma, contenido y persistencia.

- **Disposición** (`app/HudActionBar.cpp:36-51`):
  - Un **botón de contexto** de 76 dp: Atacar / Parar / Hablar / Despojar / Usar
    (`app/GameScreens.cpp:459-497`).
  - 12 botones **cuadrados** de 54 dp en 4 arcos.
  - Saltar, siguiente objetivo y página.
  - Escala `k` para que quepa.
  - Perfil ancho si `ancho ≥ 1,6 × alto`.
- **Contenido:** las 120 casillas del servidor (`SMSG_ACTION_BUTTONS`).
  - La barra principal pasa a la barra de bonificación con la forma: `ActionBar::mainBarSlot`, offsets de
    `SpellShapeshiftForm.dbc`.
    - Gato, Batalla y Sigilo = 1; Defensiva = 2; Oso, Oso temible y Rabiosa = 3.
    - Viaje, Acuática, Lechúcico, Lobo fantasmal y Forma de las sombras = 0.
  - La barra de formas va aparte (`shapeshiftForms()`), y también las MultiBars y la barra de mascota (10 × 34 dp).
- **Toques:**
  - Pulsación larga (450 ms) = tooltip con "Editar".
  - Los hechizos se colocan desde el libro con "Place on bar".
  - El hit test es **rectangular**.
  - El enfriamiento es una **sombra vertical**: hay un barrido radial en `ui/Widgets.cpp:135`, pero es privado.
- **Persistencia:**
  - **No hay posiciones guardadas.**
  - `settings.cfg` solo guarda opciones.
  - Los ficheros por personaje (`WTF\Account\<CUENTA>\<reino>\<personaje>\`) ya existen para macros y chat.
- **Huecos detectados** en la usabilidad (§10.3):
  - no se comprueba `ONLY_STEALTHED` (Ambush, Ravage y Pounce salen usables sin sigilo);
  - tampoco reactivos ni tótems, el objeto equipado necesario (escudo, daga, arma a distancia) ni "solo fuera de
    combate".

---

## 3. La disposición

> **Cambiado tras probarla (§12, «Proporciones»):** los tamaños y posiciones ya no van en milímetros sino en
> proporción a la pantalla, medidos sobre la captura de un juego de móvil comercial. Lo que sigue es la propuesta.

### 3.1 Elementos

| Elemento | Cuántos | Tamaño (cuadrada / ancha) | Qué es |
|---|---|---|---|
| **P** principal | 1 | 16 / 14 mm | La habilidad que más se pulsa |
| **A–E** anillo | 5 | 11,5 / 10 mm | Lo que se pulsa cada pocos segundos |
| **F–H** columna | 3 | 9,5 / 8 mm | Lo que se pulsa una vez por pelea |
| **Consumibles** | 3 | 9 / 7,5 mm | Solo objetos: comida, bebida, pociones, vendas (§6.1) |
| **Saltar** | 1 | 10 / 9 mm | Debajo del principal. Se dispara al tocar, como hoy |
| **Montura / viaje** | 1 | 10 / 9 mm | A la derecha de saltar; solo si tienes con qué (§6.2) |
| **Reactivo** | 0–1 | 11,5 / 10 mm | Aparece a la derecha del principal cuando una habilidad reactiva se vuelve usable (§6.3) |
| **Interacción** | 0–1 | 9 / 8 mm | Abajo a la izquierda del principal: atacar, hablar, despojar, usar, despellejar, resucitar (§6.4) |
| **Objetivo** | 1 | 8 / 7 mm | Siguiente enemigo; mantener = más opciones (§6.5) |
| **Plantillas** | 1 | 8 / 7 mm | A la derecha de los consumibles (el engranaje de la referencia) (§5.3) |
| **Cajón** | 1 | 8 / 7 mm | Tus barras del 1.12 (§6.9) |
| **Mascota** | 0–1 | 9,5 / 8 mm | Cazador, brujo y lo que controles (§6.8) |
| **Barra de clase** | 0–6 | 8 / abanico | Posturas, formas o tótems (§6.7) |

### 3.2 Perfil cuadrado (pantalla interior del Fold5, tabletas)

Es la referencia tal cual, medida sobre la imagen y pasada a mm con el principal a 16 mm.

```
                 [cajón]   (c1)   (c2)   (c3)   [plant.]          ← consumibles
          (mas)
                    [obj]              ( A )        ( B )
           (F)               ( C )

           (G)      ( D )                 (   P   )     [reactivo]
                               [inter]
           (H)      ( E )             [saltar]  [montura]
  ─────────────────────────────────────────────────────────── borde inferior (zona segura)
```

Coordenadas del centro de cada elemento, en mm, con el origen en el **centro del principal**. El principal va a
27 mm del borde derecho y 25 mm del inferior; los huecos libres entre botones son ≥ 1,5 mm.

| | x | y | | x | y |
|---|---|---|---|---|---|
| A | −1 | +24 | Consumible 1 | −20 | +37 |
| B | +15 | +24 | Consumible 2 | −8 | +37 |
| C | −15 | +16 | Consumible 3 | +4 | +37 |
| D | −24 | +1 | Plantillas | +16 | +37 |
| E | −24 | −15 | Cajón | −32 | +37 |
| F | −39,5 | +12 | Objetivo | −30 | +21 |
| G | −39,5 | −3 | Mascota | −39,5 | +27 |
| H | −39,5 | −17 | Saltar | −4 | −16 |
| Reactivo | +16 | +4 | Montura | +11 | −16 |
| Interacción | −14 | −8 | Barra de clase | fila en y = +48, de x = +4 hacia la izquierda | |

**Ocupación en el interior (≈148 × 123 mm):**
- unos 66 × 67 mm, el 45 % del ancho y el 54 % del alto;
- debajo del minimapa, sin tocarlo;
- la zona del stick (el 40 % izquierdo) queda libre.

### 3.3 Perfil ancho (exterior del Fold5 y móviles 20:9)

La exterior mide **≈147 × 57 mm** y un 20:9 de 6,5" unos 151 × 68 mm. La disposición de la referencia mide unos
67 mm de alto y no cabe. El perfil ancho **aplana** el racimo:

```
   (c1) (c2) (c3) [plant.]                       ┌──────────┐
                                                 │ minimapa │
        (F)  [obj]     ( C )    ( A )            └──────────┘
        (G)       ( D )             ( B )
        (H)       ( E )   [inter] (  P  ) [reac]
                                [saltar][mont]
  ───────────────────────────────────────────────────────── borde inferior
```

- El anillo se abre hacia la izquierda (de 100° a 210° en vez de 45°–200°) y **nada de la mitad derecha sube más
  que el borde inferior del minimapa**.
- Los consumibles y las plantillas pasan a una fila arriba a la izquierda del racimo.
- La barra de clase se convierte en un botón con abanico.
- Los números exactos se afinan con capturas de la exterior del Fold5 (`--screen cover`) y del Mi 11X
  (`--screen phone`).

**Escala del jugador:** el racimo entero entre el 70 y el 130 %, y cada elemento entre el 70 y el 150 % en el modo
edición. Por debajo de 7 mm se avisa: es demasiado pequeño para un dedo.

### 3.4 Tamaño físico

- Hoy `dp = densityDpi / 160 × ...`. `densityDpi` es un **cubo** (420, 480...), no la densidad real.
- Para que 16 mm sean 16 mm en los dos móviles se lee `DisplayMetrics.xdpi` por JNI (ya hay JNI para la
  batería) y se guarda como `mmToPx`.
- Si no está o es absurdo, se usa `densityDpi`.
- Plegar o desplegar el Fold5 cambia de geometría (cuadrada ↔ ancha) sin perder nada: la plantilla guarda las dos.

### 3.5 Toques

- **Zonas circulares** con un **margen de 2 mm**. Un toque entre dos botones va al **centro más cercano**: la mitad
  de los fallos de dedo en los móviles son toques en el hueco entre botones.
- **Multitáctil** como hoy:
  - stick con el pulgar izquierdo;
  - racimo con el derecho;
  - cámara con cualquier otro dedo sobre el mundo.
- Un dedo pertenece al botón donde empezó (`ui/Context`, ya es así).
- **Zurdos:** un interruptor refleja todo el HUD en horizontal (stick a la derecha, racimo a la izquierda). Solo se
  reflejan las x de las anclas.

---

## 4. Gestos y respuesta

| Gesto | Habilidad normal | Grupo (abanico) | Hechizo de área (suelo) | Hechizo amistoso |
|---|---|---|---|---|
| **Tocar** | Se lanza **al bajar el dedo** (la respuesta más rápida) | Lanza el actual del grupo (al soltar) | Como hoy (F13): círculo en el suelo y confirmar | Se lanza **al soltar** sobre tu objetivo, o sobre ti con autolanzamiento |
| **Mantener** (≥ 250 ms quieto) | **Repetir**: se vuelve a lanzar en cuanto se puede, como mantener una tecla en el PC | Abre el **abanico**; deslizar y soltar = elegir y lanzar | — | — |
| **Arrastrar** (sale del botón) | — | — | **Apuntar**: el desplazamiento del dedo mueve el círculo dentro del alcance; soltar = lanzar, volver al botón = cancelar | **Arrastrar a un marco** (grupo, banda, objetivo, mascota): se lanza sobre esa unidad **sin cambiar tu objetivo** |

¿Por qué "al bajar" en las ofensivas? Ahorra ~100 ms por pulsación en combate. El precio es que no se pueden
arrastrar, y no lo necesitan. Los amistosos se lanzan al soltar para que el mismo botón sirva para tocar (objetivo
o tú) y para arrastrar a un marco (sanar a otro). Es una opción («Lanzar al tocar / al soltar»).

Los **tooltips** salen en el modo edición, el libro y el cajón (mantener). En combate, mantener es repetir.

**Estados visuales** (las mismas reglas que hoy, en redondo):

| Estado | Cómo se ve |
|---|---|
| Enfriamiento | **Barrido radial** oscuro + segundos en el centro (más de 3 s) |
| GCD | Un **aro fino** que se cierra alrededor del botón (no oscurece el icono: se distingue de un enfriamiento real) |
| Sin maná / rabia / energía | Icono azulado (`kNoManaTint`, como el 1.12) |
| No usable | Gris |
| Fuera de alcance | Rojo (como hoy) |
| Activo (ataque, disparo automático, siguiente golpe en cola, aura de la forma) | Aro dorado que late (el `flash` del 1.12) |
| Reactivo disponible | Brillo `IconBorder-GlowRing` + **vibración corta** |
| Cargas / cuentas | Abajo a la derecha: objetos, reactivos (fragmentos de alma, runas), munición en Auto Shot |
| Recién colocado | Insignia "Nuevo" 10 s |
| Pulsado | El botón se encoge al 92 % mientras dura el dedo |

**Puntos de combo** (pícaro, druida felino): 5 gemas (`ComboPoint` del 1.12) en un arco sobre el principal.

---

## 5. El contenido

### 5.1 Qué cabe en un hueco

| Tipo | Detalle |
|---|---|
| **Hechizo** | Se guarda la **línea** (el id del rango 1): se lanza siempre el rango más alto que conoces. Opcional: rango fijo (bajar de rango para sanar barato) |
| **Objeto** | Por id; la cuenta, de todas las bolsas (`Inventory::count`) |
| **Macro** | Las del 1.12 (`macros-cache.txt`), como hoy |
| **Grupo** | Lista de hasta 8 acciones + la actual (§6.6). **Automático** (lo rellena la clase: "todas tus auras") o **fijo** |
| **Acción de mascota** | Una casilla de la barra de mascota (Spell Lock, Seduction...) |
| **Especiales** | Consumible automático (§6.1), Montura / viaje (§6.2), Mascota (§6.8), Formas (§6.7), Atacar (6603), Interacción (§6.4), Cajón |

### 5.2 Contextos (capas)

El racimo muestra la plantilla activa con **la capa del contexto actual encima de la base**. Un hueco vacío en la
capa **hereda** de la base, y un hueco marcado "nada" se oculta.

**Orden de prioridad** (gana el primero que se cumple):

| # | Contexto | Cómo se detecta | Qué muestra |
|---|---|---|---|
| 1 | **Controlas otra unidad** (Mind Control, Eyes of the Beast, Eye of Kilrogg, posesión) | `MoverControl`: el mover activo no eres tú; `SMSG_PET_SPELLS` de posesión | Sus habilidades (la barra de mascota) en los 9 huecos, más un botón **Salir** (cancela la canalización) en el sitio del reactivo |
| 2 | **Sin control** (miedo, aturdido, hechizado) | `playerHasControl() == false` | El racimo atenuado; solo lo que rompe el control (Will of the Forsaken, Berserker Rage, un abalorio) sigue vivo |
| 3 | **Muerto / fantasma** | Banderas del jugador | Habilidades ocultas. El principal es **Liberar espíritu**; de fantasma junto al cuerpo, **Resucitar**; y Reincarnation o Soulstone si las hay (hoy son popups) |
| 4 | **Forma / postura / sigilo** | `UNIT_FIELD_BYTES_1` byte 2. **Acecho**: forma felina + el aura de la línea de Prowl | Capa de esa forma. Las del 1.12 tienen capa por defecto: Batalla, Defensiva, Rabiosa, Sigilo, Felino, Oso. Además: Acecho, Lechúcico, Sombras, Viaje / Acuática / Lobo fantasmal |
| 5 | **Cuerpo a cuerpo del cazador** (opcional) | Objetivo a alcance cuerpo a cuerpo; se vuelve a la base a ≥ 8 m (con histéresis en la zona muerta de 5–8 m) | Capa melé: Raptor Strike, Wing Clip, Mongoose Bite, Disengage |
| 6 | **Base** | — | — |

Detalles:
- El sigilo del pícaro es la **forma** 30 (regla del 1.12), no la bandera de sigilo. Si no, Shadowmeld del elfo de
  la noche cambiaría la capa a cualquier clase.
- Las capas extra (Acecho, Lechúcico...) empiezan **vacías**: lo heredan todo y los presets solo rellenan las que
  tienen sentido.
- Una capa se puede **desactivar**. Por ejemplo, el druida equilibrio que no quiere capa de lechúcico.

**Modificador por hueco: en combate / fuera de combate.** Solo lo usan los consumibles (§6.1) y, si el jugador
quiere, cualquier hueco en el modo edición (p. ej. "fuera de combate: Mark of the Wild"). No se usa en los presets:
cambiar habilidades al entrar en combate rompería P2.

### 5.3 Plantillas

- **Plantilla** = nombre + **geometría** (cuadrada y ancha) + **contenido** (base + capas + grupos).
- Hasta 6 por personaje.
- Los presets de clase traen 1–3 según los papeles de la clase (§7): p. ej. sacerdote "Daño" y "Sanar"; guerrero
  "Armas/Furia" y "Tanque".
- El **botón Plantillas**:
  - **tocar** = lista con los nombres (la activa marcada) + "Editar…" + "Nueva…";
  - **deslizar a izquierda / derecha** = anterior / siguiente sin abrir nada (sirve en combate);
  - al cambiar sale un aviso corto con el nombre.
- Operaciones: nueva (copia de la actual), renombrar, duplicar, borrar, "Volver al preset de la clase" y "Copiar esta
  disposición a todas mis plantillas" (solo la geometría).
- **Cambio automático por sitio** (opcional, apagado): una plantilla para mundo, mazmorra, banda o campo de batalla.
  `game::Travel` ya sabe el tipo de mapa.
- En una plantilla de sanador también se pueden mover los **marcos de grupo y de banda** junto al racimo (§8): sanar
  arrastrando hasta la esquina superior izquierda del Fold5 abierto son 10 cm.

### 5.4 Rangos

- El 1.12 ya cambia el rango en sus barras con `SMSG_SUPERCEDED_SPELL`. Aquí es más sencillo: el hueco guarda la
  línea y siempre lanza el rango más alto.
- Las líneas (rango 1 → 2 → ...) salen de `SkillLineAbility.dbc` (`SupercededBySpell`).
- Para **bajar de rango** (sanadores): en el modo edición, "Rango fijo". Un grupo puede tener varios rangos de lo
  mismo (Heal 2 y Greater Heal 1).

### 5.5 Dónde se guarda

- **Fichero de texto por personaje:** `WTF\Account\<CUENTA>\<reino>\<personaje>\touch-layout.txt`, junto a sus
  macros. Lo escribe el hilo de escritura en segundo plano que ya usan los ajustes.
- **Copia en el servidor:** vMaNGOS guarda por personaje los datos de cuenta "per character" en
  `character_account_data`. El tipo **6 (`PER_CHARACTER_LAYOUT`, el `layout-cache.txt` del 1.12)** no lo usamos
  hoy y le pega: es "la disposición".
  - Va con una primera línea `VDTOUCH 1`.
  - Si un día entras con el cliente 1.12 del PC, este sobrescribirá el tipo 6 con su propio layout. Al volver, el
    móvil ve que no empieza por `VDTOUCH`, se queda con su copia local y la vuelve a subir.
  - Límite del servidor: 64 KB descomprimido; una plantilla ocupa ~2 KB.
- Así las plantillas **siguen al personaje** entre el Fold5 y el Mi 11X y sobreviven a reinstalar.

Ejemplo del formato (legible a propósito, para poder tocarlo a mano):

```
VDTOUCH 1
active "Solo"
template "Solo"
  geometry square scale 1.0
    P 0 0 16
    A -1 24 11.5
    ...
  geometry wide scale 1.0
    ...
  layer base
    P spell 1752 auto          # Sinister Strike (línea)
    A spell 2098 auto          # Eviscerate
    D group auto finishers     # Kidney Shot / Rupture / Expose Armor
    c1 consumable health
    c2 consumable mana
    c3 item 6450               # Silk Bandage (fijo)
  layer stealth
    P spell 8676 auto          # Ambush
    A spell 1833 manual        # Cheap Shot (colocado a mano)
```

---

## 6. Huecos inteligentes

### 6.1 Consumibles (los 3 de arriba)

Cada uno es **automático por categoría** o **fijo** (un objeto concreto). El tipo sale del **hechizo al usar** del
objeto en `Spell.dbc`, no de listas de ids: el servidor oculta la subclase de los consumibles.

| Categoría | Se reconoce por | En combate | Fuera de combate |
|---|---|---|---|
| **Vida** | Efecto HEAL instantáneo (pociones, categoría 4; piedra de salud, categoría 30) | La **piedra de salud** si está lista; si no, la mejor **poción** lista (enfriamientos separados en el 1.12: una tras otra) | **Comida**: aura MOD_REGEN con `CANT_USED_IN_COMBAT` (Food, 433) |
| **Maná** (clases con maná) | ENERGIZE maná (pociones, gemas de maná) | **Gema de maná** si está lista, si no **poción** | **Bebida**: MOD_POWER_REGEN maná (Drink, 430) |
| **Rabia / energía** (guerrero, pícaro) | ENERGIZE rabia (Rage Potion) / energía (Thistle Tea) | La mejor lista | Pasa a **Venda** |
| **Venda** | Canalización de Primeros auxilios (habilidad 129) | La mejor que permita tu nivel de Primeros auxilios; gris con el tiempo de Recently Bandaged | Igual |

Reglas para elegir:
- Se descarta lo que no puedes usar (nivel, habilidad).
- Primero lo **conjurado** (desaparece).
- Luego **lo que más restaura**: puntos base + dado del efecto, × ticks en la comida.
- La comida con bonificación ("Well Fed") **no se usa sola**; se fija a mano.
- El botón muestra el icono y la cuenta de lo que **se usaría ahora**. Si no hay nada, el icono de la categoría
  atenuado, para que la disposición no salte.
- Mantener sobre el botón = lista de los candidatos de tus bolsas para **fijar** uno.

**Por defecto:**
- con maná: Vida / Maná / Venda;
- guerrero: Vida / Rabia / Venda;
- pícaro: Vida / Energía / Venda.

Fuera de combate, Vida y Maná son comer y beber: dos toques y te sientas a comer y beber a la vez, como en el 1.12.

### 6.2 Montura / viaje (a la derecha de saltar)

Solo se ve si tienes algo con que viajar:

| Situación | El botón |
|---|---|
| Montado | **Bajar** (cancela el aura de montura, como el BuffFrame) |
| Nadando (druida) | Aquatic Form |
| Tienes montura y puedes montar (fuera, sin combate) | La **más rápida** que permite tu Riding: objetos con aura MOUNTED (78) y la velocidad del aura 32, o las de clase (Summon Warhorse/Charger, Felsteed/Dreadsteed). Mantener = elegir y **fijar** una favorita |
| Sin montura: druida | Travel Form (fuera) / Cat Form (dentro) |
| Sin montura: chamán | Ghost Wolf |
| Sin montura: cazador | Aspect of the Cheetah |
| Dentro, en combate | Atenuado (el 1.12 no deja) |

Dentro / fuera = la bandera de interior de los WMO que ya usa la luz (F11).

### 6.3 Reactivo (a la derecha del principal)

- Aparece con brillo, sonido (`igBonusBarOpen`) y vibración cuando **una habilidad que conoces y que depende de un
  estado de aura** pasa a usable.
- Es **genérico**: son los hechizos con `casterAuraState` / `targetAuraState` en `Spell.dbc`, que `spellUsable` ya
  evalúa. Overpower, Revenge, Execute, Riposte, Mongoose Bite, Counterattack, Hammer of Wrath... salen solos, sin
  lista a mano.
- Si la habilidad ya está en un hueco visible, **brilla allí** y el reactivo no aparece (nada duplicado).
- Si hay dos a la vez, gana la de enfriamiento más largo; mantener = abanico con las dos.

### 6.4 Interacción (abajo a la izquierda del principal)

Es el **botón de contexto de hoy**, más pequeño y fuera del principal:

- Atacar / Parar con un enemigo seleccionado. Útil para paladín y cazador, y para **no romper un control** (parar).
- **Despojar**, si el objetivo muerto tiene botín; **despellejar / herboristería / minería** si se puede.
- **Hablar** con un PNJ.
- **Usar** un objeto cercano (cofre, buzón, nodo, el corcho de pesca).
- **Resucitar** (nuevo): si el objetivo es un jugador amigo muerto, el hechizo de tu clase: Resurrection,
  Redemption, Ancestral Spirit, o Rebirth en combate.

Se oculta cuando no hay nada. Tocar al objetivo ya seleccionado en el mundo sigue haciendo lo mismo, como hoy.

### 6.5 Objetivo

- **Tocar** = siguiente enemigo (`Hud::nextEnemy`), con tres mejoras:
  1. primero los que tienes **delante**;
  2. **se salta los controlados con algo que se rompe con el daño** (Polymorph, Sap, Freezing Trap, Hibernate,
     Seduction, Shackle, Blind, Gouge): auras con interrupción por daño. Es genérico, sale del `Spell.dbc`;
  3. primero los que **te atacan a ti o a tu grupo**.
- **Deslizar** arriba / abajo = siguiente / anterior.
- **Mantener** = abanico: enemigo más cercano · siguiente amigo del grupo · objetivo de tu objetivo (asistir) · tu
  mascota · tú.

### 6.6 Grupos (abanicos)

Para lo que se elige entre varias opciones parecidas y se usa una a la vez: tótems por elemento, sellos, auras,
bendiciones, aspectos, maldiciones, armaduras, polimorfias, trampas, gritos, venenos, demonios...

- **Tocar** = la **actual**: la última elegida, que es la que muestra el icono. Un triangulito en el borde dice que
  es un grupo.
- **Mantener** (o salir del botón deslizando) = se abre un **abanico** de hasta 8 hacia el centro de la pantalla.
  Se desliza hasta una y al soltar queda como actual y **se lanza**. Soltar sobre el propio botón = cancelar.
- **Automáticos:** el preset dice "todas tus auras" y el grupo se rellena con lo que sabes (al aprender una aura
  nueva, entra). Un grupo fijo lo hace el jugador en el modo edición.
- Si el grupo es de cosas con estado (aura, aspecto, sello activo), el icono muestra **la activa** con su aro
  dorado. Tocar la vuelve a poner si se ha caído.

### 6.7 Barra de clase

La barra de formas del 1.12 (`shapeshiftForms()`) en redondo, más lo que en el móvil funciona igual:

| Clase | Qué lleva |
|---|---|
| Guerrero | Las 3 posturas |
| Druida | Oso, Felino, Viaje, Acuática, Lechúcico (cancelar la activa = volver a casters) |
| Chamán | **4 botones de tótem**, uno por elemento, cada uno un grupo (Tierra / Fuego / Agua / Aire) |
| Sacerdote | Shadowform (si la tiene) |
| Pícaro | Nada por defecto: Stealth va en el racimo. Activable |
| Paladín, cazador, mago, brujo | Nada por defecto: sus auras, aspectos y armaduras van en grupos del racimo. Activable |

En el **perfil cuadrado** es una **fila** sobre los consumibles (un toque por postura, como el 1.12). En el **ancho**
es **un botón** que abre un abanico.

### 6.8 Mascota

Un botón con el retrato de la mascota y su felicidad (verde / amarilla / roja, `UNIT_FIELD_POWER5`):

| Estado | Tocar | Mantener |
|---|---|---|
| Sin mascota (cazador) | Call Pet; si murió, **Revive Pet** | — |
| Sin mascota (brujo) | Invoca el **último demonio** | Abanico: Imp / Voidwalker / Succubus / Felhunter |
| Con mascota | **Atacar a mi objetivo**. Si ya lo está atacando: **Volver** (Follow) | **Panel de mascota**: Atacar · Seguir · Quieto · Agresiva / Defensiva / Pasiva · sus 4 habilidades (mantener en una = autolanzamiento) · Mend Pet · Feed Pet · Dismiss |
| Controlas a otra unidad | Contexto 1 (§5.2): ocupa el racimo entero | — |

- Las **habilidades de mascota que importan a tiempo** (Spell Lock, Seduction, Devour Magic, Sacrifice) se pueden
  poner en cualquier hueco del racimo como "acción de mascota". El 1.12 no lo deja; aquí es solo dónde está el
  botón, el paquete es el mismo (`CMSG_PET_ACTION`).
- **Feed Pet** abre las comidas de tus bolsas. El cliente no sabe la dieta de cada objeto (el servidor no la manda),
  así que recuerda la última comida aceptada por familia y descarta la que el servidor rechace
  (`SPELL_FAILED_WRONG_PET_FOOD`).
- La barra de mascota del 1.12 se puede seguir mostrando (opción) a la izquierda del racimo.

### 6.9 Cajón

**El cajón son tus barras del 1.12**: las 6 páginas de 12 de las 120 casillas del servidor, en una rejilla a media
pantalla.

- Tocar = usar y se cierra; "mantener abierto" opcional.
- Se edita como hoy (libro → colocar).
- Es donde vive lo que no cabe en el racimo y no es de combate: portales, teletransportes, conjurar, Piedra de
  hogar, oficios, rastreos, Ritual of Summoning, venenos, Pick Lock...
- No hay que inventar nada: está guardado en el servidor, lo rellena vMaNGOS en un personaje nuevo y sigue sirviendo
  si un día juegas desde el PC.

---

## 7. Clases

### 7.1 Papeles de los huecos y cómo se rellenan

| Hueco | Papel | Por qué ahí |
|---|---|---|
| **P** | **Relleno**: lo que más pulsas | Donde descansa el pulgar |
| **A** (arriba) | **Golpe fuerte** (enfriamiento corto) o la segunda más usada | El movimiento más natural del pulgar |
| **C** (arriba-izq.) | **Finalizador** / la tercera | Cerca |
| **D** (izquierda) | **Área** o la cuarta | Cerca |
| **E** (abajo-izq.) | **Interrumpir / controlar** | Un golpe de pulgar rápido hacia abajo |
| **B** (arriba-dcha.) | **Abrir / acercarse / tirar** | Se usa al empezar; doblar el pulgar hacia atrás cuesta más |
| **F** | **Mejoras** (casi siempre un grupo) | Una vez por pelea o menos |
| **G** | **Defensivo** | Una vez por pelea |
| **H** | **Ofensivo / racial** | Una vez por pelea |

**Algoritmo:**
- Cada hueco del preset tiene una **lista de candidatos** (`›` = "si no, el siguiente").
- Se resuelven en orden P, A, C, D, E, B, F, G, H. Cada uno se queda con **el primer candidato que conoces y que no
  está ya puesto**.
- Así los talentos se resuelven solos. Por ejemplo, A = `Mortal Strike › Bloodthirst › Rend`: el de Armas tiene
  Mortal Strike, el de Furia Bloodthirst y uno de nivel 8 Rend.
- Algunos presets dependen de la **rama de talentos con más puntos** (Fireball o Frostbolt en el mago).
- Los candidatos que requieren arma se comprueban con el equipo: Backstab y Ambush necesitan daga; Shield Bash,
  escudo.
- Se vuelve a resolver al **aprender** algo, al **reiniciar talentos** y al **cambiar de arma**, solo en los huecos
  "auto".

Las tablas que siguen son la **primera propuesta**: se afinan jugando cada clase. Los nombres van en inglés, como los
muestra el cliente enUS. ⟨…⟩ = grupo (el primero es el actual por defecto).

### 7.2 Guerrero

Capas del 1.12: **Batalla (1), Defensiva (2), Rabiosa (3)**. Casi siempre está en una; la base copia la de Batalla
por si no lo está.

| | Battle | Defensive | Berserker |
|---|---|---|---|
| P | Heroic Strike | Heroic Strike | Heroic Strike |
| A | Mortal Strike › Bloodthirst › Rend | Shield Slam › Sunder Armor | Bloodthirst › Mortal Strike › Whirlwind |
| C | Rend › Sunder Armor | Revenge | Whirlwind › Slam |
| D | Thunder Clap ⟨Cleave, Sweeping Strikes⟩ | Shield Block | Cleave |
| E | Hamstring | Shield Bash | Pummel |
| B | Charge | Taunt ⟨Mocking Blow, Challenging Shout⟩ | Intercept |
| F | ⟨Battle Shout, Demoralizing Shout, Intimidating Shout⟩ | igual | igual |
| G | Retaliation | Shield Wall ⟨Last Stand⟩ | Berserker Rage |
| H | Bloodrage ⟨Death Wish, Recklessness, racial⟩ | igual | igual |

Casuística:
- **Overpower y Execute** salen en el **reactivo**. Revenge está en C porque el tanque la pulsa siempre.
- **Heroic Strike y Raptor Strike no empiezan el ataque automático** en el 1.12 (no tienen `INITIATES_COMBAT`;
  Sinister Strike, Cleave, Claw o Maul sí: corregido al implementarlo, §12). Pulsados sin estar atacando no hacen nada.
  Asistencia: empiezan el ataque, como el "macro de atacar" que usaba todo el mundo en el 1.12 (§8).
- **Asistencia de posturas:** pulsar Overpower en Rabiosa cambia a Batalla y la lanza después (§8). Ojo: al cambiar
  se pierde la rabia por encima de lo que guarda Tactical Mastery.
- Charge solo fuera de combate e Intercept solo en Rabiosa: B cambia con la postura, P2 se cumple (B = acercarse).

### 7.3 Pícaro

Capas: **base** y **Sigilo (1)**.

| | Base | Stealth |
|---|---|---|
| P | Hemorrhage › Sinister Strike (› Backstab con daga, opcional) | Ambush (daga) › Cheap Shot |
| A | Eviscerate | Cheap Shot › Garrote |
| C | Slice and Dice | Garrote |
| D | Kidney Shot ⟨Rupture, Expose Armor⟩ | Sap |
| E | Kick ⟨Gouge, Blind⟩ | Pick Pocket |
| B | Stealth | Stealth (salir) ⟨Distract⟩ |
| F | ⟨Blade Flurry, Adrenaline Rush, Cold Blood, Ghostly Strike, Preparation⟩ › Sprint | hereda |
| G | Evasion ⟨Feint, Sprint⟩ | hereda |
| H | Vanish ⟨racial⟩ | Premeditation › hereda |

Casuística:
- **Puntos de combo** alrededor de P. Los finalizadores (A, C, D) se ven grises sin combos, como en el 1.12.
- Riposte (talento) sale en el **reactivo**.
- **Ambush, Garrote y Cheap Shot** requieren sigilo (`ONLY_STEALTHED` / postura): hoy no se comprueba y hay que
  arreglarlo (§10.3).
- **Venenos:** en el cajón. Más adelante, un botón "Venenos" que los aplica a la mano principal y a la secundaria
  en dos toques, con el flujo de objetos sobre objetos de F13.
- Pick Lock y Detect Traps: en el cajón.

### 7.4 Druida

Capas del 1.12: **Felino (1), Oso (3)**. Añadidas: **Acecho** (felino + Prowl), **Lechúcico**, **Viaje / Acuática**.
Las de forma de viaje se dejan heredar: en el 1.12 casi nada se puede lanzar en forma de viaje y se ve gris. Con la
asistencia de formas, tocar un hechizo de caster sale de la forma (opcional, apagado).

| | Base (caster) | Cat | Prowl (cat) | Bear |
|---|---|---|---|---|
| P | Wrath | Claw › Shred | Ravage › Pounce | Maul |
| A | Moonfire | Rake | Pounce | Faerie Fire (Feral) › Demoralizing Roar |
| C | Insect Swarm › Starfire | Ferocious Bite › Rip | Shred | Demoralizing Roar |
| D | Hurricane | Rip | hereda | Swipe |
| E | Entangling Roots ⟨Hibernate⟩ | Faerie Fire (Feral) | hereda | Bash |
| B | Faerie Fire | Prowl | Prowl (salir) | Growl ⟨Feral Charge, Challenging Roar⟩ |
| F | ⟨Mark of the Wild, Thorns, Omen of Clarity⟩ | Tiger's Fury | hereda | Enrage |
| G | Barkskin ⟨Rejuvenation, Regrowth, Healing Touch⟩ | Dash | hereda | Frenzied Regeneration |
| H | Innervate ⟨Rebirth, Nature's Swiftness⟩ | Cower | hereda | hereda |

- **Plantilla "Sanar"** (restauración): P Healing Touch · A Rejuvenation · C Regrowth · D Tranquility · E ⟨Remove
  Curse, Abolish Poison⟩ · B Swiftmend › Nature's Swiftness · F mejoras · G Barkskin · H Innervate ⟨Rebirth⟩.
- Las formas van en la **barra de clase**. **Viaje / Acuática** van en el botón de **montura / viaje** (§6.2).
- **Lechúcico:** por defecto sin capa: hereda la base y las curaciones salen grises, que es la regla del 1.12.

### 7.5 Cazador

Base = a distancia; capa opcional **cuerpo a cuerpo** por distancia (§5.2).

| | Base (distancia) | Melee |
|---|---|---|
| P | Auto Shot (con la cuenta de munición) | Raptor Strike |
| A | Aimed Shot › Arcane Shot | Wing Clip |
| C | Arcane Shot › ⟨Serpent Sting, Viper Sting, Scorpid Sting⟩ | Mongoose Bite |
| D | Multi-Shot ⟨Volley⟩ | hereda |
| E | Concussive Shot ⟨Scatter Shot, Wyvern Sting⟩ | Wing Clip › hereda |
| B | Hunter's Mark | Disengage |
| F | ⟨Aspect of the Hawk, Monkey, Cheetah, Pack, Beast, Wild⟩ | hereda |
| G | Feign Death ⟨Freezing Trap, Frost Trap, Immolation Trap, Explosive Trap⟩ | Deterrence › hereda |
| H | Rapid Fire ⟨Bestial Wrath, racial⟩ | hereda |

- **Mascota:** el botón de mascota (§6.8) con Call / Revive / Mend / Feed / Dismiss y la felicidad. Intimidation y
  Bestial Wrath son del jugador, en H.
- **Eyes of the Beast:** contexto "controlas otra unidad". El stick mueve a la mascota (MoverControl de F13).
- **Trampas en el grupo de Feign Death:** en el 1.12 no se ponen trampas en combate, salvo justo después de Feign
  Death.
- **Zona muerta 5–8 m:** en la capa a distancia los disparos salen en rojo dentro de 8 m (la regla de alcance
  mínimo que ya hay). La histéresis evita que la capa parpadee.
- Rastreos: en el **botón de rastreo del minimapa** (tocar = abanico con todos tus rastreos, también Find
  Herbs/Minerals y el Find Treasure del enano).

### 7.6 Mago

Solo base. Hay preset "Escarcha" y "Fuego/Arcano" según la rama con más puntos; sin talentos, Frostbolt desde el
nivel 4.

| Hueco | Contenido |
|---|---|
| P | Frostbolt (Escarcha) / Fireball (Fuego, Arcano) |
| A | Fire Blast |
| C | Arcane Missiles › Scorch · (Escarcha: Cone of Cold) · con varita y poco nivel: Shoot |
| D | ⟨Arcane Explosion, Blizzard, Flamestrike, Cone of Cold⟩ |
| E | Counterspell |
| B | ⟨Polymorph, Polymorph: Pig, Polymorph: Turtle⟩ |
| F | ⟨Frost Armor/Ice Armor, Mage Armor, Arcane Intellect, Dampen Magic, Amplify Magic, Remove Lesser Curse⟩ |
| G | Frost Nova ⟨Blink, Ice Block, Ice Barrier, Mana Shield, Fire Ward, Frost Ward⟩ |
| H | Evocation ⟨Presence of Mind, Arcane Power, Combustion, Cold Snap, racial⟩ |

- **Gemas de maná** → consumible Maná. **Agua y comida conjuradas** → Vida / Maná fuera de combate, conjurado
  primero.
- Blizzard y Flamestrike: **apuntar arrastrando** (§4).
- Blink: hacia donde miras, como el 1.12.

### 7.7 Brujo

Base + **mascota** (demonios; Enslave Demon convierte en mascota) + **Eye of Kilrogg** (contexto "controlas otra
unidad").

| Hueco | Contenido |
|---|---|
| P | Shadow Bolt |
| A | Corruption |
| C | Immolate ⟨Searing Pain, Conflagrate, Shadowburn⟩ |
| D | ⟨Rain of Fire, Hellfire⟩ |
| E | Fear ⟨Howl of Terror, Death Coil, Banish⟩ |
| B | ⟨Curse of Agony, Weakness, Recklessness, Tongues, Elements, Shadow, Exhaustion, Doom⟩ |
| F | ⟨Demon Skin/Demon Armor, Unending Breath, Detect Invisibility, Shadow Ward⟩ |
| G | Life Tap ⟨Health Funnel, Dark Pact, Drain Life, Drain Mana⟩ |
| H | Drain Soul ⟨Shoot, racial⟩ |

- La **cuenta de fragmentos de alma** en cada botón que los gasta (reactivo del `Spell.dbc`) y en el de mascota.
  Gris si faltan.
- Healthstone: crear, en el cajón; usar, en el consumible Vida. Soulstone, Ritual of Summoning e invocar Infernal o
  Doomguard: en el cajón.
- Spell Lock y Seduction: se recomienda ponerlas en un hueco del racimo como acción de mascota (§6.8).

### 7.8 Sacerdote

Base + capa **Forma de las sombras** (añadida) + **Mind Control** (contexto 1). Dos presets:

| Hueco | "Sanar" | "Daño / Sombras" | capa Shadowform |
|---|---|---|---|
| P | Flash Heal › Lesser Heal | Smite › Mind Flay | Mind Flay |
| A | Renew | Mind Blast | Mind Blast |
| C | Greater Heal › Heal | Shadow Word: Pain | Shadow Word: Pain |
| D | Prayer of Healing | Holy Nova | Vampiric Embrace |
| E | ⟨Dispel Magic, Cure Disease, Abolish Disease⟩ | Silence › Psychic Scream | hereda |
| B | Power Word: Shield | Power Word: Shield | hereda |
| F | ⟨PW: Fortitude, Divine Spirit, Shadow Protection, Inner Fire, Prayer of Fortitude⟩ | igual | hereda |
| G | Fade ⟨Psychic Scream⟩ | Fade ⟨Power Word: Shield⟩ | hereda |
| H | Inner Focus ⟨Power Infusion, **racial de sacerdote**⟩ | Shoot (varita) ⟨racial de sacerdote⟩ | hereda |

**Razas:** el sacerdote del 1.12 tiene **hechizos de clase según la raza**. Entran solos como candidatos porque
solo los conoce quien puede:
- Desperate Prayer: humano, enano.
- Fear Ward: enano.
- Feedback: humano.
- Starshards y Elune's Grace: elfo de la noche.
- Touch of Weakness y Devouring Plague: no-muerto.
- Hex of Weakness y Shadowguard: trol.

**La varita (Shoot)** es el relleno real del sacerdote, el mago y el brujo cuando suben de nivel. Va en el racimo
mientras haya una varita equipada.

### 7.9 Paladín

Solo base. El paladín del 1.12 vive del **ataque automático**: el principal es el **sello** (tocar = volver a
ponerlo) y A el **juicio**. Tres presets:

| Hueco | "Retribución / Solo" | "Sanar" | "Proteger" |
|---|---|---|---|
| P | ⟨Seal of Righteousness, Command, Crusader, Justice, Light, Wisdom⟩ | Flash of Light | ⟨sellos⟩ |
| A | Judgement | Holy Light | Judgement |
| C | Flash of Light ⟨Holy Light⟩ | Holy Shock | Holy Shield |
| D | ⟨Consecration, Exorcism, Holy Wrath⟩ | ⟨Consecration⟩ | Consecration |
| E | Hammer of Justice | ⟨Cleanse, Purify⟩ | Hammer of Justice |
| B | ⟨Blessings: Might, Wisdom, Kings, Salvation, Light, Sanctuary, Freedom, Sacrifice⟩ | igual | igual |
| F | ⟨Auras: Devotion, Retribution, Concentration, resistencias, Sanctity⟩ | igual | igual |
| G | Divine Protection › Divine Shield ⟨Blessing of Protection⟩ | igual | igual |
| H | Lay on Hands ⟨Divine Favor, racial⟩ | Divine Favor ⟨Lay on Hands⟩ | Lay on Hands |

- Hammer of Wrath (por debajo del 20 %) sale en el **reactivo**. Exorcism, solo contra no-muertos y demonios: gris
  con los demás (`targetCreatureType`).
- Monturas de clase en **montura**. Redemption en **interacción** (§6.4).

### 7.10 Chamán

Solo base; **Ghost Wolf** en montura / viaje; **4 grupos de tótems** en la barra de clase.

| Hueco | "Elemental / Solo" | "Mejora" | "Sanar" |
|---|---|---|---|
| P | Lightning Bolt | Stormstrike › Attack | Lesser Healing Wave › Healing Wave |
| A | Flame Shock | Earth Shock | Healing Wave |
| C | Chain Lightning | Flame Shock | Chain Heal |
| D | ⟨tótem de Fuego⟩ | Lightning Bolt | ⟨Cure Poison, Cure Disease⟩ |
| E | Earth Shock ⟨Frost Shock, Purge⟩ | Frost Shock ⟨Purge⟩ | Earth Shock ⟨Purge⟩ |
| B | Lightning Shield | Lightning Shield | Lightning Shield |
| F | ⟨Rockbiter, Flametongue, Frostbrand, Windfury Weapon⟩ | igual | igual |
| G | Lesser Healing Wave ⟨Healing Wave⟩ | igual | Nature's Swiftness |
| H | Elemental Mastery ⟨racial⟩ | racial | Mana Tide Totem ⟨racial⟩ |

- **Tótems (barra de clase):**
  - Tierra: Stoneskin, Earthbind, Stoneclaw, Strength of Earth, Tremor.
  - Fuego: Searing, Fire Nova, Magma, Frost Resistance, Flametongue.
  - Agua: Healing Stream, Mana Spring, Poison Cleansing, Disease Cleansing, Fire Resistance.
  - Aire: Grounding, Nature Resistance, Windfury, Sentry, Windwall, Grace of Air, Tranquil Air.
  - Cada botón muestra el tótem **plantado** de ese elemento con su tiempo, y el que pondría si no hay ninguno.
  - Se necesita el **tótem del elemento en la bolsa** (el `Totem[2]` del `Spell.dbc`): gris si no está, que hoy no
    se comprueba.
- **"Plantar los cuatro de una vez": no.** Son cuatro GCD seguidos, más de lo que hace un macro del 1.12 (P3).
- Reincarnation: al morir, en el contexto 3. Ancestral Spirit en **interacción**.

### 7.11 Razas

| Raza | Activa | Dónde |
|---|---|---|
| Humano | Perception | H (racial) |
| Enano | Stoneform · Find Treasure | G (defensivo: quita venenos, enfermedades y sangrado) · rastreos del minimapa |
| Elfo de la noche | Shadowmeld | H; **no** cambia de capa (no es una forma) |
| Gnomo | Escape Artist | G (rompe raíces / frenazos) |
| Orco | Blood Fury | H |
| No-muerto | Will of the Forsaken · Cannibalize | G (rompe miedo / encantamiento / sueño: vive aunque estés sin control, contexto 2) · cajón |
| Tauren | War Stomp | E del grupo de control o H |
| Trol | Berserking | H |

Además, los **hechizos de sacerdote por raza** (§7.8), y el idioma de la otra facción, que ya está (B4).

### 7.12 Oficios y utilidades

- **Recoger** (hierbas, minerales, despellejar, el corcho de pesca): botón de **interacción**. Es lo que más falla
  tocando objetos pequeños del mundo.
- **Vendas:** consumible Venda.
- **Oficios** (ventanas, Cooking Fire, Fishing, Smelting...): cajón.
- **Rastreos:** botón del minimapa con abanico.
- **Piedra de hogar:** cajón; más adelante, también en el menú.

---

## 8. Asistencias (opcionales, en Opciones → Controles)

Ayudas para el móvil que **no cambian las reglas** y que hacen como mucho lo que un macro del 1.12:

| Asistencia | Qué hace | Por defecto |
|---|---|---|
| **Objetivo automático** | Si pulsas un hechizo hostil sin objetivo válido, se elige el que daría el botón Objetivo y se lanza (el 1.12 deja el cursor esperando un clic, que en el móvil no existe) | Sí |
| **Encarar al lanzar** | Si el objetivo está detrás, te gira hacia él antes de lanzar (`MSG_MOVE_SET_FACING`), si el combate no lo hace ya (ver la paridad de orientación) | Sí |
| **Iniciar el ataque** | Heroic Strike, Cleave, Raptor Strike, sellos y Judgement empiezan el ataque automático si no estabas atacando (el "macro de atacar" del 1.12) | Sí |
| **Repetir manteniendo** | Mantener un botón lo relanza en cuanto se puede (máx. 1 petición cada 250 ms, solo cuando la GCD o el lanzamiento local han terminado) | Sí |
| **Asistencia de posturas** | Guerrero: una habilidad de otra postura cambia de postura y se lanza al confirmarse la forma | Sí (aviso de rabia) |
| **Asistencia de formas** | Druida: un hechizo de caster en forma animal sale de la forma | No |
| **Arrastrar a un marco** | Lanzar sobre un miembro del grupo o de la banda sin cambiar de objetivo | Sí |
| **Apuntar arrastrando** | Hechizos de suelo como en los MOBA: arrastrar desde el botón | Sí |
| **Cola de hechizos** | Una pulsación en los últimos 200 ms de la GCD o del lanzamiento se envía al terminar | No (a evaluar: en LAN la latencia es baja) |
| **Vibración** | Toque corto al lanzar, doble al aparecer el reactivo, largo al fallar | Sí, ligera |
| **Plantilla por sitio** | Cambia de plantilla al entrar en mazmorra, banda o campo de batalla | No |
| **Marcos de sanador junto al racimo** | La plantilla mueve los marcos de grupo y de banda a la izquierda del racimo | En el preset "Sanar" |

**Lo que no haremos** (rompe P3):
- botón que decida la rotación;
- pociones automáticas al bajar de vida;
- interrumpir solo;
- los cuatro tótems de una pulsación;
- atacar solo al recibir daño.

---

## 9. Modo edición

Se entra con Plantillas → "Editar…". En combate se puede cambiar de plantilla, pero no editar.

- El mundo se oscurece y todos los elementos se ven, también los ocultos (reactivo, interacción) y los huecos
  vacíos, con su contorno.
- **Panel arriba:**
  - plantilla;
  - perfil (cuadrado / ancho, el de la pantalla actual);
  - **capa** (Base / Batalla / Sigilo / ...; los huecos heredados se ven translúcidos con una cadena);
  - modo **Disposición** / **Acciones**;
  - tamaño;
  - "Restablecer", "Guardar", "Cancelar", "Zurdo".
- **Disposición:** arrastrar mueve, con rejilla de 1 mm, sin solaparse y dentro de la zona segura. Pellizcar un
  elemento o usar el deslizador lo cambia de tamaño.
- **Acciones:**
  - tocar un hueco abre el selector: pestañas Hechizos / Objetos / Macros / Mascota / Especiales / Grupo nuevo /
    Vaciar / Heredar;
  - también se puede **arrastrar desde el libro de hechizos**, que se abre a la izquierda: en el perfil cuadrado
    caben libro y racimo a la vez;
  - arrastrar un hueco a otro **intercambia** el contenido;
  - mantener = tooltip, rango fijo, "fuera de combate".
- Todo lo que se coloca aquí pasa a **manual**: los presets no lo vuelven a tocar.
- Guardar es inmediato (al fichero en segundo plano y a la subida al servidor con la espera de 30 s que ya tienen
  los datos de cuenta).

---

## 10. Implementación (cuando se apruebe)

### 10.1 Piezas

| Pieza | Qué | Reutiliza |
|---|---|---|
| `game/TouchLayout.{h,cpp}` | Modelo de datos: plantillas, geometrías, capas, acciones, grupos; texto ↔ estructura. Arrays fijos (6 plantillas × 12 capas × 12 huecos) | — |
| `game/ActionContext.{h,cpp}` | Función pura: estado → contexto (prioridad de §5.2) y las 12 acciones resueltas, **solo cuando cambia algo** (forma, auras, mover, mascota, hechizos, inventario) | `ActionBar::bonusBarOffset`, `MoverControl`, `Abilities::pet` |
| `game/SpellLines` | Líneas de rangos (`SkillLineAbility.dbc`) | `GameData` |
| `game/Consumables.{h,cpp}` | Clasificar un objeto por su hechizo al usar y elegir el mejor | `Inventory`, `SpellBook::cooldownOf` |
| `game/ClassPresets.{h,cpp}` | Tablas por clase (ids de rango 1, constantes en C++), candidatos, grupos automáticos, rama de talentos | `AbilityData` (talentos) |
| `app/ActionRunner` | **Una sola** ruta "ejecutar acción" (hechizo / objeto / macro / atacar / repetición / mascota) para el racimo, el cajón y el modo clásico: se saca de `CombatScreens::useSlot` | `CombatScreens` |
| `app/TouchBar` (vista) + `app/TouchBarScreens` (pegamento) | Dibujo del racimo, abanicos, edición; view-model en `HudFrame` como hoy | `Hud`, `HudActionSlot` |
| `ui/` | **Hit test circular** + "el más cercano"; **icono en círculo** (un modo del shader por vértice: la UV del icono ya va de 0 a 1, así que sigue siendo **un pipeline y un draw**); **barrido radial** exportado; aro procedural (con antialias, color por estado) | `ui.frag`, `Widgets::cooldownSweep` |
| `app/TouchLayoutFiles` | Fichero por personaje + tipo 6 en el servidor | `AccountDataFiles`, el escritor en segundo plano |
| Textos | `locale/enUS/touch.lang` + `locale/esES/touch.lang`; del cliente donde existen (`PET_ACTION_*`, `ATTACK`...) | ADR-0003 |

Nada de esto toca Vulkan fuera de `render/` ni crea pipelines. El icono redondo es un modo más del pipeline de la UI.

### 10.2 Datos

Todo sale de lo que ya está en `db.vdpk`:
- `Spell.dbc`: efectos, auras, estados de aura, reactivos, tótems, `Stances`, atributos.
- `SkillLineAbility.dbc`.
- `SpellShapeshiftForm.dbc`.
- Talentos.
- La respuesta de consulta de objetos (hechizos al usar, nivel, banderas de conjurado).

Si el índice de líneas cuesta más de 1 ms al entrar, se hornea en el baker (regla 7). No hace falta ningún pack
nuevo.

### 10.3 Lo que hay que arreglar en el cliente de paso

Con el racimo no hay tooltips a mano, así que el gris tiene que decir la verdad:
1. `ONLY_STEALTHED` (0x20000): Ambush, Ravage, Pounce.
2. **Reactivos y tótems** del `Spell.dbc`: fragmentos de alma, runas, el tótem del elemento.
3. **Objeto equipado:** `EquippedItemClass` / máscaras (escudo, daga, arma a distancia, varita).
4. **Fuera de combate** (Charge, Stealth, comida) y **solo en combate**.
5. **Tipo de criatura** del objetivo (Exorcism, Banish, Hibernate, Shackle Undead).
6. La barra de mascota: alcance y `SMSG_SPELL_COOLDOWN` de la mascota (hoy se ignora).

### 10.4 Pruebas y medida

- **Tests (doctest):**
  - contexto por clase con estados sintéticos (guerrero en cada postura, pícaro en sigilo, druida en
    felino + acecho, sacerdote con MC, cazador a 4 / 6 / 9 m);
  - resolución de presets por nivel y talentos (A = Mortal Strike / Bloodthirst / Rend);
  - elección de consumibles;
  - ida y vuelta del formato;
  - hit test circular y "el más cercano".
- **Galería:** `VD_UI_GALLERY=touch, touchwide, touchedit, touchfan, touchpet, touchtemplates`.
- **`VD_TOUCH_DEMO=<clase>:<nivel>[:<contexto>]`:** el racimo con un libro de hechizos de ejemplo, sin servidor.
  Para revisar las 9 clases con capturas sin crear personajes.
- **En línea:**
  - un personaje desechable por clase, en niveles 10, 30 y 60 (`.levelup`, `.learn all_myclass`,
    `.learn all_mytalents`);
  - pelear con el racimo solo;
  - sigilo, posturas, formas y acecho, MC / Eyes of the Beast, mascotas, montura, consumibles;
  - plegar / desplegar en el Fold5.
- **Rendimiento (Fold5, release):** `fight` y `crowd` con el racimo. Zonas Tracy `TouchBar::resolve` y
  `TouchBar::draw`. Objetivo: ≤ 0,15 ms de CPU, sin asignaciones, 60 fps como hoy.

### 10.5 Criterios de aceptación

- Las 9 clases, en niveles 10 / 30 / 60, pelean contra un grupo de PNJ **sin abrir el cajón ni el libro**.
- Pícaro, guerrero y druida cambian de capa al instante y con P2.
- MC y Eyes of the Beast funcionan en línea.
- Fold5: 60 fps en `fight` con el racimo, en las dos pantallas.
- Las plantillas sobreviven a reinstalar (servidor) y a plegar.

---

## 11. Decisiones

| # | Pregunta | Decisión | Alternativa descartada |
|---|---|---|---|
| 1 | **¿Dónde vive el contenido del racimo?** | **Fichero por personaje + copia en el servidor (tipo 6)**; las 120 casillas del servidor siguen siendo las barras del 1.12 (cajón y modo clásico) | Usar las 120 casillas: herencia de capas, grupos y consumibles automáticos no caben, y las plantillas se comerían las páginas |
| 2 | **¿Qué es el botón grande?** | **La habilidad principal** de la clase; atacar / hablar / despojar / usar pasa al botón de interacción | Dejarlo como hoy (botón de contexto): para un caster sería un botón grande casi inútil en combate |
| 3 | **¿Cuándo se lanza?** | Ofensivas **al bajar el dedo**, amistosas **al soltar** (para arrastrar a marcos), grupos al soltar | Todo al soltar (más uniforme, ~100 ms más lento) |
| 4 | **Herencia de capas** | **Sí** (una capa guarda solo lo que cambia) | Capas completas de 12, como las barras de bonificación del 1.12 |
| 5 | **Colocar solo lo que aprendes** | **Sí, solo en huecos "auto" vacíos del racimo**, con "Nuevo". Las barras del 1.12 no se tocan (F13) | Nunca: el jugador lo coloca todo |
| 6 | **Asistencias por defecto** | Las de la tabla de §8 | Todas apagadas (1.12 puro) |
| 7 | **Aspecto de los botones** | **Icono redondo + aro procedural dorado** (el oro del 1.12) y el brillo `IconBorder-GlowRing` del cliente | El aro de `MiniMap-TrackingBorder` del 1.12 (más fiel; no escala bien a 16 mm y tiene sombra descentrada) |
| 8 | **Modo clásico** | **Se queda como opción** (útil con teclado y mando, 19.6) | Quitarlo |
| 9 | **Cajón** | **Las 6 barras del 1.12** | Abrir el libro de hechizos |
| 10 | **Presets por rol** | 1–3 por clase como en §7 (Sanar / Daño / Tanque) | Uno por clase |

---

## 12. Implementación (fase 20): lo que cambió respecto a la propuesta

### Proporciones (tras probarlo en el Mi 11X)
- Con milímetros fijos el racimo ocupaba en un 20:9 el 62 % del alto y el 40 % del ancho (el principal, el 19 % del
  alto). Se tomaron como referencia **las proporciones** de la captura de un juego de móvil comercial (16:9): el
  principal es el 12,4 % del alto, el anillo el 9,5 %, la columna y los consumibles el 8 %, y el racimo entero cerca
  del 53 % del alto y un tercio del ancho.
- Las posiciones y tamaños van ahora en **centésimas del alto de la zona segura** (o del alto que tendría una pantalla
  16:9 de su ancho, si es más cuadrada: la interior del Fold5), medidos sobre esa referencia (`defaultTouchGeometry`): el
  principal a 24 del borde derecho y 23 del inferior; la montura a la derecha del principal y el salto debajo, a la
  izquierda, como en la referencia; el reactivo entre el principal y A–B; la interacción abajo a la izquierda. Los dos
  perfiles usan la misma disposición; solo cambia la barra de clase (fila en la cuadrada, un botón con abanico en la
  ancha, donde una fila tocaría el minimapa).
- El fichero pasa a `VDTOUCH 2`; uno de la versión 1 (milímetros) conserva sus acciones y vuelve a las posiciones por
  defecto. Los márgenes del dedo (2 mm entre botones) y la almohadilla de apuntar (22 mm) siguen siendo físicos.
- El botón Objetivo mide como Saltar (7, antes 6). El fichero pasa a `VDTOUCH 3`;
  en uno de la versión 2 un Objetivo del tamaño viejo (6) crece al nuevo y uno que el jugador cambió se queda.

### Base
- Dos geometrías por defecto (`defaultTouchGeometry`, ahora proporcionales: ver «Proporciones»); el racimo entero se
  encoge si no cabe (≤ 96 % del alto y 62 % del ancho de la zona segura).
- El cajón (§6.9) sigue en su página 1 la barra de la postura o forma, como la barra principal del 1.12.
- **Iconos propios** para saltar, montura y objetivo (`art/touch/*.svg`, ver `art/README.md`): el baker los
  rasteriza en el atlas de `ui.vdpk`. El botón de montura muestra siempre la silla, se use un caballo o una forma de
  viaje (§6.2).

### Huecos inteligentes (§6.1-6.5)
- Consumibles: piedra de salud y gemas de maná (categoría 30) antes que pociones en combate; comida = aura 84 con
  «no en combate» (20 si es por porcentaje), bebida = aura 85 de maná (21); vendas = canalización con aura 8, en gris
  con el tiempo de Recently Bandaged (inmunidad a vendas). La comida con más efectos («Well Fed») no se elige sola.
  **Rabia y energía** usan su poción también fuera de combate (la propuesta decía «pasa a Venda»: era una segunda
  venda junto a la tercera).
- Mantener un consumible o la montura abre una lista para fijar uno («Automático» lo devuelve al botón).
- Montura: dónde se puede usar cada opción sale de sus atributos (solo exteriores, no en combate, no transformado);
  Ghost Wolf es «solo exteriores» por sus datos.
- Reactivo: Judgement queda fuera (su estado de aura es el sello del paladín); Overpower entra por «gasta rabia y
  pide puntos de combo» (no tiene estado de aura en los datos). Con varios a la vez, el botón es un grupo con abanico.
- Objetivo: primero los que están en pantalla y atacan al jugador o a su grupo, luego los demás en pantalla, luego los
  que atacan fuera de ella; se salta lo que tiene un control que el daño rompe (también el botón de las barras
  clásicas). Deslizar arriba / abajo; mantener = abanico (más cercano, grupo, asistir, mascota, tú).
- Interacción: resucitar a un jugador amigo muerto (efectos 18 / 113; en combate solo Rebirth), despellejar un
  cadáver despojado, recoger hierbas y menas por su tipo de cerradura.

### Plantillas y edición (§5.3, §5.5, §9)
- El panel del modo edición va **a un lado** (el contrario al racimo), no arriba: en la pantalla ancha el racimo ocupa
  casi todo el alto.
- **Main es el ancla**: moverlo mueve todo el racimo.
- Pellizcar cambia el tamaño del botón elegido (o la escala del racimo si no hay ninguno). Atrás / Escape = Hecho.
- El selector tiene pestañas Hechizos (páginas del libro, rango más alto) / Objetos / Macros / Mascota / Otros (Atacar,
  Montura, Cajón y las categorías de consumible), Vacío (base) o «Igual que la base» / «Nada» (capas), Grupo nuevo y
  Rango fijo al mantener. Sin arrastrar desde el libro: «Place on bar» y tocar el hueco.
- Pendiente de §9: el modificador «fuera de combate» por hueco (el modelo no lo tiene) y «Zurdo» en el panel (está en
  las opciones de interfaz).
- Copia en el servidor: tipo 6 por personaje; un tipo 6 ajeno (el `layout-cache.txt` del cliente de PC) no se escribe
  y la copia local se vuelve a subir a los 30 s.
- Densidad real: `getRealMetrics` xdpi / ydpi, descartada si no es sana (xdpi e ydpi difieren > 15 %, fuera de
  100-1000 dpi o de ½-2× el cubo).

### Clases (§7)
- Cambios a las tablas: sacerdote «Daño» con Mind Flay › Smite en el principal (al revés nunca elegía Mind Flay);
  paladín con Divine Shield › Divine Protection como un solo miembro y «Sanar» con Flash of Light › Holy Light;
  druida: Acecho copia la capa felina (no la base), los huecos que la forma no tiene se ocultan, Lechúcico sin capa, el
  consumible 2 es energía en Felino y rabia en Oso; cazador: Auto Shot › Raptor Strike (sin arma a distancia) e
  Intimidation en H; guerrero «Tanque» con la Defensiva de base; racial ofensiva en H y defensiva en G (War Stomp en H);
  Ice Armor › Frost Armor, Demon Armor › Demon Skin como un miembro de grupo.
- `VD_TOUCH_DEMO=<clase>:<nivel>[:<capa>][:<rama>|none][:<raza>][:t<N>]` monta el racimo de una clase sin servidor.
- §10.3: no hay regla «solo en combate» (el 1.12 no tiene ese atributo); las cuentas de reactivos y munición salen solo
  en el racimo.
### Mascota, control y muerte (§5.2 filas 1-3, §6.8)
- Botón de mascota: sin mascota, Llamar (o Revivir si se sabe muerta: la ve tendida, la vio morir o el servidor
  contestó PETTAME_DEAD); con ella, tocar = atacar a mi objetivo o volver si ya lo ataca o no hay objetivo. Muestra el
  retrato, la cara de felicidad y se ilumina mientras pelea. El brujo sin demonio invoca el último (`demon` en el
  fichero) y mantener abre el abanico de sus demonios.
- Panel (mantener con la mascota fuera, modal): órdenes y reacciones, sus hechizos (mantener uno = autolanzamiento),
  los del jugador sobre la mascota (Mend Pet, Health Funnel, Eyes of the Beast...), Feed Pet y Dismiss / Abandon.
- Feed Pet: lo aceptado antes primero, luego las comidas por su hechizo y después los bienes de comercio (la carne
  cruda no tiene hechizo de uso); un rechazo (WRONG_PET_FOOD o FOOD_LOWLEVEL) lo quita para esa familia (`petfood`).
- Acciones de mascota en un hueco: por hechizo (Spell Lock sigue al manáfago; gris con otro demonio), con sus segundos
  de enfriamiento y el alcance medido desde la mascota. Opción «Mostrar la barra de mascota» (apagada).
- Controlar otra unidad: la barra de esa unidad en los 9 huecos (su Atacar en el principal) y **Salir** en el sitio del
  reactivo, que envía `CMSG_CANCEL_AURA` (vMaNGOS ignora CANCEL_CHANNELLING si lo controlado es un jugador).
- Sin control: el racimo atenuado; solo sigue vivo lo que rompe todos los controles que lleva el jugador (la regla de
  `Spell::CheckCasterAuras` y la inmunidad por mecánica, escuela o disipación: Will of the Forsaken contra miedo y
  sueño, no contra aturdir; Blink contra aturdir; Divine Shield, Ice Block). Las órdenes a la mascota siguen vivas.
- Muerte: Liberar espíritu en el principal (1,5 s sin responder, para que un toque al morir no libere) o Resucitar de
  fantasma junto al cuerpo; Soulstone / Reincarnation en el reactivo; Aceptar una resurrección en A.

### Asistencias (§4, §8)
- Arrastrar a un marco: el del jugador, el objetivo, el objetivo del objetivo, el grupo y sus mascotas, los marcos
  sueltos de banda y la mascota; el marco bajo el dedo se ilumina; un hechizo amistoso no cae en un hostil.
- Apuntar en el suelo: una almohadilla de 22 mm alrededor del botón (0 en su borde, 1 en el de la almohadilla) lleva el
  círculo de F13 dentro del alcance, en el marco de la cámara; soltar lanza, volver al botón cancela (aro rojo).
- Objetivo automático: solo al pulsar (mantener nunca salta de enemigo en enemigo).
- **Iniciar el ataque**: el 1.12 ya lo hace con `INITIATES_COMBAT` (0x200: Sinister Strike, Mortal Strike, Cleave,
  Claw, Maul...) al lanzar, dentro del alcance cuerpo a cuerpo, y con `EX2 0x100000` tras el SPELL_GO (Judgement, las
  aperturas del sigilo): el racimo lo sigue aunque la asistencia esté apagada; la asistencia añade Heroic Strike,
  Raptor Strike y los sellos. **Pendiente**: las barras clásicas aún no siguen esa regla del 1.12.
- Posturas: Batalla, Defensiva, Rabiosa en ese orden; espera la forma 1,5 s; respeta el enfriamiento compartido de las
  posturas (1 s); el aviso de la rabia una vez por sesión. El reactivo cuenta Overpower como usable en Rabiosa.
- Repetir: una petición en camino cada vez (hasta SPELL_START / SPELL_GO / CAST_RESULT o 600 ms), una cada 250 ms y
  1 s de espera tras un rechazo (250 ms si era «aún no»). Apagado, mantener muestra el tooltip.
- Vibración en su propio hilo (`vd-haptics`): tic al lanzar, doble al aparecer un reactivo, larga al rechazarse.
- Marcos de sanador: `healerframes` por plantilla, encendido en las cuatro plantillas «Sanar»; mueve los marcos de
  grupo junto al racimo (no los sueltos de banda).
- «Encarar al lanzar» no se hizo: el juego ya se gira tras el lanzamiento (girar antes pediría `MSG_MOVE_SET_FACING`
  antes de `CMSG_CAST_SPELL`).

### Retoques: sin aros, con los recursos del 1.12

El icono redondo ocupa todo el botón, sin aro ni zócalo (`app/TouchRoundIcon`; los SVG propios se quedan). Tras
probarlas, se prefirió la respuesta de antes a las texturas de ActionButton del 1.12 (pulsado, activo, ataque automático) y la recarga nuestra al
modelo del 1.12, así que esas vuelven a ser como eran:

| Estado | Antes | Ahora |
|---|---|---|
| Enfriamiento | barrido radial + segundos | igual: se prefirió al modelo `UI-Cooldown-Indicator.m2` del 1.12 (que sí usan las barras clásicas, las bolsas, el libro y la barra de la mascota) |
| GCD | aro fino | igual |
| Pulsado | encoger al 92 % | igual |
| Activo | aro dorado que late | igual (solo mientras está activa) |
| Plaza elegida en edición | contorno | `CheckButtonHilight` |
| Ataque automático | parpadeo rojo sobre el icono | igual |
| Cancelar al arrastrar | aro rojo | `UI-QuickslotRed` sobre el icono |
| Equipado | — | `UI-ActionButton-Border` en verde |
| Fuera de alcance | rojo | rojo sobre el icono (adaptación: el 1.12 pinta la tecla, que aquí no hay) |

Siguen: el brillo `IconBorder-GlowRing` del reactivo (arte del cliente), el disco oscuro de saltar, montura y objetivo
con sus SVG propios, el eslabón y el aspa del modo edición. Los puntos de combo son el arte de ComboFrame (hueco,
gema y brillo) con sus fundidos (0,4 / 0,3 / 0,4 s). El autolanzamiento de la mascota es `UI-AutoCastButton.m2`.
