<#
.SYNOPSIS
    Crea la clave de firma de las releases (fase 22, ADR-0005 seccion 6): ECDSA P-256 con OpenSSL.

.DESCRIPTION
    Escribe la mitad PRIVADA (PEM) en -PrivateKeyOut, que tiene que estar FUERA del repositorio (y de cualquier otro
    repositorio git), y la PUBLICA en release\release-key.pub.hex: el punto sin comprimir (04 || X || Y) en
    hexadecimal, que CMake compila dentro de la app y del Setup (core/ReleaseSignature). Despues:

      1. Commitea release\release-key.pub.hex: los programas compilados desde ahi solo aceptan manifiestos firmados
         con esta clave.
      2. Guarda la clave privada en la CI como secreto de repositorio RELEASE_SIGNING_KEY (Settings > Secrets and
         variables > Actions de cer0Labs/VanillaDroid; este script imprime la orden de gh; no la ejecuta).
      3. Guarda una copia fuera de linea (un USB, un gestor de contrasenas). Si se pierde, las apps y los Setup ya
         instalados no aceptaran ninguna release firmada con otra clave: habria que actualizarlos a mano.

    Sustituye a la clave de PRUEBA (tests\data\release-test-key.pem, cuya mitad privada es publica). Cambiar una clave
    de verdad por otra (-Force) deja a los programas ya publicados sin actualizaciones automaticas: solo si se ha
    filtrado.

    Usa el openssl del PATH o el de Git for Windows (C:\Program Files\Git\usr\bin\openssl.exe).

.EXAMPLE
    .\scripts\release-key.ps1 -PrivateKeyOut D:\claves\vanilladroid-release.pem
#>
param(
    [Parameter(Mandatory)] [string] $PrivateKeyOut,
    # Replace a real key that is already in release\release-key.pub.hex (only if it leaked).
    [switch] $Force
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'release-common.ps1')

$root = Get-RepoRoot
$keyPath = Resolve-FullPath $PrivateKeyOut
$keyDir = Split-Path -Parent $keyPath

# Never inside this repository nor any other git working tree: a key there is one `git add` away from being public.
$rootFull = [IO.Path]::GetFullPath($root).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
if ($keyPath.StartsWith($rootFull, [StringComparison]::OrdinalIgnoreCase)) {
    throw "La clave privada no puede ir dentro del repositorio ($root): elige una carpeta fuera"
}
if (-not (Test-Path -LiteralPath $keyDir)) {
    throw "No existe la carpeta $keyDir"
}
$ErrorActionPreference = 'Continue' # Windows PowerShell turns a redirected native stderr into errors
$insideGit = & git -C $keyDir rev-parse --is-inside-work-tree 2>$null
$gitCode = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($gitCode -eq 0 -and $insideGit -eq 'true') {
    throw "$keyDir esta dentro de un repositorio git: elige una carpeta fuera de cualquier repositorio"
}
if (Test-Path -LiteralPath $keyPath) {
    throw "$keyPath ya existe: nunca se sobrescribe una clave (borrala a mano si de verdad sobra)"
}

$publicPath = Get-ReleaseKeyPath
$current = Read-PublicKeyHex $publicPath
if ($current -ne (Get-TestKeyHex) -and -not $Force) {
    throw ("release\release-key.pub.hex ya tiene una clave de verdad. Cambiarla deja sin actualizaciones automaticas " +
           "a los programas ya publicados; solo si se ha filtrado, con -Force")
}

Invoke-OpenSsl @('genpkey', '-algorithm', 'EC', '-pkeyopt', 'ec_paramgen_curve:P-256', '-pkeyopt',
                 'ec_param_enc:named_curve', '-out', $keyPath) | Out-Null
# Only the current user may read it.
if ($env:OS -eq 'Windows_NT') {
    & icacls $keyPath /inheritance:r /grant:r "$($env:USERNAME):(R,W)" | Out-Null
} else {
    & chmod 600 $keyPath
}

$point = Get-PublicPointHex $keyPath
Write-Utf8File $publicPath ($point + "`n")

Write-Host ''
Write-Host "Clave privada: $keyPath"
Write-Host "Clave publica: $publicPath"
Write-Host "  $point"
Write-Host ''
Write-Host 'Siguientes pasos:'
Write-Host '  1. Commitear la clave publica:'
Write-Host '       git add release/release-key.pub.hex'
Write-Host '       git commit -m "release: the release signing key"'
Write-Host '  2. El secreto de la CI (de repositorio, en cer0Labs/VanillaDroid), con gh autenticado o en la web:'
Write-Host "       PowerShell:  Get-Content -Raw '$keyPath' | gh secret set RELEASE_SIGNING_KEY --repo cer0Labs/VanillaDroid"
Write-Host "       cmd / bash:  gh secret set RELEASE_SIGNING_KEY --repo cer0Labs/VanillaDroid < `"$keyPath`""
Write-Host '  3. Una copia fuera de linea de la clave privada. Sin ella no se pueden firmar releases que acepten los'
Write-Host '     programas ya instalados.'
