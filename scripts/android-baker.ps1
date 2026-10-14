<#
.SYNOPSIS
    Compila vd_baker para Android (arm64) como programa de consola y, con ADB, lo sube al movil y mide cada paso.

.DESCRIPTION
    Fase 22 (22.7, ADR-0005 punto 8): el baker en el movil. Compila con el CMake 3.22.1 y el Ninja del SDK de Android
    y la toolchain del NDK 28.2.13676358 (arm64-v8a, Android 11+, libc++ estatica: el ejecutable no necesita nada
    mas) en build\android-baker\, en Release y sin descargar nada: las dependencias son las fuentes de una
    compilacion de escritorio (build\windows\_deps\<nombre>-src de este checkout o, si no las tiene y es un segundo
    arbol de trabajo del repositorio, las del principal; o -DepsDir). Espera al cerrojo de compilacion de la maquina
    (Global\VanillaDroidBuild): una compilacion a la vez en este PC.

    En el movil todo vive en /data/local/tmp (el usuario shell de ADB puede escribir ahi):
      vd_baker               el ejecutable                                  (-Push)
      vd/locale, vd/art      textos y arte propios, para --only ui          (-Push)
      wow/Data/*.MPQ         los MPQ oficiales del 1.12.1                   (-PushWow; sin backup.MPQ ni parches)
      packs/                 lo horneado                                    (-Measure)
      bake-measure.log       el registro de la medida                       (-Measure, -Follow)

    -Measure lanza en el movil, desacoplado de la conexion ADB (nohup), un paso del baker tras otro; cada uno acaba
    con su tiempo, su CPU y su pico de memoria ("bake: ..."), y la temperatura de la bateria va antes y despues. El
    script muestra el registro hasta que termina; si la conexion se corta, -Follow lo retoma sin repetir nada.
    Con la pantalla apagada el movil puede dormirse: activar "No bloquear" (opciones de desarrollador) y cargarlo.

    -Compare comprueba que el movil hornea lo mismo que el PC (ADR-0005 punto 3, las recetas): hornea en el PC los
    mismos pasos con el baker de escritorio de este checkout (build\windows\...\vd_baker.exe, con la cache de
    packs\.cache si la hay) en build\android-baker\pc-packs\, calcula "vd_baker digest" de los dos lados y dice pack
    por pack si coinciden. Los dos sin clientes en otros idiomas (el movil no los tiene). De un pack distinto dice
    que recursos cambian ("vd_baker digest-assets" en los dos lados).

.EXAMPLE
    .\scripts\android-baker.ps1                                  # solo compilar
    .\scripts\android-baker.ps1 -NoBuild -Push -PushWow          # subir el baker, lo propio y el WoW (5 GB)
    .\scripts\android-baker.ps1 -NoBuild -Measure                # todos los pasos, sin cache, hilos por defecto
    .\scripts\android-baker.ps1 -NoBuild -Measure -Steps db,bodyfx,cinematics -Jobs 4
    .\scripts\android-baker.ps1 -Follow                          # seguir leyendo una medida en curso
    .\scripts\android-baker.ps1 -NoBuild -Compare                # recetas del movil contra las del PC
    .\scripts\android-baker.ps1 -NoBuild -Push -Measure -DryRun  # ver las ordenes de ADB sin ejecutarlas
    # Con dos moviles conectados: $env:ANDROID_SERIAL = '<serie de adb devices>'
#>
param(
    [switch] $NoBuild,
    [switch] $Push,
    [switch] $PushWow,
    [switch] $Measure,
    [switch] $Follow,
    [switch] $Compare,
    [switch] $DryRun,
    [string] $DepsDir,
    [string] $WowDir = 'C:\World of Warcraft 1.12',
    # Pasos de "vd_baker bake --only", en este orden; world y collision solo con -Maps.
    [string[]] $Steps = @('db', 'textures', 'models', 'actors', 'world', 'collision', 'objects', 'ui', 'sound',
                          'cinematics', 'grass', 'worldsound', 'bodyfx'),
    [string] $Maps = 'azeroth',
    [int] $Jobs = 0,           # hilos que hornean; 0: los del baker (todos los nucleos menos uno)
    [string] $Cache = 'off'    # off | on (packs/.cache) | <carpeta del movil>
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$sdk = Join-Path $env:LOCALAPPDATA 'Android\Sdk'
$ndk = Join-Path $sdk 'ndk\28.2.13676358'
$cmakeBin = Join-Path $sdk 'cmake\3.22.1\bin'
$buildDir = Join-Path $root 'build\android-baker'
$built = Join-Path $buildDir 'tools\baker\vd_baker'
# What goes to the phone: the same without its debug information (46 -> 2 MB). The built one keeps it, for
# reading a crash's addresses (llvm-symbolizer).
$binary = Join-Path $buildDir 'vd_baker'
# Always the SDK adb: a different adb version on PATH (e.g. scrcpy's) keeps killing the ADB server.
$adb = Join-Path $sdk 'platform-tools\adb.exe'
$remote = '/data/local/tmp'
$remoteLog = "$remote/bake-measure.log"

# The official MPQ of the 1.12.1 client (enUS and esES name them alike): backup.MPQ and server patches stay home.
$officialMpq = @('base.MPQ', 'dbc.MPQ', 'fonts.MPQ', 'interface.MPQ', 'misc.MPQ', 'model.MPQ', 'patch.MPQ',
                 'patch-2.MPQ', 'sound.MPQ', 'speech.MPQ', 'terrain.MPQ', 'texture.MPQ', 'wmo.MPQ')

function Invoke-Adb {
    if ($DryRun) {
        Write-Host "adb $args"
        return
    }
    # adb writes its progress to stderr: with 'Stop', Windows PowerShell 5.1 takes it for an error and aborts.
    $ErrorActionPreference = 'Continue'
    & $adb @args 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($code -ne 0) { throw "adb $args fallo ($code)" }
}

# One command line for the device shell, as a single argument (Windows PowerShell mangles quotes it forwards).
function Invoke-Shell([string] $command) {
    if ($DryRun) {
        Write-Host "adb shell `"$command`""
        return @()
    }
    $ErrorActionPreference = 'Continue'
    $out = & $adb shell $command 2>&1 | ForEach-Object { "$_" }
    $ErrorActionPreference = 'Stop'
    return $out
}

if (-not $NoBuild -and -not $Follow) {
    if (-not (Test-Path (Join-Path $ndk 'build\cmake\android.toolchain.cmake'))) { throw "Falta el NDK: $ndk" }
    if (-not (Test-Path (Join-Path $cmakeBin 'cmake.exe'))) { throw "Falta el CMake 3.22.1 del SDK: $cmakeBin" }
    if (-not $DepsDir) {
        # This checkout's desktop build or, in a second working tree of the repository, the main one's.
        $candidates = @(Join-Path $root 'build\windows\_deps')
        try {
            $common = & git -C $root rev-parse --git-common-dir
            if ($LASTEXITCODE -eq 0 -and $common) {
                $main = Split-Path -Parent ([IO.Path]::GetFullPath([IO.Path]::Combine($root, $common)))
                $candidates += Join-Path $main 'build\windows\_deps'
            }
        } catch { }
        foreach ($candidate in $candidates) {
            if (Test-Path (Join-Path $candidate 'stormlib-src')) { $DepsDir = $candidate; break }
        }
    }
    $configure = @('-S', $root, '-B', $buildDir, '-G', 'Ninja',
                   "-DCMAKE_MAKE_PROGRAM=$((Join-Path $cmakeBin 'ninja.exe') -replace '\\', '/')",
                   "-DCMAKE_TOOLCHAIN_FILE=$((Join-Path $ndk 'build\cmake\android.toolchain.cmake') -replace '\\', '/')",
                   '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=30', '-DANDROID_STL=c++_static',
                   '-DCMAKE_BUILD_TYPE=Release', '-DVD_ANDROID_BAKER=ON')
    if ($DepsDir) {
        # The sources of a desktop build, read only: nothing is downloaded.
        $configure += '-DFETCHCONTENT_FULLY_DISCONNECTED=ON'
        foreach ($name in @('vulkan_headers', 'volk', 'vma', 'imgui', 'lz4', 'glm', 'mbedtls', 'miniaudio', 'stormlib',
                            'astcenc')) {
            $source = Join-Path $DepsDir "$name-src"
            if (-not (Test-Path $source)) { throw "Falta $source (configura antes el escritorio: cmake --preset windows)" }
            $configure += "-DFETCHCONTENT_SOURCE_DIR_$($name.ToUpper())=$($source -replace '\\', '/')"
        }
    } else {
        Write-Warning 'No hay fuentes de las dependencias en disco: CMake las descargara'
    }

    # One build at a time on this PC: the same lock as the desktop builds.
    $mutex = New-Object System.Threading.Mutex($false, 'Global\VanillaDroidBuild')
    $waited = [Diagnostics.Stopwatch]::StartNew()
    try { [void]$mutex.WaitOne() } catch [System.Threading.AbandonedMutexException] { }
    Write-Host ('cerrojo de compilacion tras {0:N0} s' -f $waited.Elapsed.TotalSeconds)
    try {
        $sw = [Diagnostics.Stopwatch]::StartNew()
        # The whole output in a log; on a failure, its errors and its last lines.
        New-Item -ItemType Directory -Force $buildDir | Out-Null
        $log = Join-Path $buildDir 'last-build.log'
        & (Join-Path $cmakeBin 'cmake.exe') @configure *> $log
        $code = $LASTEXITCODE
        if ($code -eq 0) {
            & (Join-Path $cmakeBin 'cmake.exe') --build $buildDir --target vd_baker *>> $log
            $code = $LASTEXITCODE
        }
        if ($code -ne 0) {
            Get-Content $log | Where-Object { $_ -match 'error|FAILED|CMake Error' } | Select-Object -First 40
            Get-Content $log -Tail 15
            throw "La compilacion fallo ($code; registro: $log)"
        }
        $strip = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe'
        & $strip --strip-all -o $binary $built
        if ($LASTEXITCODE -ne 0) { throw "llvm-strip fallo ($LASTEXITCODE)" }
        Write-Host ('vd_baker para Android en {0:N0} s: {1} ({2:N1} MB)' -f $sw.Elapsed.TotalSeconds, $binary,
                    ((Get-Item $binary).Length / 1MB))
    } finally {
        $mutex.ReleaseMutex()
        $mutex.Dispose()
    }
}

if ($Push) {
    if (-not (Test-Path $binary)) { throw "No hay $binary (compila primero)" }
    Invoke-Adb push $binary "$remote/vd_baker"
    Invoke-Shell "chmod 755 $remote/vd_baker" | Out-Null
    # Our own texts and art (ui.vdpk): small, pushed whole every time.
    Invoke-Shell "rm -rf $remote/vd && mkdir -p $remote/vd" | Out-Null
    Invoke-Adb push (Join-Path $root 'locale') "$remote/vd/locale"
    Invoke-Adb push (Join-Path $root 'art') "$remote/vd/art"
    # Without arguments it prints its usage: proof that it runs there.
    Invoke-Shell "$remote/vd_baker" | Select-Object -First 1
}

if ($PushWow) {
    $data = Join-Path $WowDir 'Data'
    Invoke-Shell "mkdir -p $remote/wow/Data" | Out-Null
    foreach ($name in $officialMpq) {
        $file = Get-Item (Join-Path $data $name)
        # The MPQ never change: the same size on the phone means it is already there.
        $remoteSize = "$(Invoke-Shell "stat -c %s '$remote/wow/Data/$name' 2>/dev/null")".Trim()
        if ($remoteSize -eq "$($file.Length)") {
            Write-Host "= $name (ya esta)"
            continue
        }
        Write-Host ('> {0} ({1:N0} MB)' -f $name, ($file.Length / 1MB))
        Invoke-Adb push $file.FullName "$remote/wow/Data/$name"
    }
}

if ($Measure) {
    $options = "--wow-dir $remote/wow --out $remote/packs --locale-dir $remote/vd/locale --art-dir $remote/vd/art"
    if ($Jobs -gt 0) { $options += " --jobs $Jobs" }
    if ($Cache -eq 'off') { $options += ' --cache off' } elseif ($Cache -ne 'on') { $options += " --cache $Cache" }
    # A shell script on the phone, started with nohup: a dropped ADB connection does not stop the measurement.
    # Single-quoted PowerShell strings: $(...) belongs to the phone's shell.
    $lines = @('#!/system/bin/sh',
               "cd $remote",
               'temp() { dumpsys battery | grep temperature | tr -d " "; }',
               'echo "=== start $(date +%T) $(temp) cores=$(nproc) $(getprop ro.product.model)"')
    foreach ($step in $Steps) {
        $extra = if ($step -in @('world', 'collision')) { " --maps $Maps" } else { '' }
        $lines += 'echo "=== ' + $step + ' $(date +%T) $(temp)"'
        $lines += "./vd_baker bake $options --only $step$extra"
        $lines += 'echo "=== ' + $step + ' exit $? $(date +%T) $(temp)"'
    }
    $lines += 'ls -l packs'
    $lines += 'echo "=== done $(date +%T)"'
    $script = Join-Path $env:TEMP 'vd-bake-measure.sh'
    # LF line endings: the phone's shell does not take CRLF.
    [IO.File]::WriteAllText($script, (($lines -join "`n") + "`n"))
    if ($DryRun) { Get-Content $script | ForEach-Object { Write-Host "  | $_" } }
    Invoke-Adb push $script "$remote/bake-measure.sh"
    Invoke-Shell ("chmod 755 $remote/bake-measure.sh && rm -f $remoteLog && " +
                  "nohup $remote/bake-measure.sh > $remoteLog 2>&1 < /dev/null &") | Out-Null
    $Follow = $true
}

if ($Follow -and -not $DryRun) {
    # Prints the log as it grows and stops at the last marker. Ctrl+C leaves the bake running on the phone.
    $shown = 0
    for (;;) {
        $log = @(Invoke-Shell "cat $remoteLog 2>/dev/null")
        for ($i = $shown; $i -lt $log.Count; ++$i) { Write-Host $log[$i] }
        $shown = $log.Count
        if ($log -match '^=== done') { break }
        Start-Sleep -Seconds 20
    }
    Write-Host "Registro en el movil: $remoteLog (adb pull $remoteLog)"
}

if ($Compare) {
    # The same steps on the PC, with the desktop baker of this checkout and the client the phone got.
    $desktopBaker = Join-Path $root 'build\windows\tools\baker\RelWithDebInfo\vd_baker.exe'
    if (-not (Test-Path $desktopBaker)) { throw "No hay $desktopBaker (compila antes el escritorio)" }
    $pcPacks = Join-Path $buildDir 'pc-packs'
    $cacheDir = Join-Path $root 'packs\.cache'
    $common = @('--wow-dir', $WowDir, '--out', $pcPacks, '--locale-clients', 'none')
    if (Test-Path $cacheDir) { $common += @('--cache', $cacheDir) } # the same output, only faster
    foreach ($step in $Steps) {
        $extra = if ($step -in @('world', 'collision')) { @('--maps', $Maps) } else { @() }
        Write-Host "PC: bake --only $step"
        if ($DryRun) { continue }
        # A heavy job: the machine's build lock, like a build.
        $mutex = New-Object System.Threading.Mutex($false, 'Global\VanillaDroidBuild')
        try { [void]$mutex.WaitOne() } catch [System.Threading.AbandonedMutexException] { }
        try {
            $ErrorActionPreference = 'Continue'
            & $desktopBaker bake @common --only $step @extra 2>&1 | Where-Object { "$_" -match 'bake: |ERROR' } |
                ForEach-Object { "  $_" }
            $ErrorActionPreference = 'Stop'
        } finally {
            $mutex.ReleaseMutex()
            $mutex.Dispose()
        }
    }
    $phoneDigest = @(Invoke-Shell "cd $remote && ./vd_baker digest --packs packs 2>/dev/null")
    if ($DryRun) { return }
    $ErrorActionPreference = 'Continue'
    $pcDigest = @(& $desktopBaker digest --packs $pcPacks 2>$null)
    $ErrorActionPreference = 'Stop'
    # "name.vdpk": "recipe" lines of both.
    $recipes = { param($text) $map = @{}; foreach ($line in $text) {
            if ("$line" -match '"([^"]+\.vdpk)":\s*"([0-9a-f]+)"') { $map[$Matches[1]] = $Matches[2] } }; $map }
    $phone = & $recipes $phoneDigest
    $pc = & $recipes $pcDigest
    $same = 0
    foreach ($name in ($pc.Keys + $phone.Keys | Sort-Object -Unique)) {
        if (-not $phone.ContainsKey($name)) { Write-Host "falta en el movil: $name" }
        elseif (-not $pc.ContainsKey($name)) { Write-Host "falta en el PC:    $name" }
        elseif ($pc[$name] -eq $phone[$name]) { ++$same }
        else {
            Write-Host ("DISTINTO: {0} (PC {1}, movil {2})" -f $name, $pc[$name], $phone[$name])
            # Which assets: "<digest> <type> <raw size> <name>" of each side, compared by name.
            $assets = { param($text) $map = @{}; foreach ($line in $text) {
                    $fields = "$line" -split ' ', 4; if ($fields.Count -eq 4) { $map[$fields[3]] = $fields[0] } }; $map }
            $phoneAssets = & $assets @(Invoke-Shell "cd $remote && ./vd_baker digest-assets --pack packs/$name 2>/dev/null")
            $ErrorActionPreference = 'Continue'
            $pcAssets = & $assets @(& $desktopBaker digest-assets --pack (Join-Path $pcPacks $name) 2>$null)
            $ErrorActionPreference = 'Stop'
            $differ = @($pcAssets.Keys | Where-Object { $phoneAssets[$_] -ne $pcAssets[$_] } | Sort-Object)
            Write-Host ("  {0} de {1} recursos distintos" -f $differ.Count, $pcAssets.Count)
            $differ | Select-Object -First 25 | ForEach-Object { Write-Host "  $_" }
        }
    }
    Write-Host ("{0} packs iguales de {1}" -f $same, $pc.Count)
}
