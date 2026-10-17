# Starts isolated instances: -dx11 (D3D11 hooks), windowed 1280x720, own -UserDir, 60 FPS cap.
#   tools\start.ps1 -Role host     # the Host launches the headless Guest itself (LilyInEM.ini)
#   tools\start.ps1 -Role guest    # Guest alone (debugging); set LIE_SHOW_GUEST=1 to keep its window visible
param(
    [ValidateSet('host', 'guest', 'both')][string]$Role = 'host',
    [int]$Width = 1280,
    [int]$Height = 720
)
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'devconfig.ps1')

function Start-Instance([string]$name) {
    $inst = Get-LieInstance $name
    $running = Find-LieProcess $name
    if ($running.Count -gt 0) { Write-Output "[start] $name already running (PID $($running[0].ProcessId))"; return }
    New-Item -ItemType Directory -Force -Path $inst.userDir | Out-Null
    $argv = @('-dx11', '-windowed', "-ResX=$Width", "-ResY=$Height", '-MultiprocessOSS', '-SaveToUserDir', ('-UserDir="' + $inst.userDir + '"'), '-ExecCmds="t.MaxFPS 60"')
    $p = Start-Process -FilePath (Join-Path $inst.root $inst.exe) -WorkingDirectory $inst.root -ArgumentList $argv -PassThru
    Write-Output "[start] $name PID $($p.Id)"
}

if ($Role -in @('guest', 'both')) { Start-Instance 'guest' }
if ($Role -in @('host', 'both')) {
    $env:LIE_DEV_AUTOCONTINUE = '1' # debug Lua continues save slot 0 from the EM title screen
    Start-Instance 'host'
}
