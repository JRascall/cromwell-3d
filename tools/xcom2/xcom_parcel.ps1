<#
    xcom_parcel.ps1 - dump an XCOM parcel's layout as a placement list.

    Parcels are .umap packages under
    XComGame\Content\XCOM_2\Maps\Parcels\<Biome>\. Every placed prop in one is
    an XComLevelActor (XComDestructibleActor and friends derive from it), and
    BatchExport writes each to .T3D complete with its StaticMesh reference,
    Location, Rotation and DrawScale3D. So a parcel round-trips headlessly:

        .\tools\xcom2\xcom_parcel.ps1 -Parcel md_Advent_Security_03
        .\tools\xcom2\xcom_parcel.ps1 -List
        .\tools\xcom2\xcom_parcel.ps1 -Parcel lg_Museum_01 -Summary

    Output: xcom_extracted/parcels/<Parcel>/placements.csv, one row per prop, in
    the same units the converted meshes use (unreal units / 96). Cross-
    reference mesh names against xcom_extracted/models/index.csv to find the
    geometry.

    Expect ~30s per parcel - the editor loads the whole map and everything it
    references.
#>
[CmdletBinding()]
param(
    [string]$Parcel,
    [switch]$List,
    [switch]$Summary,
    # Sweep every map of the given kinds instead of a single named parcel.
    # Parcels/Plots/PCPs are the tactical building blocks; Strategy and
    # Missions are whole maps. Sliceable like the other sweeps.
    [switch]$All,
    [string[]]$Kind = @('Parcels', 'Plots', 'PCPs'),
    [int]$Slice = 0,
    [int]$Of = 1,
    [string]$Biome,
    [string]$SdkRoot = 'E:\SteamLibrary\steamapps\common\XCOM 2 SDK'
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$mapRoot = Join-Path $SdkRoot 'XComGame\Content\XCOM_2\Maps\Parcels'

if (-not $All -and ($List -or -not $Parcel)) {
    Get-ChildItem $mapRoot -Recurse -File -Filter *.umap |
        Where-Object { -not $Biome -or $_.Directory.Name -like "*$Biome*" } |
        Select-Object @{n='Biome';e={$_.Directory.Name}},
                      @{n='Parcel';e={$_.BaseName}},
                      @{n='MB';e={[math]::Round($_.Length/1MB,2)}} |
        Sort-Object Biome, Parcel | Format-Table -AutoSize
    if (-not $Parcel) { return }
}

# ------------------------------------------------------------- sweep mode
if ($All) {
    $mapRootAll = Join-Path $SdkRoot 'XComGame\Content\XCOM_2\Maps'
    $maps = @()
    foreach ($k in ($Kind -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })) {
        $d = Join-Path $mapRootAll $k
        if (Test-Path -LiteralPath $d) { $maps += Get-ChildItem $d -Recurse -File -Filter *.umap }
    }
    $maps = $maps | Sort-Object BaseName -Unique
    if ($Of -gt 1) {
        $a = @($maps); $maps = @(for ($i = $Slice; $i -lt $a.Count; $i += $Of) { $a[$i] })
    }

    $editorDirAll = Join-Path $SdkRoot 'Binaries\Win64'
    $n = 0; $ok = 0; $empty = 0
    foreach ($m in $maps) {
        $n++
        $od = Join-Path $repo "xcom_extracted\parcels\$($m.BaseName)"
        # placements.csv is the marker: a map with no placed props writes an
        # empty one, so re-runs skip it rather than re-loading the map.
        if (Test-Path -LiteralPath (Join-Path $od 'placements.csv')) { continue }

        Write-Progress -Activity 'xcom_parcel sweep' -Status "$($m.BaseName) ($n/$($maps.Count))" `
            -PercentComplete ([int](100 * $n / $maps.Count))
        $td = Join-Path $od 't3d'
        New-Item -ItemType Directory -Force -Path $td | Out-Null
        $prev = Get-Location
        try {
            Set-Location -LiteralPath $editorDirAll
            & (Join-Path $editorDirAll 'XComGame.com') batchexport $m.BaseName XComLevelActor T3D $td 2>&1 | Out-Null
        } catch { } finally { Set-Location -LiteralPath $prev }

        if (@(Get-ChildItem $td -Filter *.T3D -EA SilentlyContinue).Count -gt 0) {
            & py -3 (Join-Path $PSScriptRoot 'xcom_parcel.py') --t3d $td `
                --out (Join-Path $od 'placements.csv') | Out-Null
            # The raw T3D is bulky and fully represented by the CSV.
            Remove-Item -LiteralPath $td -Recurse -Force -EA SilentlyContinue
            $ok++
        } else {
            Remove-Item -LiteralPath $td -Recurse -Force -EA SilentlyContinue
            New-Item -ItemType File -Path (Join-Path $od 'placements.csv') -Force | Out-Null
            $empty++
        }
    }
    Write-Progress -Activity 'xcom_parcel sweep' -Completed
    Write-Host ("swept {0} maps: {1} with placements, {2} empty" -f $maps.Count, $ok, $empty)
    return
}

$outDir = Join-Path $repo "xcom_extracted\parcels\$Parcel"
$t3dDir = Join-Path $outDir 't3d'
New-Item -ItemType Directory -Force -Path $t3dDir | Out-Null

$editorDir = Join-Path $SdkRoot 'Binaries\Win64'
$prev = Get-Location
try {
    Set-Location -LiteralPath $editorDir
    # XComLevelActor is the common base for placed props, so one pass catches
    # the destructibles, the tile-frac actors and the plain scenery together.
    $log = & (Join-Path $editorDir 'XComGame.com') batchexport $Parcel XComLevelActor T3D $t3dDir 2>&1
}
finally { Set-Location -LiteralPath $prev }

if ($log -match 'Failure') { throw "BatchExport failed for $Parcel`n$($log | Out-String)" }
$n = @(Get-ChildItem $t3dDir -Filter *.T3D -ErrorAction SilentlyContinue).Count
Write-Host "Exported $n actors from $Parcel"
if ($n -eq 0) { throw "No actors exported - is '$Parcel' a real parcel name? Try -List." }

$args2 = @('-3', (Join-Path $PSScriptRoot 'xcom_parcel.py'), '--t3d', $t3dDir)
if ($Summary) { $args2 += '--summary' } else { $args2 += @('--out', (Join-Path $outDir 'placements.csv')) }
& py @args2
