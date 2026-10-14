<#
.SYNOPSIS
    Ejecuta un recorrido de benchmark en el movil y muestra el resultado (linea BENCH del log).

.DESCRIPTION
    Lanza la app con el extra de intent "bench": la camara vuela la ruta (app/Benchmark.cpp) tras 8 s de
    calentamiento y al terminar escribe en logcat una linea "BENCH <ruta>: frames, fps, p50/p99/max,
    frames > 20 ms y > 33 ms, GPU, CPU, draws, triangulos".
    Rutas F2: elwynn (Elwynn > Paramos > Bosque del Ocaso), durotar (Durotar > Los Baldios),
    barrens (Los Baldios norte > sur), goldshire (corta, para pruebas rapidas).
    Rutas F3, a pie de calle: stormwind, orgrimmar, ironforge, stranglethorn (Vega de Tuercespina > Bahia del Botin).
    Ruta F5: crowd (40 personajes animados sinteticos frente al banco de Orgrimmar; la camara los rodea) y crowd0
    (la misma vuelta sin personajes, para medir lo que cuestan).
    Rutas F9: fxcrowd (escena cargada de particulas), fight (40 combatientes con hechizos y efectos frente al banco
    de Orgrimmar) y fight20 (20).
    Rutas F6, a pie con el controlador del personaje sobre la colision horneada (app/WalkTest.cpp, patrullas de PNJ
    de vMaNGOS): walk_goldshire (Villadorada > Ventormenta), walk_stormwind, walk_orgrimmar, walk_gorlach; fase 12, en
    instancias: walk_deadmines (Minas de la Muerte) y walk_zulgurub (Zul'Gurub). Su linea
    de resultado ("Walk ...: puntos alcanzados, atascos, caidas...") incluye tambien fps, CPU y el coste del
    controlador.
    Fase 10: -Quality low|medium|high|ultra (preset en vez de los ajustes por defecto), -RenderScale 0.5..1 (resolucion
    del mundo fija, sin calidad dinamica), -DynamicQuality on|off. La ruta soak (Orgrimmar + combate de 40 en bucle)
    dura -Minutes (por defecto 30); para medir bateria y temperatura usar scripts\soak.ps1.
    Requisitos: build release instalada y packs subidos (push-data.ps1): world_*, textures, models y actors (y
    collision_* para las rutas a pie).
    Procedimiento de medida: docs/PERFORMANCE.md (bateria, pantalla interior, calentamiento).

.EXAMPLE
    .\scripts\bench.ps1 -Route fight -RenderScale 0.7
    .\scripts\bench.ps1 -Route soak -Minutes 3
    .\scripts\bench.ps1 -Route elwynn
    .\scripts\bench.ps1 -Route durotar -TimeoutSeconds 240
    .\scripts\bench.ps1 -Route stormwind -TimeoutSeconds 300
#>
param(
    [ValidateSet('elwynn', 'durotar', 'barrens', 'goldshire', 'stormwind', 'orgrimmar', 'ironforge', 'stranglethorn',
                 'crowd', 'crowd0', 'fxcrowd', 'fight', 'fight20', 'soak', 'walk_goldshire', 'walk_stormwind', 'walk_orgrimmar',
                 'walk_gorlach', 'walk_deadmines', 'walk_zulgurub')]
    [string] $Route = 'elwynn',
    [int] $TimeoutSeconds = 240,
    [ValidateSet('', 'low', 'medium', 'high', 'ultra')] [string] $Quality = '',
    [double] $RenderScale = 0,
    [ValidateSet('', 'on', 'off')] [string] $DynamicQuality = '',
    [double] $Minutes = 0,
    [string] $Weather = '',
    [switch] $Ghost,
    [string] $Grass = '',
    [string[]] $EnvSwitches = @() # fase 14: interruptores de desarrollo, p. ej. VD_DOODAD_ANIM=0,VD_DOODAD_FADE=0
)

$adb = Join-Path $env:LOCALAPPDATA 'Android\Sdk\platform-tools\adb.exe'
$package = 'dev.cer.vanilladroid'

& $adb logcat -c
& $adb shell am force-stop $package
$extras = "--es bench $Route"
if ($Quality) { $extras += " --es quality $Quality" }
if ($RenderScale -gt 0) { $extras += " --es renderscale " + $RenderScale.ToString([Globalization.CultureInfo]::InvariantCulture) }
if ($DynamicQuality) { $extras += " --es dynamicquality $DynamicQuality" }
if ($Weather) { $extras += " --es weather $Weather" } # fase 11: rain:1, snow:1, sand:1...
if ($Ghost) { $extras += " --es ghost 1" }            # fase 11: el mundo del fantasma
if ($Grass) { $extras += " --es grass $Grass" }       # fase 14: 0 = sin hierba; un numero = su frillDensity (16)
if ($EnvSwitches) { $extras += " --es env " + ($EnvSwitches -join ',') } # fase 14: como el entorno del escritorio
if ($Minutes -gt 0) {
    $extras += " --es benchminutes " + $Minutes.ToString([Globalization.CultureInfo]::InvariantCulture)
    $TimeoutSeconds = [Math]::Max($TimeoutSeconds, [int]($Minutes * 60) + 180)
} elseif ($Route -eq 'soak') {
    $TimeoutSeconds = [Math]::Max($TimeoutSeconds, 30 * 60 + 180)
}
# One command line for the device shell: PowerShell 5.1 mangles the quotes of arguments it forwards.
& $adb shell "am start -n $package/.MainActivity $extras" | Out-Null
Write-Host "Recorrido '$Route' en marcha (no toques el movil)..."

$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
$result = $null
while (-not $result -and (Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 5
    $result = & $adb logcat -d -s VanillaDroid:V | Select-String 'BENCH |points reached'
    $crash = & $adb logcat -d -s AndroidRuntime:E libc:F DEBUG:F | Select-String 'FATAL|Fatal signal'
    if ($crash) {
        $crash
        throw 'La app se ha cerrado durante el recorrido (ver logcat)'
    }
}
if (-not $result) {
    throw "Sin resultado tras $TimeoutSeconds s (mira .\scripts\android.ps1 -Logcat)"
}
# The app keeps the screen on: leave nothing running (a forgotten lap heats the phone and spoils the next one).
& $adb shell am force-stop $package
$model = (& $adb shell getprop ro.product.model).Trim()
Write-Host "[$model] $($result.Line)"
