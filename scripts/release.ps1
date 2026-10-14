<#
.SYNOPSIS
    Marca una release de VanillaDroid: hornea con los clientes de referencia, escribe release\packs.json y las notas,
    hace el commit y la etiqueta vX.Y.Z (fase 22, ADR-0005 seccion 6). No sube nada.

.DESCRIPTION
    Pasos, en este orden:
      1. Comprueba: version X.Y.Z mayor que la ultima etiqueta v*, rama main, arbol limpio (solo puede haber cambios
         en las notas de esta version) y que release\release-key.pub.hex no es la clave de PRUEBA.
      2. Las notas para los jugadores, release\notes\vX.Y.Z.md, en ingles y en castellano (el ingles primero): de
         -NotesFile, de ese fichero si ya existe, o de una plantilla que abre en el Bloc de notas. NUNCA los mensajes
         de commit (cuentan como funciona por dentro). Son el texto de la release publica.
      3. Compila vd_baker y vdsetup (preset windows-release), bajo el cerrojo de compilacion de la maquina.
      4. Hornea todo, incremental (la cache de packs\.cache), con el cliente enUS y el esES de referencia en -Packs
         (por defecto packs\ de este repositorio). Los clientes tienen que estar limpios: solo los MPQ oficiales.
      5. vd_baker digest -> release\packs.json (las recetas: que packs cambian para los jugadores) y un resumen.
      6. Commit ("release X.Y.Z: ...", en ingles) y etiqueta anotada vX.Y.Z. Imprime las ordenes de git push: subir la
         etiqueta lanza la CI (.github\workflows\release.yml), que publica la release en este repositorio.

    -DryRun hace todo menos el commit y la etiqueta: release\packs.json y las notas van a -OutDir (por defecto una
    carpeta temporal) y la clave de prueba o otra rama solo avisan.

.EXAMPLE
    .\scripts\release.ps1 -Version 0.2.0
    .\scripts\release.ps1 -Version 0.2.0 -NotesFile D:\notas-0.2.0.md
    .\scripts\release.ps1 -Version 0.2.0 -DryRun -SkipBake -OutDir $env:TEMP\prueba-release
#>
param(
    [Parameter(Mandatory)] [string] $Version,
    [string] $NotesFile,
    [string] $Packs,
    [string] $WowDir = 'C:\World of Warcraft 1.12',
    [string] $LocaleWowDir = 'C:\World of Warcraft 1.12 esES',
    # The tools are already built / the packs already baked with this commit (they are digested as they are).
    [switch] $SkipBuild,
    [switch] $SkipBake,
    # No `git fetch --tags` before comparing the version with the last tag.
    [switch] $Offline,
    [switch] $DryRun,
    [string] $OutDir
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'release-common.ps1')

$root = Get-RepoRoot
$versionCode = Get-VersionCode $Version
$tag = "v$Version"
$Packs = if ($Packs) { Resolve-FullPath $Packs } else { Join-Path $root 'packs' }
$baker = Join-Path $root 'build\windows\tools\baker\RelWithDebInfo\vd_baker.exe'
$notesRelative = "release/notes/$tag.md"
$enye = [string][char]0x00F1 # the script stays ASCII: Windows PowerShell reads a .ps1 without BOM as ANSI
$spanishHeading = "## Espa$($enye)ol"

# Native programs log to stderr: with 'Stop', Windows PowerShell would turn a redirected stderr into errors. Their exit
# code decides.
function Invoke-Git {
    $ErrorActionPreference = 'Continue'
    $output = & git -C $root @args
    if ($LASTEXITCODE -ne 0) { throw "git $($args -join ' ') fallo ($LASTEXITCODE)" }
    return $output
}

# A step that may stop the release; in a dry run, only a warning.
function Stop-Release {
    param([Parameter(Mandatory)] [string] $Message)
    if ($DryRun) {
        Write-Warning "$Message (prueba: sigo)"
    } else {
        throw $Message
    }
}

# One heavy job at a time on this PC (the same lock as scripts\android-baker.ps1 and the agents' builds).
function Invoke-WithBuildLock {
    param([Parameter(Mandatory)] [scriptblock] $Body)
    $mutex = New-Object System.Threading.Mutex($false, 'Global\VanillaDroidBuild')
    try { [void]$mutex.WaitOne() } catch [System.Threading.AbandonedMutexException] { }
    try {
        & $Body
    } finally {
        $mutex.ReleaseMutex()
        $mutex.Dispose()
    }
}

