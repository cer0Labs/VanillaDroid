param(
    [string]$Build = "build\windows",               # a configured and built tree (the CI's: Release, VD_DIST=ON)
    [string]$Config = "Release",                    # Release: no PDB and no path of one inside the programs
    [string]$Out = "build\dist\VanillaDroidSetup",  # the folder made, and <Out>.zip next to it
    [string]$Apk = ""                               # the signed dist APK (the CI adds it): VanillaDroid.apk
)
# Phase 22 (gui): VanillaDroid Setup for Windows, ready to publish. Gathers what CMake installs for the component
# "setup" (tools/setup/gui/CMakeLists.txt): VanillaDroidSetup.exe, vdsetup.exe, vd_baker.exe, LICENSE and
# THIRD_PARTY_NOTICES.md (the repository's), adds the APK when given, checks that no PDB went in and zips it.
#   cmake --build build\windows --config Release --target VanillaDroidSetup
#   .\scripts\package-setup.ps1 -Apk apps\android\app\build\outputs\apk\dist\app-dist.apk
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not [IO.Path]::IsPathRooted($Build)) { $Build = Join-Path $root $Build }
if (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $root $Out }
if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
& cmake --install $Build --config $Config --component setup --prefix $Out
if ($LASTEXITCODE -ne 0) { throw "cmake --install failed ($LASTEXITCODE)" }
if ($Apk) { Copy-Item $Apk (Join-Path $Out 'VanillaDroid.apk') }
foreach ($name in @('VanillaDroidSetup.exe', 'vdsetup.exe', 'vd_baker.exe', 'LICENSE', 'THIRD_PARTY_NOTICES.md')) {
    if (-not (Test-Path (Join-Path $Out $name))) { throw "$name is missing from $Out" }
}
$pdb = @(Get-ChildItem $Out -Recurse -Include *.pdb, *.ilk, *.lib, *.exp)
if ($pdb.Count -gt 0) { throw "files that must not ship: $($pdb.Name -join ', ')" }
$zip = "$Out.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $Out '*') -DestinationPath $zip
Get-ChildItem $Out | Select-Object Name, Length | Format-Table -AutoSize | Out-String
"{0} ({1:N1} MB)" -f $zip, ((Get-Item $zip).Length / 1MB)
