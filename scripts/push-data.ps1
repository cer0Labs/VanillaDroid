<#
.SYNOPSIS
    Copia los packs horneados (packs\*.vdpk) al almacenamiento de la app en el movil.

.DESCRIPTION
    Destino: /sdcard/Android/data/dev.cer.vanilladroid/files/packs/ (almacenamiento propio de la app, sin
    permisos). La app debe haberse abierto al menos una vez para que Android cree la carpeta.
    Solo copia los packs cuyo MD5 difiere del que hay en el movil (el tamano no basta: ASTC produce el
    mismo tamano con distinta calidad).
    Ojo: desinstalar la app borra esa carpeta.

.EXAMPLE
    .\scripts\push-data.ps1
    .\scripts\push-data.ps1 -Force      # copia todo sin comparar
#>
param(
    [string] $PacksDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'packs'),
    [switch] $Force
)

$ErrorActionPreference = 'Stop'
$adb = Join-Path $env:LOCALAPPDATA 'Android\Sdk\platform-tools\adb.exe'
$remoteDir = '/sdcard/Android/data/dev.cer.vanilladroid/files/packs'

# app.vdpk (fase 22) va dentro del APK: la app usa su copia interna, no la de packs/.
$packs = Get-ChildItem -Path $PacksDir -Filter '*.vdpk' -File | Where-Object { $_.Name -ne 'app.vdpk' }
if (-not $packs) { throw "No hay packs en $PacksDir (ejecuta primero vd_baker bake --out packs)" }

& $adb shell mkdir -p $remoteDir
if ($LASTEXITCODE -ne 0) { throw "No se pudo crear $remoteDir (abre la app una vez primero)" }

foreach ($pack in $packs) {
    $remote = "$remoteDir/$($pack.Name)"
    if (-not $Force) {
        $localMd5 = (Get-FileHash -Algorithm MD5 -Path $pack.FullName).Hash.ToLower()
        $remoteMd5 = "$(& $adb shell "md5sum '$remote' 2>/dev/null")".Split(' ')[0]
        if ($remoteMd5 -eq $localMd5) {
            Write-Host "= $($pack.Name) (sin cambios)"
            continue
        }
    }
    Write-Host ("> {0} ({1:N0} MB)" -f $pack.Name, ($pack.Length / 1MB))
    # adb escribe el progreso y el resumen en stderr: con 'Stop', PowerShell 5.1 lo toma como error y aborta.
    $ErrorActionPreference = 'Continue'
    & $adb push $pack.FullName $remote 2>&1 | ForEach-Object { "$_" }
    $pushExit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($pushExit -ne 0) { throw "adb push fallo para $($pack.Name)" }
    if (-not $Force) {
        $remoteMd5 = "$(& $adb shell "md5sum '$remote' 2>/dev/null")".Split(' ')[0]
        if ($remoteMd5 -ne $localMd5) { throw "MD5 distinto tras copiar $($pack.Name) (movil: '$remoteMd5')" }
    }
}
Write-Host 'Packs en el movil:'
& $adb shell ls -l $remoteDir
