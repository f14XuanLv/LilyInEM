# Differential collision oracle (FR-MOV-4): is the EL mirror blocked exactly where EM blocks Lilac?
#   tools\collision_check.ps1 [-HalfWidth 800] [-Below 400] [-Above 600] [-Step 16]
# Probes a grid around Lilac in EM with Lilac's own capsule profile ("Player") and the same points, mapped through
# the Guest's FrameMap, in EL with Lily's own profile. Both answers come from the engines' scene queries, so any
# cause of a difference (scan, shell, merge, slab, mirror placement, one-way, stale verdict) shows up.
# Cells on an EM solid/free boundary are tolerated (grid quantisation). Exit code = number of mismatches.
# Stand still while it runs: both sides are sampled one after the other.
param(
    [int]$HalfWidth = 800,
    [int]$Below = 400,
    [int]$Above = 600,
    [int]$Step = 16
)
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$ErrorActionPreference = 'Stop'
$eval = Join-Path $PSScriptRoot 'eval.ps1'

function Invoke-Lua([string]$Role, [string]$Code) {
    $r = & $eval -Role $Role -Code $Code -TimeoutMs 30000 | ConvertFrom-Json
    if (-not $r.ok) { throw "$Role eval failed: $($r.error)" }
    return [string]$r.result
}

