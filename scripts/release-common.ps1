<#
.SYNOPSIS
    Funciones comunes de las releases (fase 22, ADR-0005, seccion 6): la clave de firma, OpenSSL, SHA-256, UTF-8.

.DESCRIPTION
    Las cargan con ". $PSScriptRoot\release-common.ps1" scripts\release.ps1, scripts\release-key.ps1 y
    scripts\release-manifest.ps1. Valen en Windows PowerShell 5.1 (este PC) y en PowerShell 7 (la CI, tambien en
    Linux).

    La clave de las releases es ECDSA P-256. Su mitad publica va en release\release-key.pub.hex como el punto sin
    comprimir (04 || X || Y, 65 bytes) en hexadecimal y en minusculas, una linea; CMake la compila dentro de los
    programas (core/ReleaseSignature). La privada nunca esta en el repositorio: es el secreto RELEASE_SIGNING_KEY de la
    CI y una copia fuera de linea. Hasta que se cree la de verdad (scripts\release-key.ps1),
    release\release-key.pub.hex es la clave de PRUEBA de tests\data\release-test-key.pem, cuya mitad privada es publica:
    las releases la rechazan.
#>

# The DER SubjectPublicKeyInfo of a P-256 key is this prefix (id-ecPublicKey, prime256v1, BIT STRING of 66 bytes)
# followed by the 65 bytes of the uncompressed point.
$script:P256SpkiPrefix = '3059301306072a8648ce3d020106082a8648ce3d030107034200'

function Get-RepoRoot {
    return (Split-Path -Parent $PSScriptRoot)
}

# A path made absolute against PowerShell's current folder (.NET calls resolve against the process's, not that one).
function Resolve-FullPath {
    param([Parameter(Mandatory)] [string] $Path)
    $here = (Get-Location -PSProvider FileSystem).ProviderPath
    return [IO.Path]::GetFullPath([IO.Path]::Combine($here, $Path))
}

# OpenSSL: the one on PATH, else the one of Git for Windows.
function Get-OpenSsl {
    $command = Get-Command openssl -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) { return $command.Source }
    foreach ($candidate in @("$env:ProgramFiles\Git\usr\bin\openssl.exe", "$env:ProgramFiles\Git\mingw64\bin\openssl.exe")) {
        if ($env:ProgramFiles -and (Test-Path -LiteralPath $candidate)) { return $candidate }
    }
    throw 'No encuentro openssl (ni en el PATH ni el de Git for Windows): instala Git for Windows o OpenSSL'
}

function Invoke-OpenSsl {
    param([Parameter(Mandatory)] [string[]] $Arguments)
    $openssl = Get-OpenSsl
    $ErrorActionPreference = 'Continue' # its warnings go to stderr: the exit code decides
    $output = & $openssl @Arguments
    if ($LASTEXITCODE -ne 0) { throw "openssl $($Arguments[0]) fallo ($LASTEXITCODE)" }
    return $output
}

function ConvertTo-HexString {
    param([Parameter(Mandatory)] [byte[]] $Bytes)
    return (($Bytes | ForEach-Object { $_.ToString('x2') }) -join '')
}

function ConvertFrom-HexString {
    param([Parameter(Mandatory)] [string] $Hex)
    $bytes = New-Object byte[] ($Hex.Length / 2)
    for ($i = 0; $i -lt $bytes.Length; $i++) {
        $bytes[$i] = [Convert]::ToByte($Hex.Substring($i * 2, 2), 16)
    }
    return , $bytes
}

function Write-Utf8File {
    param([Parameter(Mandatory)] [string] $Path, [Parameter(Mandatory)] [AllowEmptyString()] [string] $Text)
    [IO.File]::WriteAllText($Path, $Text, (New-Object Text.UTF8Encoding $false)) # no BOM
}

function Get-Sha256Hex {
    param([Parameter(Mandatory)] [string] $Path)
    $sha = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($Path)
    try {
        return (ConvertTo-HexString ($sha.ComputeHash($stream)))
    } finally {
        $stream.Dispose()
        $sha.Dispose()
    }
}

# A key file of ours: the point in hex, checked (04 || X || Y, lowercase).
function Read-PublicKeyHex {
    param([Parameter(Mandatory)] [string] $Path)
    $hex = ([IO.File]::ReadAllText($Path)).Trim()
    if ($hex -cnotmatch '^04[0-9a-f]{128}$') {
        throw "$Path no es un punto P-256 sin comprimir (04 || X || Y: 130 cifras hexadecimales en minusculas)"
    }
    return $hex
}

# The public point (hex) of a private key in PEM, by OpenSSL; it must be a P-256 key.
function Get-PublicPointHex {
    param([Parameter(Mandatory)] [string] $PrivateKeyPem)
    $der = [IO.Path]::GetTempFileName()
    try {
        Invoke-OpenSsl @('pkey', '-in', $PrivateKeyPem, '-pubout', '-outform', 'DER', '-out', $der) | Out-Null
        $spki = ConvertTo-HexString ([IO.File]::ReadAllBytes($der))
    } finally {
        Remove-Item -LiteralPath $der -Force -ErrorAction SilentlyContinue
    }
    if (-not $spki.StartsWith($script:P256SpkiPrefix) -or $spki.Length -ne $script:P256SpkiPrefix.Length + 130) {
        throw "$PrivateKeyPem no es una clave ECDSA P-256 (prime256v1)"
    }
    return $spki.Substring($script:P256SpkiPrefix.Length)
}

# The public key in PEM (SubjectPublicKeyInfo) for `openssl dgst -verify`, from the point in hex.
function Write-PublicKeyPem {
    param([Parameter(Mandatory)] [string] $PointHex, [Parameter(Mandatory)] [string] $Path)
    $base64 = [Convert]::ToBase64String((ConvertFrom-HexString ($script:P256SpkiPrefix + $PointHex)))
    $lines = @('-----BEGIN PUBLIC KEY-----')
    for ($i = 0; $i -lt $base64.Length; $i += 64) {
        $lines += $base64.Substring($i, [Math]::Min(64, $base64.Length - $i))
    }
    $lines += '-----END PUBLIC KEY-----'
    Write-Utf8File $Path (($lines -join "`n") + "`n")
}

# The test key's point (tests\data\release-test-key.pub.hex): no release may be signed for it.
function Get-TestKeyHex {
    return (Read-PublicKeyHex (Join-Path (Get-RepoRoot) 'tests/data/release-test-key.pub.hex'))
}

function Get-ReleaseKeyPath {
    return (Join-Path (Get-RepoRoot) 'release/release-key.pub.hex')
}

# X.Y.Z -> X * 1,000,000 + Y * 1,000 + Z (core/Version.h, Gradle's versionCode).
function Get-VersionCode {
    param([Parameter(Mandatory)] [string] $Version)
    if ($Version -notmatch '^(\d{1,3})\.(\d{1,3})\.(\d{1,3})$') { throw "La version debe ser X.Y.Z, no '$Version'" }
    return [int]$Matches[1] * 1000000 + [int]$Matches[2] * 1000 + [int]$Matches[3]
}
