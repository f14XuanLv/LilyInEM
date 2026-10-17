# Runs Lua on an instance's game thread through UEBridge (<root>\ue4ss\bridge\request.json / response.json).
#   tools\eval.ps1 -Role host -Code "return LIE_HostStatus()"
# Each eval is its own chunk: persist values across calls in _G.
param(
    [Parameter(Mandatory)][ValidateSet('host', 'guest')][string]$Role,
    [Parameter(Mandatory)][string]$Code,
    [int]$TimeoutMs = 10000
)
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'devconfig.ps1')

$dir = Join-Path (Get-LieInstance $Role).root 'ue4ss\bridge'
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$req = Join-Path $dir 'request.json'
$resp = Join-Path $dir 'response.json'
if (Test-Path -LiteralPath $resp) { Remove-Item -LiteralPath $resp -Force }
$payload = @{ id = [Guid]::NewGuid().ToString('N'); op = 'eval'; code = $Code } | ConvertTo-Json -Compress
[IO.File]::WriteAllText($req, $payload, (New-Object Text.UTF8Encoding($false)))
$sw = [Diagnostics.Stopwatch]::StartNew()
while ($sw.ElapsedMilliseconds -lt $TimeoutMs) {
    if (Test-Path -LiteralPath $resp) {
        Start-Sleep -Milliseconds 30
        $raw = [IO.File]::ReadAllText($resp, [Text.Encoding]::UTF8)
        if ($raw) { Write-Output $raw; return }
    }
    Start-Sleep -Milliseconds 50
}
throw "timeout waiting for UEBridge on '$Role'"
