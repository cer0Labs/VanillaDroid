<#
.SYNOPSIS
    Compila, instala, lanza y muestra el log de VanillaDroid en el dispositivo conectado por ADB.

.DESCRIPTION
    Antes de Gradle hornea app.vdpk (textos, arte y fuente propios, fase 22) en los assets del APK con el baker de
    escritorio (build\windows\tools\baker\RelWithDebInfo\vd_baker.exe bake --only app): no necesita el WoW.

    Variantes:
      release  - la que se usa para jugar y para las cifras finales de rendimiento
      profile  - release + cliente de Tracy (ver scripts\tracy.ps1)
      debug    - solo para depurar crashes; nunca para medir
      dist     - la que se publica (fase 22, ADR-0005): com.cer0labs.vanilladroid, sin paneles de depuracion ni
                 interruptores VD_*, firmada con la clave de release si VD_KEYSTORE* estan definidas

.EXAMPLE
    .\scripts\android.ps1 -Variant release -Install -Run -Logcat
    .\scripts\android.ps1 -Variant profile -Install -Run   # y luego .\scripts\tracy.ps1
    .\scripts\android.ps1 -Reverse     # tunel al vMaNGOS del PC (3724 realmd, 8085 world)
    .\scripts\android.ps1 -Logcat      # solo log
    # Entrar en el servidor: por Wi-Fi a la IP del PC, o con -Reverse y -Realmd 127.0.0.1:3724
    .\scripts\android.ps1 -Run -Account <cuenta> -Password ... -Character <nombre> -Realmd <host>:3724
    # La contrasena solo viaja en el intent (nunca se guarda); sin -Realmd se usa client.cfg del movil.
    # Pruebas en linea (GM): -Commands ".tele goldshire;.modify speed 2" -WalkRoute walk_goldshire
#>
param(
    [ValidateSet('debug', 'release', 'profile', 'dist')] [string] $Variant = 'release',
    [switch] $Build,
    [switch] $Install,
    [switch] $Run,
    [switch] $Logcat,
    [switch] $Reverse,
    [string] $Account,
    [string] $Password,
    [string] $Character,
    [string] $Realmd,
    [string] $Commands,
    [string] $WalkRoute
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$gradleProject = Join-Path $root 'apps\android'
# Phase 22: the published build has its own applicationId (both can be installed side by side).
$package = if ($Variant -eq 'dist') { 'com.cer0labs.vanilladroid' } else { 'dev.cer.vanilladroid' }
# Always the SDK adb: a different adb version on PATH (e.g. scrcpy's) keeps killing the ADB server.
$adb = Join-Path $env:LOCALAPPDATA 'Android\Sdk\platform-tools\adb.exe'

if (-not $env:JAVA_HOME) {
    $env:JAVA_HOME = 'C:\Program Files\Android\Android Studio\jbr'
}

function Invoke-Adb {
    & $adb @args
    if ($LASTEXITCODE -ne 0) { throw "adb $args failed ($LASTEXITCODE)" }
}

# -Install always rebuilds first so the device never runs a stale APK.
if ($Build -or $Install) {
    # Phase 22 (apppack): app.vdpk (our texts, art and font) into the APK's assets, from the repository alone.
    $baker = Join-Path $root 'build\windows\tools\baker\RelWithDebInfo\vd_baker.exe'
    $assets = Join-Path $gradleProject 'app\build\generated\vdassets'
    if (-not (Test-Path $baker)) {
        throw "Falta $baker (compila el baker: cmake --build --preset windows-release --target vd_baker)"
    }
    New-Item -ItemType Directory -Force -Path $assets | Out-Null
    & $baker bake --only app --out $assets
    if ($LASTEXITCODE -ne 0) { throw "vd_baker bake --only app fallo ($LASTEXITCODE)" }

    $task = 'assemble' + $Variant.Substring(0, 1).ToUpper() + $Variant.Substring(1)
    Push-Location $gradleProject
    try {
        & .\gradlew.bat $task
        if ($LASTEXITCODE -ne 0) { throw "Gradle $task failed ($LASTEXITCODE)" }
    } finally {
        Pop-Location
    }
}

if ($Reverse) {
    Invoke-Adb reverse tcp:3724 tcp:3724
    Invoke-Adb reverse tcp:8085 tcp:8085
    Write-Host 'adb reverse activo: 127.0.0.1:3724/8085 en el movil -> PC'
}

if ($Install) {
    $apk = Join-Path $gradleProject "app\build\outputs\apk\$Variant\app-$Variant.apk"
    Invoke-Adb install -r $apk
}

if ($Run) {
    # One command line for the device shell, each value single-quoted (they may hold spaces and ';'). Built here as a
    # single argument: Windows PowerShell mangles quotes inside arguments it forwards to native programs.
    $command = "am start -n $package/dev.cer.vanilladroid.MainActivity" # the class keeps the code's namespace
    foreach ($pair in @(@('account', $Account), @('password', $Password), @('character', $Character), @('realmd', $Realmd),
                        @('commands', $Commands), @('walkroute', $WalkRoute))) {
        if ($pair[1]) { $command += " --es $($pair[0]) '" + $pair[1].Replace("'", "'\''") + "'" }
    }
    Invoke-Adb shell am force-stop $package
    Invoke-Adb shell $command
}

if ($Logcat) {
    & $adb logcat -v time -s 'VanillaDroid:V' 'GameActivity:V' 'AndroidRuntime:E' 'DEBUG:V' 'libc:E'
}
