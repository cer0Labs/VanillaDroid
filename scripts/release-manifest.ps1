<#
.SYNOPSIS
    Escribe el manifest.json de una release y lo firma (fase 22, ADR-0005 seccion 6): lo llama la CI al publicar.

.DESCRIPTION
    manifest.json describe la release vX.Y.Z publicada en -Repository (cer0Labs/VanillaDroid): version, versionCode, fecha, enlace
    a las notas, el APK y los Setup de Windows y macOS (URL, tamano y SHA-256) y las recetas de los packs
    (release\packs.json del commit etiquetado). Lo leen el Setup y la app para avisar y actualizarse.

    Con -SigningKey lo firma: manifest.json.sig es el base64 (una linea) de la firma DER ECDSA P-256 sobre el SHA-256
    de los bytes exactos de manifest.json (openssl dgst -sha256 -sign). Antes comprueba que la clave es la que llevan
    los programas (release\release-key.pub.hex) y que no es la de prueba (tests\data\release-test-key.pem; solo con
    -AllowTestKey, para probar en local); despues verifica la firma contra release\release-key.pub.hex.

    Los ficheros deben llamarse como en la release: VanillaDroid-X.Y.Z.apk, VanillaDroidSetup-X.Y.Z-windows.zip y
    VanillaDroidSetup-X.Y.Z-macos.zip. Si falta un Setup, el manifiesto no lo lleva.

    Con -BaseUrl las URL de los ficheros apuntan alli (http://192.168.1.20:8000/VanillaDroid-X.Y.Z.apk) en lugar de a
    la release de GitHub: una release de prueba servida en la red local (scripts\test-release.ps1 la prepara entera).

.EXAMPLE
    # En local, con la clave de prueba y ficheros de mentira:
    .\scripts\release-manifest.ps1 -Version 0.2.0 -Apk dist\VanillaDroid-0.2.0.apk `
        -SetupWindows dist\VanillaDroidSetup-0.2.0-windows.zip -Out dist\manifest.json `
        -SigningKey tests\data\release-test-key.pem -PublicKey tests\data\release-test-key.pub.hex -AllowTestKey
#>
param(
    [Parameter(Mandatory)] [string] $Version,
    [Parameter(Mandatory)] [string] $Apk,
    [string] $SetupWindows,
    [string] $SetupMacos,
    [Parameter(Mandatory)] [string] $Out,
    # release\packs.json of the tagged commit: the recipes of the packs.
    [string] $Packs,
    # yyyy-MM-dd, the day of the release (UTC today by default).
    [string] $Date,
    [string] $Repository = 'cer0Labs/VanillaDroid',
    # Where the release's files are served instead of GitHub (a test release on the local network); the notes link
    # stays the GitHub page.
    [string] $BaseUrl,
    [string] $SigningKey,
    # The key the programs carry (release\release-key.pub.hex by default): the signature must verify against it.
    [string] $PublicKey,
    [switch] $AllowTestKey
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'release-common.ps1')

$root = Get-RepoRoot
$versionCode = Get-VersionCode $Version
$Packs = if ($Packs) { Resolve-FullPath $Packs } else { Join-Path $root 'release/packs.json' }
$PublicKey = if ($PublicKey) { Resolve-FullPath $PublicKey } else { Get-ReleaseKeyPath }
if ($SigningKey) { $SigningKey = Resolve-FullPath $SigningKey }
if (-not $Date) { $Date = [DateTime]::UtcNow.ToString('yyyy-MM-dd', [Globalization.CultureInfo]::InvariantCulture) }
if ($Date -notmatch '^\d{4}-\d{2}-\d{2}$') { throw "La fecha debe ser yyyy-MM-dd, no '$Date'" }

if ($SigningKey) {
    # The signing key must be the one built into the programs, and never the test key outside of local tests: checked
    # before anything is written.
    $keyHex = Get-PublicPointHex $SigningKey
    $expectedHex = Read-PublicKeyHex $PublicKey
    if ($keyHex -ne $expectedHex) {
        throw "La clave de firma no es la de $PublicKey (la que llevan los programas): su firma no la aceptaria nadie"
    }
    if ($keyHex -eq (Get-TestKeyHex)) {
        if (-not $AllowTestKey) {
            throw 'La clave de firma es la de PRUEBA (tests\data\release-test-key.pem, publica): crea la de verdad con scripts\release-key.ps1'
        }
        Write-Warning 'Firmado con la clave de PRUEBA (solo para probar en local)'
    }
}

$tag = "v$Version"
$downloads = "https://github.com/$Repository/releases/download/$tag"
if ($BaseUrl) {
    if ($BaseUrl -notmatch '^https?://[^/\s]+(/\S*)?$') { throw "-BaseUrl debe ser una URL http(s)://..., no '$BaseUrl'" }
    $downloads = $BaseUrl.TrimEnd('/')
}

# A JSON string: the values here are URLs, hex digests, dates and pack names, but escape them anyway.
function ConvertTo-JsonString {
    param([Parameter(Mandatory)] [AllowEmptyString()] [string] $Text)
    $builder = New-Object Text.StringBuilder
    [void]$builder.Append('"')
    foreach ($c in $Text.ToCharArray()) {
        switch ($c) {
            '"' { [void]$builder.Append('\"'); continue }
            '\' { [void]$builder.Append('\\'); continue }
            default {
                if ([int]$c -lt 0x20) {
                    [void]$builder.Append(('\u{0:x4}' -f [int]$c))
                } else {
                    [void]$builder.Append($c)
                }
            }
        }
    }
    [void]$builder.Append('"')
    return $builder.ToString()
}

# {"url", "size", "sha256"} of a release file, whose name must be the published one.
function Get-Asset {
    param([Parameter(Mandatory)] [string] $Path, [Parameter(Mandatory)] [string] $ExpectedName)
    $file = Get-Item -LiteralPath $Path
    if ($file.Name -cne $ExpectedName) {
        throw "$Path debe llamarse $ExpectedName (el nombre del fichero en la release)"
    }
    return [ordered]@{
        url    = "$downloads/$ExpectedName"
        size   = [int64]$file.Length
        sha256 = Get-Sha256Hex $file.FullName
    }
}

function Format-Asset {
    param([Parameter(Mandatory)] $Asset, [Parameter(Mandatory)] [string] $Indent)
    $inner = "$Indent  "
    return "{`n" +
        "$inner`"url`": $(ConvertTo-JsonString $Asset.url),`n" +
        "$inner`"size`": $($Asset.size),`n" +
        "$inner`"sha256`": $(ConvertTo-JsonString $Asset.sha256)`n" +
        "$Indent}"
}

$apkAsset = Get-Asset $Apk "VanillaDroid-$Version.apk"
$setups = [ordered]@{}
if ($SetupWindows) { $setups['windows'] = Get-Asset $SetupWindows "VanillaDroidSetup-$Version-windows.zip" }
if ($SetupMacos) { $setups['macos'] = Get-Asset $SetupMacos "VanillaDroidSetup-$Version-macos.zip" }

# The recipes, as release\packs.json lists them (sorted by file name: vd_baker digest writes them so).
$recipes = ([IO.File]::ReadAllText($Packs) | ConvertFrom-Json)
if ($recipes.format -ne 1 -or $null -eq $recipes.packs) { throw "$Packs no es un packs.json de formato 1" }
$packLines = @()
foreach ($property in $recipes.packs.PSObject.Properties) {
    $packLines += "    $(ConvertTo-JsonString $property.Name): $(ConvertTo-JsonString ([string]$property.Value))"
}

# The manifest (release_contract: format 1). Two-space indentation, LF, UTF-8 without BOM, a final line end: what
# is signed are these exact bytes.
$lines = @(
    '{',
    '  "format": 1,',
    "  `"version`": $(ConvertTo-JsonString $Version),",
    "  `"versionCode`": $versionCode,",
    "  `"date`": $(ConvertTo-JsonString $Date),",
    "  `"notes`": $(ConvertTo-JsonString "https://github.com/$Repository/releases/tag/$tag"),",
    "  `"apk`": $(Format-Asset $apkAsset '  '),"
)
if ($setups.Count -gt 0) {
    $setupLines = @()
    foreach ($platform in $setups.Keys) {
        $setupLines += "    $(ConvertTo-JsonString $platform): $(Format-Asset $setups[$platform] '    ')"
    }
    $lines += '  "setup": {'
    $lines += ($setupLines -join ",`n")
    $lines += '  },'
}
if ($packLines.Count -gt 0) {
    $lines += '  "packs": {'
    $lines += ($packLines -join ",`n")
    $lines += '  }'
} else {
    $lines += '  "packs": {}'
}
$lines += '}'
$text = ($lines -join "`n") + "`n"

$outPath = Resolve-FullPath $Out
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $outPath) | Out-Null
Write-Utf8File $outPath $text
Remove-Item -LiteralPath "$outPath.sig" -Force -ErrorAction SilentlyContinue # never a stale signature next to it
# It must read back as JSON (the programs parse it with core/Json).
$check = $text | ConvertFrom-Json
if ($check.versionCode -ne $versionCode -or $check.apk.sha256 -ne $apkAsset.sha256) { throw 'manifest.json no se relee igual' }
Write-Host "manifest.json: $outPath ($($setups.Count) Setup, $($packLines.Count) packs)"

if (-not $SigningKey) {
    Write-Host 'Sin -SigningKey: manifest.json sin firmar'
    return
}

$signaturePath = "$outPath.sig"
$der = [IO.Path]::GetTempFileName()
$pem = [IO.Path]::GetTempFileName()
try {
    Invoke-OpenSsl @('dgst', '-sha256', '-sign', $SigningKey, '-out', $der, $outPath) | Out-Null
    $signature = [Convert]::ToBase64String([IO.File]::ReadAllBytes($der))
    [IO.File]::WriteAllText($signaturePath, $signature, (New-Object Text.ASCIIEncoding)) # one line, no line end
    # Verified as the programs will: against the published key, over the bytes on disk.
    Write-PublicKeyPem $expectedHex $pem
    $verified = Invoke-OpenSsl @('dgst', '-sha256', '-verify', $pem, '-signature', $der, $outPath)
    if (($verified -join '') -notmatch 'Verified OK') { throw 'La firma de manifest.json no se verifica' }
} finally {
    Remove-Item -LiteralPath $der, $pem -Force -ErrorAction SilentlyContinue
}
Write-Host "manifest.json.sig: $signaturePath (verificada con $PublicKey)"
