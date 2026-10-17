param([ValidateSet('host', 'guest', 'both')][string]$Role = 'both')
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'devconfig.ps1')

foreach ($r in @('host', 'guest')) {
    if ($Role -ne 'both' -and $Role -ne $r) { continue }
    foreach ($p in Find-LieProcess $r) {
        Write-Output "[stop] $r PID $($p.ProcessId)"
        Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
    }
}
