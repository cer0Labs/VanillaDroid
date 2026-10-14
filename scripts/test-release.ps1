<#
.SYNOPSIS
    Prepara una release de PRUEBA para servirla en la red local (fase 22, ADR-0005 secciones 6-7): el APK y el zip del
    Setup de ahora, su manifest.json firmado con la clave de prueba y las ordenes para servirla y probarla.

.DESCRIPTION
    Copia el APK (por defecto el release de apps\android, si no el dist) como VanillaDroid-X.Y.Z.apk y, si lo hay, el
    zip de scripts\package-setup.ps1 como VanillaDroidSetup-X.Y.Z-windows.zip en -Out (build\test-release\vX.Y.Z), y
    escribe manifest.json y manifest.json.sig con scripts\release-manifest.ps1 -BaseUrl http://<PC>:<puerto>: las URL
    apuntan a esta carpeta, servida con python -m http.server. La version sale del APK (aapt2 del SDK) si no se da.

    Se niega a seguir si la clave que llevan los programas (release\release-key.pub.hex) no es la de prueba
    (tests\data\release-test-key.pub.hex): una release firmada con la clave de prueba solo la aceptan los programas
    compilados con ella, y la de verdad nunca se usa para probar.

    -ChangePack cambia la receta de esos packs en el manifiesto (una release que cambia datos: el Setup dice que hay que
    volver a hornearlos y la app avisa); sin el, las recetas son las de release\packs.json (una release solo de codigo).

    Para probar una actualizacion, lo instalado tiene que ser de una version menor: compilar el APK y el Setup de la
    release de prueba con -PvdVersion=X.Y.Z / -DVD_VERSION=X.Y.Z.

.EXAMPLE
    .\scripts\test-release.ps1 -Version 0.2.0
    .\scripts\test-release.ps1 -Apk apps\android\app\build\outputs\apk\release\app-release.apk -ChangePack models,ui
    .\scripts\test-release.ps1 -HostAddress 127.0.0.1 -Port 8766   # solo este PC (o el movil con adb reverse)
#>
param(
    # X.Y.Z; empty: the APK's versionName (it must be the APK's: the app checks the version it installs).
    [string] $Version,
    # The APK of the test release; empty: the release build's (a development app reads VD_UPDATE_URL), else dist's.
    [string] $Apk,
    # VanillaDroid Setup for Windows (scripts\package-setup.ps1's zip); empty: build\dist\VanillaDroidSetup.zip if it
    # exists. 'none': the release has no Setup.
    [string] $SetupZip,
    [string] $Out,
    # The PC's address on the phone's network; empty: the first private IPv4 address of this PC.
    [string] $HostAddress,
    [int] $Port = 8000,
    # Packs whose recipe the test release changes (models, world_azeroth, ui...).
    [string[]] $ChangePack = @()
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'release-common.ps1')

$root = Get-RepoRoot
$testKey = Join-Path $root 'tests/data/release-test-key.pem'
$builtIn = Read-PublicKeyHex (Get-ReleaseKeyPath)
if ($builtIn -ne (Get-TestKeyHex)) {
    throw 'release\release-key.pub.hex ya no es la clave de PRUEBA: los programas no aceptarian una release firmada con ella (y la de verdad no se usa para probar)'
}

# The APK.
if (-not $Apk) {
    foreach ($variant in @('release', 'dist')) {
        $candidate = Join-Path $root "apps\android\app\build\outputs\apk\$variant\app-$variant.apk"
        if (Test-Path -LiteralPath $candidate) { $Apk = $candidate; break }
    }
    if (-not $Apk) { throw 'No hay APK compilado (apps\android\app\build\outputs\apk\release|dist): compila uno o da -Apk' }
}
$Apk = Resolve-FullPath $Apk
if (-not (Test-Path -LiteralPath $Apk)) { throw "No existe $Apk" }

# Its version, from the SDK's aapt2 (the newest build-tools).
$apkVersion = $null
$apkPackage = $null
$sdk = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { Join-Path $env:LOCALAPPDATA 'Android\Sdk' }
$aapt2 = Get-ChildItem (Join-Path $sdk 'build-tools') -Directory -ErrorAction SilentlyContinue |
    Sort-Object { try { [version]$_.Name } catch { [version]'0.0' } } -Descending |
    ForEach-Object { Join-Path $_.FullName 'aapt2.exe' } | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if ($aapt2) {
    $ErrorActionPreference = 'Continue'
    $badging = (& $aapt2 dump badging $Apk 2>$null) | Select-Object -First 1
    $ErrorActionPreference = 'Stop'
    if ($badging -match "package: name='([^']+)' versionCode='(\d+)' versionName='([^']+)'") {
        $apkPackage = $Matches[1]
        $apkVersion = $Matches[3]
    }
}
if (-not $Version) {
    if (-not $apkVersion) { throw "No se pudo leer la version de $Apk (aapt2 del SDK): da -Version" }
    $Version = $apkVersion
}
$versionCode = Get-VersionCode $Version
if ($apkVersion -and $apkVersion -ne $Version) {
    throw "El APK es la version $apkVersion, no ${Version}: la app no instalaria un APK de otra version que la del manifiesto"
}

