# Dot-source to get Get-LieConfig / Get-LieInstance / Find-LieProcess, or run with -Get <key> to print one top-level value.
param([string]$Get)
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

function Get-LieRoot { (Resolve-Path (Join-Path $PSScriptRoot '..')).Path }

function Get-LieConfig {
    $root = Get-LieRoot
    $cfg = Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $root 'devconfig.json') | ConvertFrom-Json
    $localFile = Join-Path $root 'devconfig.local.json'
    if (Test-Path -LiteralPath $localFile) {
        $local = Get-Content -Raw -Encoding UTF8 -LiteralPath $localFile | ConvertFrom-Json
        foreach ($p in $local.PSObject.Properties) { $cfg | Add-Member -NotePropertyName $p.Name -NotePropertyValue $p.Value -Force }
    }
    $tokens = @{ '{magnoliaGame}' = $cfg.magnoliaGame; '{liliesGame}' = $cfg.liliesGame; '{localAppData}' = $env:LOCALAPPDATA }
    $expand = { param($s) if ($s -is [string]) { foreach ($k in $tokens.Keys) { $s = $s.Replace($k, $tokens[$k]) } }; $s }
    foreach ($inst in $cfg.instances.PSObject.Properties) {
        foreach ($f in @($inst.Value.PSObject.Properties)) { $inst.Value.($f.Name) = & $expand $f.Value }
    }
    $cfg
}

function Get-LieInstance([ValidateSet('host', 'guest')][string]$Role) { (Get-LieConfig).instances.$Role }

function Find-LieProcess([ValidateSet('host', 'guest')][string]$Role) {
    $inst = Get-LieInstance $Role
    $exe = Join-Path $inst.root $inst.exe
    @(Get-CimInstance Win32_Process -Filter "Name='$($inst.exe)'" |
        Where-Object { [string]::Equals($_.ExecutablePath, $exe, [StringComparison]::OrdinalIgnoreCase) })
}

if ($Get) { (Get-LieConfig).$Get }
