param(
    [string]$Exe = "",              # VanillaDroidSetup.exe; empty: this checkout's development build
    [string]$Dir = "build\shots",   # where the screenshots go
    [int]$Seconds = 4,              # time to start before the first step
    [string[]]$Steps = @(),         # "tap:x,y" | "move:x,y" | "type:text" | "key:vk" | "wheel:notches,x,y" | "wait:seconds" |
                                    # "shot:name" | "close" (the window's close button) | "cpu:seconds" (idle CPU)
    [string[]]$AppArgs = @(),       # --data-dir <folder> --page <page> --lang <locale> --loopback ...
    [int]$Width = 0, [int]$Height = 0 # resize the window first (client pixels)
)
# Phase 22 (gui): screenshots of VanillaDroid Setup's window without touching the screen or the focus. Starts it with
# --no-focus and VD_SETUP_POSTED_INPUT=1 (a development build then hands the messages posted to its window straight
# to Dear ImGui: SDL alone would follow the real cursor and the focus), posts mouse and keyboard messages to ITS
# window only, captures the window's own content with PrintWindow and closes it. The window is found by its process
# id.
#   .\scripts\setup-window-shots.ps1 -AppArgs '--data-dir','build\setup-test','--page','wow' -Steps 'shot:wow'
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class SetupWin {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder s, int n);
    public static IntPtr WindowOf(uint pid) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => {
            uint p; GetWindowThreadProcessId(h, out p);
            if (p != pid || !IsWindowVisible(h)) return true;
            found = h; return false;
        }, IntPtr.Zero);
        return found;
    }
}
"@
function Shot($h, $out) {
    $r = New-Object SetupWin+RECT
    [SetupWin]::GetClientRect($h, [ref]$r) | Out-Null
    $w = $r.Right - $r.Left; $hgt = $r.Bottom - $r.Top
    $bmp = New-Object System.Drawing.Bitmap $w, $hgt
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    # PW_CLIENTONLY | PW_RENDERFULLCONTENT: the window's own content, never whatever is on screen over it.
    $hdc = $g.GetHdc(); [SetupWin]::PrintWindow($h, $hdc, 3) | Out-Null; $g.ReleaseHdc($hdc)
    $bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    Write-Output "saved $out ($w x $hgt)"
}
function MakeLParam($x, $y) { [IntPtr](([int]$y -shl 16) -bor ([int]$x -band 0xFFFF)) }

