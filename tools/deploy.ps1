# Deploys the built mods into the isolated LilyInEM instances (paths from devconfig.json only).
#   tools\deploy.ps1 [-Target all|host|guest] [-ScriptsOnly]
# Per role: main.dll, data/binary.json (EL.json / EM.json), debug Lua, enabled.txt, a mods.txt entry,
# steam_appid.txt, and an isolated -UserDir seeded with a copy of save slot 0 (the source is never modified).
param(
    [ValidateSet('all', 'host', 'guest')][string]$Target = 'all',
    [switch]$ScriptsOnly
)
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'devconfig.ps1')

$repo = Get-LieRoot
$cfg = Get-LieConfig

function Set-ModsTxtEntry([string]$file, [string]$mod) {
    $lines = if (Test-Path -LiteralPath $file) { @(Get-Content -LiteralPath $file -Encoding UTF8) } else { @() }
    $lines = @($lines | Where-Object { $_ -notmatch "^\s*$([regex]::Escape($mod))\s*:" })
    # UE4SS requires the built-in Keybinds entry to stay last
    $idx = [Array]::FindIndex([string[]]$lines, [Predicate[string]] { param($l) $l -match '^\s*;\s*Built-in keybinds' })
    if ($idx -lt 0) { $lines += "$mod : 1" } else { $lines = @($lines[0..($idx - 1)]) + "$mod : 1" + @($lines[$idx..($lines.Count - 1)]) }
    Set-Content -LiteralPath $file -Value $lines -Encoding UTF8
}

function Initialize-UserDir($inst) {
    $saves = Join-Path $inst.userDir 'Saved\SaveGames'
    New-Item -ItemType Directory -Force -Path $saves | Out-Null
    foreach ($f in @('GameSlot_0.sav', 'SettingsSlot.sav', 'AchievementsSlot.sav')) {
        $dst = Join-Path $saves $f
        $src = Join-Path $inst.saveSeed $f
        if (-not (Test-Path -LiteralPath $dst) -and (Test-Path -LiteralPath $src)) {
            Copy-Item -LiteralPath $src -Destination $dst
            Write-Output "[deploy] seeded $f -> $saves"
        }
    }
}

function Deploy-Role([string]$role) {
    $inst = $cfg.instances.$role
    $mod = $inst.mod
    if (-not (Test-Path -LiteralPath (Join-Path $inst.root $inst.exe))) { throw "instance '$role' has no game exe: $($inst.root)" }
    if (-not $ScriptsOnly -and (Find-LieProcess $role).Count -gt 0) { throw "instance '$role' is running (main.dll is locked): run tools\stop.ps1 first" }

    $modDir = Join-Path $inst.root "ue4ss\Mods\$mod"
    foreach ($d in @('dlls', 'data', 'Scripts')) { New-Item -ItemType Directory -Force -Path (Join-Path $modDir $d) | Out-Null }
    if (-not $ScriptsOnly) {
        $dll = Join-Path $repo "build\mods\$mod\dlls\main.dll"
        if (-not (Test-Path -LiteralPath $dll)) { throw "missing $dll (run build.bat)" }
        Copy-Item -LiteralPath $dll -Destination (Join-Path $modDir 'dlls\main.dll') -Force
        $pdb = [IO.Path]::ChangeExtension($dll, '.pdb')
        if (Test-Path -LiteralPath $pdb) { Copy-Item -LiteralPath $pdb -Destination (Join-Path $modDir 'dlls\main.pdb') -Force }
    }
    $manifest = if ($role -eq 'host') { 'EM.json' } else { 'EL.json' }
    Copy-Item -LiteralPath (Join-Path $repo "data\binary\$manifest") -Destination (Join-Path $modDir 'data\binary.json') -Force
    $lua = Join-Path $repo "mods\$mod\Scripts"
    if (Test-Path -LiteralPath $lua) { Copy-Item -Path (Join-Path $lua '*') -Destination (Join-Path $modDir 'Scripts') -Recurse -Force }
    Set-Content -LiteralPath (Join-Path $modDir 'enabled.txt') -Value '' -NoNewline
    Set-ModsTxtEntry (Join-Path $inst.root 'ue4ss\Mods\mods.txt') $mod

    $appId = Join-Path $inst.root 'steam_appid.txt'
    if (-not (Test-Path -LiteralPath $appId)) { Set-Content -LiteralPath $appId -Value $inst.appId -Encoding ASCII -NoNewline }
    Initialize-UserDir $inst

    if ($role -eq 'host') {
        # FR-ENG-5: the Host (parent) launches the headless Guest from this file
        $g = $cfg.instances.guest
        $ini = @('[Guest]', 'AutoLaunch=1', ('Exe=' + (Join-Path $g.root $g.exe)), ('UserDir=' + $g.userDir), 'ExtraArgs=-ExecCmds="t.MaxFPS 60"') -join "`r`n"
        Set-Content -LiteralPath (Join-Path $modDir 'LilyInEM.ini') -Value $ini -Encoding Unicode
    }
    Write-Output "[deploy] $role -> $modDir"
}

if ($Target -in @('all', 'guest')) { Deploy-Role 'guest' }
if ($Target -in @('all', 'host')) { Deploy-Role 'host' }