# The Setup zip.
if (-not $SetupZip) {
    $candidate = Join-Path $root 'build\dist\VanillaDroidSetup.zip'
    if (Test-Path -LiteralPath $candidate) { $SetupZip = $candidate }
}
if ($SetupZip -eq 'none') { $SetupZip = '' }
if ($SetupZip) {
    $SetupZip = Resolve-FullPath $SetupZip
    if (-not (Test-Path -LiteralPath $SetupZip)) { throw "No existe $SetupZip" }
}

# The PC's address on the local network.
if (-not $HostAddress) {
    $HostAddress = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Where-Object { $_.IPAddress -match '^(10\.|192\.168\.|172\.(1[6-9]|2\d|3[01])\.)' -and $_.AddressState -eq 'Preferred' } |
        Sort-Object InterfaceMetric | Select-Object -ExpandProperty IPAddress -First 1
    if (-not $HostAddress) { $HostAddress = '127.0.0.1' }
}
$base = "http://${HostAddress}:$Port"

# The folder.
if (-not $Out) { $Out = Join-Path $root "build\test-release\v$Version" }
$Out = Resolve-FullPath $Out
if (Test-Path -LiteralPath $Out) { Remove-Item -LiteralPath $Out -Recurse -Force }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$apkName = "VanillaDroid-$Version.apk"
Copy-Item -LiteralPath $Apk -Destination (Join-Path $Out $apkName)
$arguments = @{
    Version      = $Version
    Apk          = (Join-Path $Out $apkName)
    Out          = (Join-Path $Out 'manifest.json')
    BaseUrl      = $base
    SigningKey   = $testKey
    PublicKey    = (Get-ReleaseKeyPath)
    AllowTestKey = $true
}
if ($SetupZip) {
    $zipName = "VanillaDroidSetup-$Version-windows.zip"
    Copy-Item -LiteralPath $SetupZip -Destination (Join-Path $Out $zipName)
    $arguments.SetupWindows = (Join-Path $Out $zipName)
}
# Recipes: release\packs.json, with the packs of -ChangePack changed.
$packsJson = Join-Path $root 'release/packs.json'
$changes = @($ChangePack | ForEach-Object { $_ -split '[,;\s]+' } | Where-Object { $_ })
if ($changes.Count -gt 0) {
    $recipes = [IO.File]::ReadAllText($packsJson) | ConvertFrom-Json
    foreach ($name in $changes) {
        # A recipe no folder has (also for a pack release\packs.json does not list yet): it has to be baked again.
        $recipes.packs | Add-Member -NotePropertyName $name -NotePropertyValue "test-release-$Version-changed" -Force
    }
    $packsJson = Join-Path $Out 'packs.json'
    Write-Utf8File $packsJson ($recipes | ConvertTo-Json -Depth 4)
}
$arguments.Packs = $packsJson
& (Join-Path $PSScriptRoot 'release-manifest.ps1') @arguments
if ($changes.Count -gt 0) { Remove-Item -LiteralPath $packsJson -Force }

$app = if ($apkPackage) { $apkPackage } else { 'dev.cer.vanilladroid' }
Write-Host ''
Write-Host "Release de prueba $Version ($versionCode) en $Out"
Get-ChildItem -LiteralPath $Out | ForEach-Object { Write-Host ("  {0,-44} {1,12:N0}" -f $_.Name, $_.Length) }
Write-Host ''
Write-Host 'Servirla (dejarla abierta mientras se prueba):'
Write-Host "  python -m http.server $Port --bind $HostAddress --directory `"$Out`""
if ($HostAddress -eq '127.0.0.1') {
    Write-Host 'El movil la ve por ADB:'
    Write-Host "  adb reverse tcp:$Port tcp:$Port"
}
Write-Host 'La app de desarrollo (dist solo lee HTTPS y la release de GitHub):'
Write-Host "  adb shell am start -n $app/dev.cer.vanilladroid.MainActivity --es env VD_UPDATE_URL=$base/manifest.json"
Write-Host 'VanillaDroid Setup de desarrollo (por consola, o la ventana con VD_UPDATE_URL):'
Write-Host "  vdsetup update --check --manifest-url $base/manifest.json"
Write-Host "  `$env:VD_UPDATE_URL = '$base/manifest.json'; VanillaDroidSetup.exe"