$root = Split-Path -Parent $PSScriptRoot
if (-not $Exe) { $Exe = Join-Path $root 'build\windows\tools\setup\RelWithDebInfo\VanillaDroidSetup.exe' }
if (-not [IO.Path]::IsPathRooted($Dir)) { $Dir = Join-Path $root $Dir }
New-Item -ItemType Directory -Force -Path $Dir | Out-Null
$arguments = @('--no-focus') + $AppArgs | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }
$log = Join-Path $Dir 'setup.log'
$previous = $env:VD_SETUP_POSTED_INPUT
$env:VD_SETUP_POSTED_INPUT = '1'
$p = Start-Process -FilePath $Exe -ArgumentList $arguments -WorkingDirectory $root -RedirectStandardOutput $log `
    -RedirectStandardError ($log + '.err') -PassThru
$env:VD_SETUP_POSTED_INPUT = $previous
try {
    Start-Sleep -Seconds $Seconds
    $h = [SetupWin]::WindowOf([uint32]$p.Id)
    if ($h -eq [IntPtr]::Zero) { Write-Output "no window (exited: $($p.HasExited))"; exit 1 }
    if ($Width -gt 0 -and $Height -gt 0) {
        $wr = New-Object SetupWin+RECT; $cr = New-Object SetupWin+RECT
        [SetupWin]::GetWindowRect($h, [ref]$wr) | Out-Null; [SetupWin]::GetClientRect($h, [ref]$cr) | Out-Null
        $extraW = ($wr.Right - $wr.Left) - $cr.Right; $extraH = ($wr.Bottom - $wr.Top) - $cr.Bottom
        # SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE
        [SetupWin]::SetWindowPos($h, [IntPtr]::Zero, 0, 0, $Width + $extraW, $Height + $extraH, 0x16) | Out-Null
        Start-Sleep -Milliseconds 500
    }
    if ($Steps.Count -eq 0) { $Steps = @('shot:shot') }
    foreach ($step in $Steps) {
        $kind, $arg = $step -split ':', 2
        switch ($kind) {
            'tap' {
                $x, $y = $arg -split ','
                $at = MakeLParam $x $y
                [SetupWin]::PostMessageW($h, 0x0200, [IntPtr]0, $at) | Out-Null # WM_MOUSEMOVE
                Start-Sleep -Milliseconds 60
                [SetupWin]::PostMessageW($h, 0x0201, [IntPtr]1, $at) | Out-Null # WM_LBUTTONDOWN
                Start-Sleep -Milliseconds 80
                [SetupWin]::PostMessageW($h, 0x0202, [IntPtr]0, $at) | Out-Null # WM_LBUTTONUP
                Start-Sleep -Milliseconds 300
            }
            'move' {
                $x, $y = $arg -split ','
                [SetupWin]::PostMessageW($h, 0x0200, [IntPtr]0, (MakeLParam $x $y)) | Out-Null
                Start-Sleep -Milliseconds 300
            }
            'type' {
                foreach ($ch in $arg.ToCharArray()) {
                    [SetupWin]::PostMessageW($h, 0x0102, [IntPtr][int]$ch, [IntPtr]1) | Out-Null # WM_CHAR
                    Start-Sleep -Milliseconds 20
                }
            }
            'key' {
                [SetupWin]::PostMessageW($h, 0x0100, [IntPtr][int]$arg, [IntPtr]1) | Out-Null # WM_KEYDOWN
                Start-Sleep -Milliseconds 30
                [SetupWin]::PostMessageW($h, 0x0101, [IntPtr][int]$arg, [IntPtr]0xC0000001) | Out-Null
            }
            'wheel' {
                $n, $x, $y = $arg -split ','
                $w = [IntPtr](([int]$n * 120) -shl 16)
                [SetupWin]::PostMessageW($h, 0x020A, $w, (MakeLParam $x $y)) | Out-Null # WM_MOUSEWHEEL
                Start-Sleep -Milliseconds 300
            }
            'wait' { Start-Sleep -Milliseconds ([int]([double]$arg * 1000)) }
            'spawn' {
                # spawn:<program>|<arguments>: another program in the background (a phone's download with curl)
                $program, $rest = $arg -split '\|', 2
                Start-Process -FilePath $program -ArgumentList $rest -WindowStyle Hidden | Out-Null
            }
            'shot' { Shot $h (Join-Path $Dir ($arg + '.png')) }
            'close' { [SetupWin]::PostMessageW($h, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null; Start-Sleep -Seconds 2 } # WM_CLOSE
            'cpu' {
                # The window idle: CPU time over the interval (% of one core) and memory.
                $p.Refresh(); $before = $p.TotalProcessorTime.TotalMilliseconds
                Start-Sleep -Milliseconds ([int]([double]$arg * 1000))
                $p.Refresh(); $after = $p.TotalProcessorTime.TotalMilliseconds
                "cpu {0:N2} % of one core over {1} s; working set {2:N1} MB, private {3:N1} MB" -f `
                    (($after - $before) / ([double]$arg * 10)), $arg, ($p.WorkingSet64 / 1MB), ($p.PrivateMemorySize64 / 1MB)
            }
        }
    }
} finally {
    if (-not $p.HasExited) { Start-Sleep -Milliseconds 200 }
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Write-Output 'stopped' } else { Write-Output "exited $($p.ExitCode)" }
}
Get-Content ($log + '.err') -ErrorAction SilentlyContinue | Select-Object -First 20