function Read-Utf8 {
    param([Parameter(Mandatory)] [string] $Path)
    return [IO.File]::ReadAllText($Path, (New-Object Text.UTF8Encoding $false))
}

# The notes: a Spanish and an English section, each with some text (HTML comments do not count: they are not shown).
function Test-Notes {
    param([Parameter(Mandatory)] [string] $Path)
    $text = [regex]::Replace((Read-Utf8 $Path), '(?s)<!--.*?-->', '')
    $problems = @()
    foreach ($heading in @('## English', $spanishHeading)) {
        $match = [regex]::Match($text, "(?ms)^$([regex]::Escape($heading))[ \t]*\r?\n(.*?)(?=^## |\z)")
        if (-not $match.Success) {
            $problems += "falta la seccion '$heading'"
        } elseif ($match.Groups[1].Value -notmatch '\p{L}') {
            $problems += "la seccion '$heading' esta vacia"
        }
    }
    return $problems
}

function New-NotesTemplate {
    param([Parameter(Mandatory)] [string] $Path)
    $template = @(
        "<!-- Notas de VanillaDroid $Version para los jugadores, en ingles y en castellano: se publican tal cual en",
        "     https://github.com/cer0Labs/VanillaDroid/releases. Cuenta lo que cambia para quien juega (novedades,",
        "     arreglos, si hay que volver a preparar los datos con VanillaDroid Setup), nunca como funciona por dentro:",
        "     no copies los mensajes de commit. Estos comentarios no se ven. -->",
        '',
        '## English',
        '',
        '- ',
        '',
        $spanishHeading,
        '',
        '- ',
        ''
    ) -join "`n"
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) | Out-Null
    Write-Utf8File $Path $template
}

# "world_azeroth.vdpk" -> "world" (pack::bakeStepOf, for the summary only).
function Get-BakeStep {
    param([Parameter(Mandatory)] [string] $File)
    $name = $File -replace '\.vdpk$', ''
    if ($name -match '^[a-z]+\.[a-zA-Z]{4}$') { return 'locale' }
    if ($name -eq 'collision_objects') { return 'objects' }
    if ($name -match '^(world|collision)_') { return $Matches[1] }
    if ($name -eq 'liquids') { return 'world' }
    return $name
}

function Read-Recipes {
    param([AllowEmptyString()] [string] $Json)
    $recipes = [ordered]@{}
    if ($Json) {
        $parsed = $Json | ConvertFrom-Json
        foreach ($property in $parsed.packs.PSObject.Properties) { $recipes[$property.Name] = [string]$property.Value }
    }
    return $recipes
}

# --- 1. Checks --------------------------------------------------------------------------------------------------
Write-Host "== VanillaDroid $Version ($tag, versionCode $versionCode)$(if ($DryRun) { ' - PRUEBA (-DryRun)' })"

$branch = (Invoke-Git rev-parse --abbrev-ref HEAD) -join ''
if ($branch -ne 'main') { Stop-Release "Las releases salen de main, no de '$branch'" }

$dirty = @(Invoke-Git status --porcelain --untracked-files=all) | Where-Object { $_ }
$unexpected = @($dirty | Where-Object { $_.Substring(3).Trim('"') -ne $notesRelative })
if ($unexpected.Count -gt 0) {
    throw ("El arbol tiene cambios sin commitear (solo se admiten las notas $notesRelative):`n  " +
           ($unexpected -join "`n  "))
}

if (-not $Offline) {
    $ErrorActionPreference = 'Continue'
    & git -C $root fetch --tags --quiet origin
    $fetched = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($fetched -ne 0) { Write-Warning 'git fetch --tags fallo: comparo solo con las etiquetas locales' }
}
$tags = @(Invoke-Git tag --list 'v*') | Where-Object { $_ -match '^v\d+\.\d+\.\d+$' }
if ($tags -contains $tag) { throw "La etiqueta $tag ya existe" }
$last = $tags | ForEach-Object { [version]$_.Substring(1) } | Sort-Object | Select-Object -Last 1
if ($last -and [version]$Version -le $last) { throw "La version $Version no es mayor que la ultima, v$last" }
Write-Host "Ultima etiqueta: $(if ($last) { "v$last" } else { '(ninguna)' })"

