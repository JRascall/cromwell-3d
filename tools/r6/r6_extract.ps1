<#
.SYNOPSIS
    Sweep Rainbow Six: Siege's .forge archives into loose assets.

.DESCRIPTION
    The driver around the patched RainbowForge DumpTool (see tools/README.md for
    what was patched and why -- stock and all three public forks fail on the
    current build). This script does the parts the tool does not: pick archives,
    route each one's output to its own directory, resume, and slice.

    Archives are classified by filename, which is reliable here because Ubisoft
    packs by type: a *_mesh archive holds meshes and nothing else. That is what
    makes -Kind worth having, and it is the difference between a 1.3 GB mesh pass
    and a 24 GB texture pass.

    Resumable. An archive whose output directory carries a .done marker is
    skipped, so an interrupted sweep costs nothing. Sliceable the same way as
    xcom_bulk.ps1 and hd2_extract.ps1: -Slice i -Of n over n processes, disjoint
    by archive, so workers never contend for an output directory.

.EXAMPLE
    .\tools\r6\r6_extract.ps1 -List
    .\tools\r6\r6_extract.ps1 -Kind mesh
    .\tools\r6\r6_extract.ps1 -Pilot 1 -Kind texture     # time one archive first
    .\tools\r6\r6_extract.ps1 -Slice 0 -Of 6
#>
[CmdletBinding()]
param(
    [string]   $GameRoot = "C:\Program Files (x86)\Steam\steamapps\common\Tom Clancy's Rainbow Six Siege",
    [string]   $Out      = "r6_extracted",
    [string]   $DumpTool = "workbench\r6\RainbowForge3\RainbowForge3-master\DumpTool\bin\Release\net6.0\DumpTool.dll",
    [string]   $Oodle    = "",
    [string[]] $Kind     = @(),
    [int]      $Slice    = 0,
    [int]      $Of       = 1,
    [int]      $Pilot    = 0,
    [switch]   $List,
    [switch]   $Force
)

$ErrorActionPreference = 'Stop'

# Classify by filename. Order matters: "meshshape" must be tested before "mesh",
# and "guitextures" before "textures", or the coarser pattern swallows the finer.
function Get-Kind([string]$name) {
    if ($name -match '_meshshape')    { return 'meshshape' }
    if ($name -match '_mesh')         { return 'mesh' }
    if ($name -match '_guitextures')  { return 'guitexture' }
    if ($name -match '_textures')     { return 'texture' }
    if ($name -match '_soundmedia')   { return 'soundmedia' }
    if ($name -match '_soundbank')    { return 'soundbank' }
    if ($name -match '_gidata')       { return 'gidata' }
    if ($name -match '^datapc64_(pvp|onb|tdm)\d*') { return 'world' }
    return 'other'
}

if (-not (Test-Path $GameRoot)) { throw "Game not found: $GameRoot" }

$archives = Get-ChildItem -Path $GameRoot -Filter '*.forge' | Sort-Object Name
$rows = foreach ($a in $archives) {
    [pscustomobject]@{
        Name = [IO.Path]::GetFileNameWithoutExtension($a.Name)
        Path = $a.FullName
        Kind = Get-Kind $a.Name
        MB   = [math]::Round($a.Length / 1MB, 1)
    }
}

if ($Kind.Count -gt 0) { $rows = $rows | Where-Object { $Kind -contains $_.Kind } }

if ($List) {
    # Both tables are rendered to strings before printing: two Format-Table
    # streams in one pipeline interleave and PowerShell errors out.
    $rows | Sort-Object Kind, @{Expression = 'MB'; Descending = $true} |
        Format-Table Kind, Name, MB -AutoSize | Out-String | Write-Host
    $rows | Group-Object Kind |
        ForEach-Object {
            [pscustomobject]@{
                Kind = $_.Name
                Archives = $_.Count
                TotalGB = [math]::Round((($_.Group | Measure-Object MB -Sum).Sum / 1024), 2)
            }
        } | Sort-Object TotalGB -Descending | Format-Table -AutoSize | Out-String | Write-Host
    return
}

# Disjoint by archive so parallel workers never share an output directory.
# Indexed by position rather than IndexOf: that searches by value, so two archives
# with equal fields would collapse onto the same slice and one would never run.
if ($Of -gt 1) {
    $rows = @(for ($i = 0; $i -lt $rows.Count; $i++) { if (($i % $Of) -eq $Slice) { $rows[$i] } })
}
if ($Pilot -gt 0) { $rows = @($rows | Select-Object -First $Pilot) }

if (-not (Test-Path $DumpTool)) {
    throw "DumpTool not built at $DumpTool. See tools/README.md -- it needs building from the patched source in workbench/r6."
}

# The tool P/Invokes oo2core_8_win64.dll by name and separately insists on
# OODLE2_8_PATH pointing at a real file, so both have to resolve.
if (-not $Oodle) { $Oodle = Join-Path (Split-Path $DumpTool) 'oo2core_8_win64.dll' }
if (-not (Test-Path $Oodle)) { throw "Oodle not found at $Oodle (see tools/README.md)" }
$env:OODLE2_8_PATH = (Resolve-Path $Oodle).Path

$dll = (Resolve-Path $DumpTool).Path
$started = Get-Date
$done = 0
$skipped = 0

foreach ($r in $rows) {
    $dir = Join-Path (Join-Path $Out $r.Kind) $r.Name
    $marker = Join-Path $dir '.done'

    if ((Test-Path $marker) -and -not $Force) {
        $skipped++
        continue
    }

    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    Write-Host ("[{0,3}/{1,3}] {2,-10} {3,-52} {4,8} MB" -f ($done + $skipped + 1), $rows.Count, $r.Kind, $r.Name, $r.MB)

    $t0 = Get-Date
    # dumpall writes to the process working directory, so each archive gets its
    # own; the log goes beside it because a failed asset is reported per-UID and
    # is worth keeping rather than scrolling past.
    $log = Join-Path $dir 'dump.log'
    # Both paths contain spaces ("Program Files (x86)", "Game Development"), and
    # Start-Process joins ArgumentList on spaces without quoting, so quote here.
    $p = Start-Process -FilePath 'dotnet' -ArgumentList @("`"$dll`"", 'dumpall', "`"$($r.Path)`"") `
        -WorkingDirectory (Resolve-Path $dir).Path -NoNewWindow -Wait -PassThru `
        -RedirectStandardError $log -RedirectStandardOutput ([IO.Path]::Combine($dir, 'dump.out'))

    $files = @(Get-ChildItem -Path $dir -File -Recurse |
               Where-Object { $_.Name -notlike 'dump.*' -and $_.Name -ne '.done' }).Count
    $errs = 0
    if (Test-Path $log) { $errs = @(Select-String -Path $log -Pattern 'Error while dumping' -SimpleMatch).Count }

    $secs = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1)
    Write-Host ("            -> {0,7} files, {1} errors, {2}s" -f $files, $errs, $secs)

    if ($p.ExitCode -eq 0) { Set-Content -Path $marker -Value (Get-Date -Format o) -Encoding utf8 }
    $done++
}

$mins = [math]::Round(((Get-Date) - $started).TotalMinutes, 1)
Write-Host ""
Write-Host "$done archives extracted, $skipped already done, $mins min"
Write-Host "Output: $Out"
Write-Host "Index with: py -3 tools\r6\r6_index.py --build --out $Out\index.csv"
