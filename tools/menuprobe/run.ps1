# Dev tool: dumps EL rest-menu dimmer strips (atlas pixels + ini DPI rule) into out/ under the repo.
# Needs the local CUE4Parse sources (devconfig.json: cue4parse).
param([string]$Out = "")
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\devconfig.ps1')
$cfg = Get-LieConfig
$paks = Join-Path $cfg.liliesGame 'Content\Paks'
$c4p = $cfg.cue4parse
if (-not $c4p) { $c4p = 'C:\Code2\Ender5\CUE4ParseProject\CUE4Parse' }
if (-not $Out) { $Out = Join-Path $PSScriptRoot 'out' }
$tree = if ($args -contains '--tree') { '--tree' } else { '' }
dotnet run --project $PSScriptRoot -c Release -p:CUE4ParseRoot="$c4p" -- --paks $paks --out $Out $tree
if ($LASTEXITCODE -ne 0) { throw "menuprobe failed ($LASTEXITCODE)" }