if ((Read-PublicKeyHex (Get-ReleaseKeyPath)) -eq (Get-TestKeyHex)) {
    Stop-Release ('release\release-key.pub.hex es la clave de PRUEBA (su mitad privada es publica): crea la de ' +
                  'verdad con scripts\release-key.ps1, commitea la publica y pon la privada en la CI')
}

if (-not $OutDir) { $OutDir = Join-Path ([IO.Path]::GetTempPath()) "vanilladroid-release-$Version" }
$OutDir = Resolve-FullPath $OutDir
$packsJson = if ($DryRun) { Join-Path $OutDir 'packs.json' } else { Join-Path $root 'release\packs.json' }
$notesPath = if ($DryRun) { Join-Path $OutDir "notes\$tag.md" } else { Join-Path $root "release\notes\$tag.md" }
if ($DryRun) { New-Item -ItemType Directory -Force -Path $OutDir | Out-Null }

# --- 2. Notes (first: the rest runs unattended) -------------------------------------------------------------------
$repoNotes = Join-Path $root "release\notes\$tag.md"
if ($NotesFile) {
    $source = Resolve-FullPath $NotesFile
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $notesPath) | Out-Null
    if ($source -ne $notesPath) { Copy-Item -LiteralPath $source -Destination $notesPath -Force }
} elseif (Test-Path -LiteralPath $repoNotes) {
    if ($repoNotes -ne $notesPath) {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $notesPath) | Out-Null
        Copy-Item -LiteralPath $repoNotes -Destination $notesPath -Force
    }
} else {
    New-NotesTemplate $notesPath
}
while ($true) {
    $problems = @(Test-Notes $notesPath)
    if ($problems.Count -eq 0) { break }
    Write-Host "Notas ($notesPath): $($problems -join '; ')"
    Write-Host 'Escribelas en castellano y en ingles, para los jugadores, en el Bloc de notas que se abre.'
    # Not -Wait: the Notepad of Windows 11 returns at once.
    Start-Process -FilePath 'notepad.exe' -ArgumentList "`"$notesPath`""
    $answer = Read-Host 'Guardalas y pulsa Intro ("q" cancela la release)'
    if ($answer -eq 'q') { throw 'Cancelado: sin notas no hay release' }
}
Write-Host "Notas: $notesPath"

# --- 3. Build -----------------------------------------------------------------------------------------------------
if (-not $SkipBuild) {
    Write-Host '== Compilando vd_baker y vdsetup (windows-release)'
    Invoke-WithBuildLock {
        $ErrorActionPreference = 'Continue'
        Push-Location $root
        try {
            if (-not (Test-Path -LiteralPath (Join-Path $root 'build\windows\CMakeCache.txt'))) {
                & cmake --preset windows
                if ($LASTEXITCODE -ne 0) { throw "cmake --preset windows fallo ($LASTEXITCODE)" }
            }
            & cmake --build --preset windows-release --target vd_baker vdsetup
            if ($LASTEXITCODE -ne 0) { throw "La compilacion fallo ($LASTEXITCODE)" }
        } finally {
            Pop-Location
        }
    }
}
if (-not (Test-Path -LiteralPath $baker)) { throw "Falta $baker (compila sin -SkipBuild)" }

# --- 4. Bake ------------------------------------------------------------------------------------------------------
if (-not $SkipBake) {
    # The reference clients: clean 1.12.1 (5875) enUS and esES, only the official archives (the recipes are what a
    # player's clean client bakes).
    $official = @('backup.mpq', 'base.mpq', 'dbc.mpq', 'fonts.mpq', 'interface.mpq', 'misc.mpq', 'model.mpq',
                  'patch.mpq', 'patch-2.mpq', 'sound.mpq', 'speech.mpq', 'terrain.mpq', 'texture.mpq', 'wmo.mpq')
    foreach ($client in @($WowDir, $LocaleWowDir)) {
        $data = Join-Path $client 'Data'
        if (-not (Test-Path -LiteralPath $data)) { throw "No encuentro $data (cliente de referencia)" }
        $extra = @(Get-ChildItem -LiteralPath $data -Filter '*.mpq' -File |
                   Where-Object { $official -notcontains $_.Name.ToLowerInvariant() } | ForEach-Object { $_.Name })
        if ($extra.Count -gt 0) {
            throw "$data tiene MPQ que no son del 1.12.1 oficial ($($extra -join ', ')): los clientes de referencia deben estar limpios"
        }
    }
    Write-Host "== Horneando en $Packs (incremental; $WowDir + $LocaleWowDir)"
    New-Item -ItemType Directory -Force -Path $Packs | Out-Null
    $bakeWatch = [Diagnostics.Stopwatch]::StartNew()
    Invoke-WithBuildLock {
        $ErrorActionPreference = 'Continue'
        & $baker bake --out $Packs --wow-dir $WowDir --locale-clients $LocaleWowDir
        if ($LASTEXITCODE -ne 0) { throw "vd_baker bake fallo ($LASTEXITCODE)" }
    }
    Write-Host ('Horneado en {0:N0} s' -f $bakeWatch.Elapsed.TotalSeconds)
}

# --- 5. Recipes ---------------------------------------------------------------------------------------------------
Write-Host "== Recetas: vd_baker digest --packs $Packs"
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $packsJson) | Out-Null
Invoke-WithBuildLock {
    $ErrorActionPreference = 'Continue'
    & $baker digest --packs $Packs --out $packsJson
    if ($LASTEXITCODE -ne 0) { throw "vd_baker digest fallo ($LASTEXITCODE)" }
}

$ErrorActionPreference = 'Continue'
$previousJson = (& git -C $root show "HEAD:release/packs.json" 2>$null) -join "`n"
$ErrorActionPreference = 'Stop'
$before = Read-Recipes $previousJson
$after = Read-Recipes (Read-Utf8 $packsJson)
$changed = @($after.Keys | Where-Object { $before.Contains($_) -and $before[$_] -ne $after[$_] })
$added = @($after.Keys | Where-Object { -not $before.Contains($_) })
$removed = @($before.Keys | Where-Object { -not $after.Contains($_) })
$steps = @(($changed + $added) | ForEach-Object { Get-BakeStep $_ } | Sort-Object -Unique)
Write-Host "Packs: $($after.Count) ($($changed.Count) cambian, $($added.Count) nuevos, $($removed.Count) ya no estan)"
if ($changed.Count + $added.Count -eq 0) {
    Write-Host 'Ningun pack cambia: los jugadores solo necesitan el APK y el Setup nuevos.'
} elseif ($before.Count -eq 0) {
    Write-Host 'Primera release con recetas: los Setup y las apps de antes no sabian que hornear de nuevo.'
} else {
    Write-Host "Los jugadores vuelven a hornear: $($steps -join ', ')"
    foreach ($file in $changed) { Write-Host "  cambia  $file" }
    foreach ($file in $added) { Write-Host "  nuevo   $file" }
}
if ($removed.Count -gt 0) {
    foreach ($file in $removed) { Write-Host "  falta   $file" }
    Stop-Release "Faltan $($removed.Count) packs que tenia la release anterior en $Packs (un horneado a medias?)"
}

# --- 6. Commit and tag --------------------------------------------------------------------------------------------
$summary = if ($before.Count -eq 0) {
    "the first ones, $($after.Count) packs"
} elseif ($changed.Count + $added.Count -eq 0) {
    "no pack changes"
} else {
    "$($changed.Count + $added.Count) of $($after.Count) packs change: $($steps -join ', ')"
}
$message = "release ${Version}: release notes and pack recipes ($summary)"
if ($DryRun) {
    Write-Host ''
    Write-Host "PRUEBA: sin commit ni etiqueta. Se habria hecho:"
    Write-Host "  git add release/packs.json $notesRelative"
    Write-Host "  git commit -m `"$message`""
    Write-Host "  git tag -a $tag -m `"VanillaDroid $Version`""
    Write-Host "Ficheros en $OutDir"
    return
}

Invoke-Git add 'release/packs.json' $notesRelative | Out-Null
$messageFile = [IO.Path]::GetTempFileName()
try {
    Write-Utf8File $messageFile ($message + "`n")
    Invoke-Git commit --quiet -F $messageFile | Out-Null
} finally {
    Remove-Item -LiteralPath $messageFile -Force -ErrorAction SilentlyContinue
}
Invoke-Git tag -a $tag -m "VanillaDroid $Version" | Out-Null
$commit = (Invoke-Git rev-parse --short HEAD) -join ''
Write-Host ''
Write-Host "Hecho: commit $commit y etiqueta $tag (nada subido). Para publicar:"
Write-Host '  git push origin main'
Write-Host "  git push origin $tag"
Write-Host "La CI (Actions de este repositorio) compila, firma y publica la release."
