# Captures an instance's client area (PrintWindow) to <userDir>\Saved\LIE\shots\<Name>-<role>.png and prints
# the path plus its SHA-256 (two identical hashes in a row = frozen frame, not progress).
param(
    [Parameter(Mandatory)][ValidateSet('host', 'guest')][string]$Role,
    [string]$Name = (Get-Date -Format 'HHmmss')
)
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'devconfig.ps1')
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class LieWin {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr GetWindowDpiAwarenessContext(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    public static IntPtr Find(uint pid) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => {
            uint p; GetWindowThreadProcessId(h, out p);
            if (p != pid) return true;
            var sb = new StringBuilder(64); GetClassName(h, sb, 64);
            if (sb.ToString() == "UnrealWindow") { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
'@
$inst = Get-LieInstance $Role
$proc = Find-LieProcess $Role | Select-Object -First 1
if (-not $proc) { throw "'$Role' is not running" }
$hwnd = [LieWin]::Find([uint32]$proc.ProcessId)
if ($hwnd -eq [IntPtr]::Zero) { throw "no UnrealWindow for '$Role'" }
[void][LieWin]::SetThreadDpiAwarenessContext([LieWin]::GetWindowDpiAwarenessContext($hwnd))
$r = New-Object LieWin+RECT
[void][LieWin]::GetClientRect($hwnd, [ref]$r)
$bmp = New-Object Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
$g = [Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
$ok = [LieWin]::PrintWindow($hwnd, $dc, 3)
$g.ReleaseHdc($dc); $g.Dispose()
if (-not $ok) { $bmp.Dispose(); throw 'PrintWindow failed' }
$dir = Join-Path $inst.userDir 'Saved\LIE\shots'
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$out = Join-Path $dir "$Name-$Role.png"
$bmp.Save($out, [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
Write-Output "$out $((Get-FileHash -LiteralPath $out).Hash.Substring(0, 16))"
