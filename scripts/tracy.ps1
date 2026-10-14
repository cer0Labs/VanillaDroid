<#
.SYNOPSIS
    Abre el Tracy Profiler conectado a VanillaDroid en el movil.

.DESCRIPTION
    El cliente de Tracy solo se compila en la variante Android 'profile'. Escucha en el puerto 8086 del movil
    y no graba nada hasta que el perfilador se conecta. Este script hace 'adb forward' y abre la GUI.
    La GUI (v0.14.1, misma version que el cliente) vive en .tools\tracy-0.14.1 (no se commitea).

.EXAMPLE
    .\scripts\android.ps1 -Variant profile -Install -Run
    .\scripts\tracy.ps1
#>

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$profiler = Join-Path $root '.tools\tracy-0.14.1\tracy-profiler.exe'
if (-not (Test-Path $profiler)) {
    throw "No esta Tracy en $profiler. Descargar windows-0.14.1.zip de https://github.com/wolfpld/tracy/releases/tag/v0.14.1 y descomprimir ahi."
}

$adb = Join-Path $env:LOCALAPPDATA 'Android\Sdk\platform-tools\adb.exe'
& $adb forward tcp:8086 tcp:8086
if ($LASTEXITCODE -ne 0) { throw "adb forward failed ($LASTEXITCODE)" }

Start-Process -FilePath $profiler -ArgumentList '-a', '127.0.0.1'
