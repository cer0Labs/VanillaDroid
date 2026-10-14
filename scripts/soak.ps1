<#
.SYNOPSIS
    Sesion larga en el movil (fase 10): la ruta soak (Orgrimmar y el combate de 40 frente al banco, en bucle) durante
    -Minutes, con rendimiento, temperatura, calidad dinamica y bateria minuto a minuto.

.DESCRIPTION
    Para medir la bateria el movil tiene que estar DESENCHUFADO (ADB inalambrico): enchufado, el script se niega salvo
    con -AllowCharging (entonces solo vale para la temperatura y el rendimiento).
    Durante la sesion el script no habla con el movil (el trafico de adb tambien gasta bateria): el juego escribe cada
    minuto sus lineas SOAK en files/packs/bench_soak.txt y el script las recoge al acabar, junto con dumpsys battery
    antes y despues. Resultado: la tabla por minuto y el resumen en pantalla, y un CSV en build\soak\.
    Procedimiento: docs/PERFORMANCE.md (pantalla interior, brillo fijo, sin tocar el movil).

.EXAMPLE
    .\scripts\soak.ps1                      # 30 min, calidad por defecto (Alta) con calidad dinamica
    .\scripts\soak.ps1 -Minutes 5 -AllowCharging
    .\scripts\soak.ps1 -DynamicQuality off  # la misma sesion sin adaptar nada (referencia)
#>
param(
    [double] $Minutes = 30,
    [switch] $AllowCharging,
    [ValidateSet('', 'low', 'medium', 'high', 'ultra')] [string] $Quality = '',
    [ValidateSet('', 'on', 'off')] [string] $DynamicQuality = ''
)

$ErrorActionPreference = 'Stop'
$adb = Join-Path $env:LOCALAPPDATA 'Android\Sdk\platform-tools\adb.exe'
$package = 'dev.cer.vanilladroid'
$remote = '/sdcard/Android/data/dev.cer.vanilladroid/files/packs/bench_soak.txt'
$invariant = [Globalization.CultureInfo]::InvariantCulture

function Get-Battery {
    $text = (& $adb shell dumpsys battery) -join "`n"
    $value = { param($name) if ($text -match "(?m)^\s*$name\s*:\s*(\S+)") { $Matches[1] } else { '' } }
    [pscustomobject]@{
        Time      = Get-Date
        Level     = [int](& $value 'level')
        Plugged   = ((& $value 'AC powered') -eq 'true') -or ((& $value 'USB powered') -eq 'true') -or
                    ((& $value 'Wireless powered') -eq 'true')
        VoltageMv = [int](& $value 'voltage')
        Celsius   = [int](& $value 'temperature') / 10.0
        ChargeUah = [long](& $value 'Charge counter')
    }
}

$model = (& $adb shell getprop ro.product.model).Trim()
$before = Get-Battery
if ($before.Plugged -and -not $AllowCharging) {
    throw "[$model] Esta cargando: desenchufalo para medir la bateria (o usa -AllowCharging)."
}
$brightness = (& $adb shell settings get system screen_brightness).Trim()
& $adb shell rm -f $remote
& $adb shell am force-stop $package
$extras = '--es bench soak --es benchminutes ' + $Minutes.ToString($invariant)
if ($Quality) { $extras += " --es quality $Quality" }
if ($DynamicQuality) { $extras += " --es dynamicquality $DynamicQuality" }
& $adb shell "am start -n $package/.MainActivity $extras" | Out-Null
Write-Host ("[{0}] Soak de {1} min en marcha: bateria {2}% ({3:N1} C), brillo {4}. No toques el movil." -f $model,
    $Minutes, $before.Level, $before.Celsius, $brightness)

# Silence while it runs: the minutes, the warm-up (8 s) and the start-up.
Start-Sleep -Seconds ([int]($Minutes * 60) + 40)
$report = ''
$deadline = (Get-Date).AddMinutes(4)
while ((Get-Date) -lt $deadline) {
    $report = (& $adb shell "cat '$remote' 2>/dev/null") -join "`n"
    if ($report -match 'SOAK done') { break }
    $crash = & $adb logcat -d -s AndroidRuntime:E libc:F DEBUG:F | Select-String 'FATAL|Fatal signal'
    if ($crash) { $crash; throw 'La app se ha cerrado durante la sesion (ver logcat)' }
    Start-Sleep -Seconds 15
}
$after = Get-Battery
& $adb shell am force-stop $package
if ($report -notmatch 'SOAK done') {
    $report
    throw 'Sin resumen SOAK done (mira .\scripts\android.ps1 -Logcat)'
}

# Minute table -> CSV.
$rows = foreach ($line in ($report -split "`n")) {
    if ($line -match '^SOAK minute (\d+): ([\d.]+) fps \| p50 ([\d.]+) p99 ([\d.]+) max ([\d.]+) ms \| >20 ms (\d+), >33 ms (\d+) \| GPU ([\d.]+) CPU ([\d.]+) ms \| scale ([\d.]+) distance ([\d.]+) effects ([\d.]+) T(\d) L(\d) \| headroom ([-\d.na]+) status (-?\d+) \| battery (-?\d+)% ([-+\d.]+) mA ([\d.]+) W ([-\d.na]+) C') {
        [pscustomobject]@{
            minute = [int]$Matches[1]; fps = $Matches[2]; p50_ms = $Matches[3]; p99_ms = $Matches[4]; max_ms = $Matches[5]
            over20 = $Matches[6]; over33 = $Matches[7]; gpu_ms = $Matches[8]; cpu_ms = $Matches[9]; scale = $Matches[10]
            distance = $Matches[11]; effects = $Matches[12]; thermal_level = $Matches[13]; load_level = $Matches[14]
            headroom = $Matches[15]; thermal_status = $Matches[16]; battery_pct = $Matches[17]; current_ma = $Matches[18]
            watts = $Matches[19]; battery_c = $Matches[20]
        }
    }
}
$outDir = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\soak'
New-Item -ItemType Directory -Force $outDir | Out-Null
$csv = Join-Path $outDir ("soak_{0}_{1}.csv" -f ($model -replace '[^\w-]', '_'), (Get-Date -Format 'yyyyMMdd-HHmm'))
$rows | Export-Csv -NoTypeInformation -Path $csv -Encoding UTF8
$rows | Format-Table minute, fps, p99_ms, max_ms, over20, gpu_ms, cpu_ms, scale, thermal_level, headroom, thermal_status,
    battery_pct, current_ma, watts, battery_c -AutoSize | Out-String -Width 220 | Write-Host
($report -split "`n") | Where-Object { $_ -match '^(SOAK done|BENCH)' } | ForEach-Object { Write-Host "[$model] $_" }

# The battery as Android saw it, before and after (the game's own figures are in SOAK done).
$hours = ($after.Time - $before.Time).TotalHours
$usedMah = ($before.ChargeUah - $after.ChargeUah) / 1000.0
if (-not $before.Plugged -and -not $after.Plugged -and $hours -gt 0) {
    Write-Host ("[{0}] dumpsys battery: {1}% -> {2}% en {3:N1} min = {4:N1} %/h | {5:N0} mAh = {6:N0} mAh/h | {7:N1} -> {8:N1} C" -f
        $model, $before.Level, $after.Level, ($hours * 60), (($before.Level - $after.Level) / $hours), $usedMah,
        ($usedMah / $hours), $before.Celsius, $after.Celsius)
} else {
    Write-Host "[$model] Enchufado: la bateria no se ha medido."
}
Write-Host "CSV: $csv"