# Shared probe: rows top -> bottom, one char per cell ('.' free, letter = blocking thing), then the legend.
# %PRE% sets: pawn, plane_x, to_y(y), to_z(z), name(comp)
$probe = @'
local ksl = StaticFindObject('/Script/Engine.Default__KismetSystemLibrary')
%PRE%
local prof = pawn.CapsuleComponent:GetCollisionProfileName()
local idx, legend, rows = {}, {}, {}
local glyphs = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789'
for j = 0, NZ - 1 do
  local row = {}
  for i = 0, NY - 1 do
    local y, z = to_y(Y0 + i * STEP), to_z(ZTOP - j * STEP)
    local hit = {}
    local b = ksl:SphereTraceSingleByProfile(pawn, {X = plane_x, Y = y, Z = z}, {X = plane_x, Y = y, Z = z + 0.1}, 2.0, prof, false, {pawn}, 0, hit, true,
      {R = 1, G = 0, B = 0, A = 1}, {R = 0, G = 1, B = 0, A = 1}, 0)
    local ch = '.'
    if b then
      local c = hit.Component:Get()
      local k = c:IsValid() and name(c) or '?'
      if not idx[k] then legend[#legend + 1] = k; idx[k] = #legend <= #glyphs and glyphs:sub(#legend, #legend) or '?' end
      ch = idx[k]
    end
    row[#row + 1] = ch
  end
  rows[#rows + 1] = table.concat(row)
end
local leg = {}
for _, k in ipairs(legend) do leg[#leg + 1] = idx[k] .. '=' .. k end
return prof:ToString() .. '\n' .. table.concat(rows, '\n') .. '\n' .. table.concat(leg, '\n')
'@

# Guest mapping: EL = G + (EM - H)
$gs = Invoke-Lua guest 'return LIE_GuestStatus()'
if ($gs -notmatch 'map=\(H (-?[\d.]+),(-?[\d.]+) \| G (-?[\d.]+),(-?[\d.]+)\)') { throw "no mapping in guest status: $gs" }
$hy, $hf, $gy, $gf = [double]$Matches[1], [double]$Matches[2], [double]$Matches[3], [double]$Matches[4]

$lil = Invoke-Lua host @'
local p = FindFirstOf('BP_p0000_Lilac_C')
local l = p:K2_GetActorLocation()
return string.format('%.3f %.3f %.3f', l.X, l.Y, l.Z - p.CapsuleComponent.CapsuleHalfHeight)
'@
$lx, $ly, $lf = $lil.Split(' ') | ForEach-Object { [double]$_ }
# samples on the scanner's column centres (absolute multiples of 16): column edges sit at +-8 and would be ambiguous
$y0 = [math]::Floor(($ly - $HalfWidth) / 16) * 16
$ny = [int](2 * $HalfWidth / $Step) + 1
$ztop = $lf + $Above - $Step / 2
$nz = [int](($Above + $Below) / $Step)
$inv = [Globalization.CultureInfo]::InvariantCulture
$grid = "local Y0, ZTOP, STEP, NY, NZ = {0}, {1}, {2}, {3}, {4}" -f $y0.ToString($inv), $ztop.ToString($inv), $Step, $ny, $nz

$emPre = @"
$grid
local pawn = FindFirstOf('BP_p0000_Lilac_C')
local plane_x = pawn:K2_GetActorLocation().X
local function to_y(y) return y end
local function to_z(z) return z end
local function name(c) return c:GetOwner():GetFName():ToString() .. '.' .. c:GetFName():ToString() end
"@
$elPre = @"
$grid
local pawn = FindFirstOf('BP_p0000_Lily_C')
local plane_x = pawn:K2_GetActorLocation().X
local function to_y(y) return $($gy.ToString($inv)) + (y - ($($hy.ToString($inv)))) end
local function to_z(z) return $($gf.ToString($inv)) + (z - ($($hf.ToString($inv)))) end
local function name(c) return c:GetOwner():GetClass():GetFName():ToString() end
"@

function Split-Probe([string]$raw) {
    $lines = $raw -split "`n"
    $rows = $lines[1..$nz]
    $legend = @{}
    foreach ($l in $lines[($nz + 1)..($lines.Count - 1)]) { if ($l -match '^(.)=(.*)$') { $legend[$Matches[1]] = $Matches[2] } }
    return @{ profile = $lines[0]; rows = $rows; legend = $legend }
}
$em = Split-Probe (Invoke-Lua host ($probe.Replace('%PRE%', $emPre)))
$el = Split-Probe (Invoke-Lua guest ($probe.Replace('%PRE%', $elPre)))

$solid = { param($rows, $j, $i) $j -ge 0 -and $j -lt $nz -and $i -ge 0 -and $i -lt $ny -and $rows[$j][$i] -ne '.' }
$missing = 0; $phantom = 0
$blame = @{}
$map = New-Object System.Collections.Generic.List[string]
for ($j = 0; $j -lt $nz; $j++) {
    $sb = New-Object System.Text.StringBuilder
    for ($i = 0; $i -lt $ny; $i++) {
        $t = & $solid $em.rows $j $i
        $m = & $solid $el.rows $j $i
        $edge = ($t -ne (& $solid $em.rows ($j - 1) $i)) -or ($t -ne (& $solid $em.rows ($j + 1) $i)) -or
                ($t -ne (& $solid $em.rows $j ($i - 1))) -or ($t -ne (& $solid $em.rows $j ($i + 1)))
        if ($t -eq $m) { [void]$sb.Append($(if ($t) { '#' } else { '.' })) }
        elseif ($edge) { [void]$sb.Append($(if ($t) { '#' } else { '.' })) }
        elseif ($t) { $missing++; [void]$sb.Append('M'); $k = 'missing: EM ' + $em.legend[[string]$em.rows[$j][$i]]; $blame[$k] = 1 + [int]$blame[$k] }
        else { $phantom++; [void]$sb.Append('P'); $k = 'phantom: EL ' + $el.legend[[string]$el.rows[$j][$i]]; $blame[$k] = 1 + [int]$blame[$k] }
    }
    $map.Add(('{0,6} {1}' -f [int]($ztop - $j * $Step - $lf), $sb.ToString()))
}

Write-Output ("EM profile {0} / EL profile {1}; Lilac Y {2:F0} foot {3:F0}; map H({4:F1},{5:F1}) G({6:F1},{7:F1}); grid {8}x{9} @ {10} UU" -f `
    $em.profile, $el.profile, $ly, $lf, $hy, $hf, $gy, $gf, $ny, $nz, $Step)
Write-Output ("mismatches: {0} (missing {1}, phantom {2})" -f ($missing + $phantom), $missing, $phantom)
if ($missing + $phantom -gt 0) {
    $blame.GetEnumerator() | Sort-Object Value -Descending | ForEach-Object { Write-Output ("  {0,5}  {1}" -f $_.Value, $_.Key) }
    Write-Output "z above Lilac's foot | # solid both, . free both, M EM-only (missing in EL), P EL-only (phantom); column $([int](($ly - $y0) / $Step)) = Lilac"
    $map | ForEach-Object { Write-Output $_ }
}
exit ($missing + $phantom)
