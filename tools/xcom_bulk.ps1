<#
    xcom_bulk.ps1 - extract and convert a whole subtree of the SDK content.

    The curated kit (build_test_kit.ps1) picks a few dozen assets by hand. This
    is the other end: sweep every package under a content folder so there is a
    LIBRARY to build parcels out of, then index it.

        .\tools\xcom_bulk.ps1                      # Environments + Vehicles
        .\tools\xcom_bulk.ps1 -Subtree Environments\Rural
        .\tools\xcom_bulk.ps1 -Pilot 10            # time a sample first
        .\tools\xcom_bulk.ps1 -IndexOnly           # rebuild the CSV only

    Output goes to xcom_extracted/models/<Package>/ - NOT to assets/, because the
    full sweep is far too large to check in. assets/models/ stays the curated,
    committed subset; copy folders across as a parcel needs them.

    RESUMABLE: a package whose output folder already exists is skipped, so an
    interrupted run picks up where it stopped. Delete a folder to redo it.

    The index (xcom_extracted/models/index.csv) is the point of the whole thing -
    every mesh with its tile dimensions and the cover class parsed out of
    XCOM's own naming, so a parcel can be assembled by querying rather than by
    opening 600 folders.
#>
[CmdletBinding()]
param(
    [string[]]$Subtree = @('Environments', 'Vehicles'),
    # Sweep the ENTIRE content tree instead of named subtrees. Audio and
    # config areas are excluded by default: exporting StaticMesh/Texture2D
    # from them yields nothing and still costs an editor launch each.
    [switch]$AllContent,
    [string[]]$Exclude = @('Voices', 'Sound', 'Sounds', 'GameData'),
    [string]$SdkRoot = 'E:\SteamLibrary\steamapps\common\XCOM 2 SDK',
    [int]$TextureSize = 512,
    [int]$Pilot = 0,
    [switch]$IndexOnly,
    [switch]$KeepRaw,
    # Slice N of M: run several copies over disjoint slices to use more cores.
    # Safe because each package owns its output folder and its raw staging dir,
    # so workers never touch the same path.
    [int]$Slice = 0,
    [int]$Of = 1
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$contentRoot = Join-Path $SdkRoot 'XComGame\Content\XCOM_2\Packages'
# Extracted output lives in xcom_extracted/ at the project root, NOT under
# workbench/. workbench/ is scratch that gets wiped, and it is gitignored, so a
# 30 GB asset library buried in there is both fragile and invisible.
$libRoot = Join-Path $repo 'xcom_extracted\models'
# The raw TGA/OBJ dump IS genuine scratch - deleted per package as we go.
# workbench/, not builds/: builds/ is compiler output and gets deleted on a
# clean, and these intermediates are worth more than that.
$rawRoot = Join-Path $repo 'workbench\xcom_raw'
New-Item -ItemType Directory -Force -Path $libRoot | Out-Null

# ---------------------------------------------------------------- the sweep
$packages = @()
if ($AllContent) {
    # Everything under Content, minus the excluded areas. Note Common/ and a
    # few others sit beside XCOM_2\Packages, so this walks from Content itself.
    $all = Get-ChildItem (Join-Path $SdkRoot 'XComGame\Content') -Recurse -File -Filter *.upk
    $skip = @($Exclude | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    $packages = $all | Where-Object {
        $rel = $_.DirectoryName
        -not ($skip | Where-Object { $rel -match "\\$_(\\|$)" })
    }
} else {
    foreach ($s in ($Subtree -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })) {
        $dir = Join-Path $contentRoot $s
        if (-not (Test-Path -LiteralPath $dir)) { throw "No such content subtree: $dir" }
        $packages += Get-ChildItem $dir -Recurse -File -Filter *.upk
    }
}
$packages = $packages | Sort-Object Name -Unique
if ($Pilot -gt 0) { $packages = $packages | Select-Object -First $Pilot }
if ($Of -gt 1) {
    $all = @($packages)
    $packages = @(for ($i = $Slice; $i -lt $all.Count; $i += $Of) { $all[$i] })
}

if (-not $IndexOnly) {
    Write-Host ("Sweeping {0} packages ({1:N1} GB of .upk) -> {2}" -f `
        $packages.Count, (($packages | Measure-Object Length -Sum).Sum / 1GB), $libRoot)

    $started = Get-Date
    $done = 0; $skipped = 0; $failed = @()

    foreach ($p in $packages) {
        $name = $p.BaseName
        $outDir = Join-Path $libRoot $name
        $done++

        if (Test-Path -LiteralPath $outDir) { $skipped++; continue }

        $pct = [int](100 * $done / $packages.Count)
        $elapsed = (Get-Date) - $started
        $rate = if ($done - $skipped -gt 0) { $elapsed.TotalSeconds / ($done - $skipped) } else { 0 }
        $eta = [TimeSpan]::FromSeconds($rate * ($packages.Count - $done))
        Write-Progress -Activity "xcom_bulk" -Status "$name  ($done/$($packages.Count))" `
            -PercentComplete $pct -SecondsRemaining $eta.TotalSeconds

        try {
            & (Join-Path $PSScriptRoot 'xcom_extract.ps1') -Package $name -SdkRoot $SdkRoot |
                Out-Null

            $raw = Join-Path $rawRoot $name
            # A staging folder means an interrupted run never leaves a
            # half-written folder that the resume logic would skip.
            $staging = "$outDir.partial"
            if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }

            $log = & py -3 (Join-Path $PSScriptRoot 'xcom_convert.py') `
                        --raw $raw --out $staging --max-size $TextureSize `
                        --fast-png --quiet 2>&1
            if ($LASTEXITCODE -ne 0) { throw ($log | Out-String) }
            # No shared log file is written: xcom_index.py rebuilds everything
            # from the converted folders, so parallel workers never contend.

            if (Test-Path -LiteralPath $staging) { Rename-Item -LiteralPath $staging -NewName $name }
            else { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }

            if (-not $KeepRaw -and (Test-Path -LiteralPath $raw)) {
                # The raw TGA/OBJ dump is several times the size of the
                # converted output; drop it as we go or the sweep needs
                # hundreds of GB rather than tens.
                Remove-Item -LiteralPath $raw -Recurse -Force
            }
        }
        catch {
            $failed += [pscustomobject]@{ Package = $name; Error = "$_" }
            Write-Host "  FAILED $name : $_" -ForegroundColor Yellow
        }
    }
    Write-Progress -Activity "xcom_bulk" -Completed

    $took = (Get-Date) - $started
    Write-Host ("`nSwept {0} packages in {1:hh\:mm\:ss} ({2} skipped, {3} failed)" -f `
        $packages.Count, $took, $skipped, $failed.Count)
    if ($failed) {
        $failed | Export-Csv (Join-Path $libRoot 'failed.csv') -NoTypeInformation
        Write-Host "  failures listed in xcom_extracted\models\failed.csv"
    }
}

# ---------------------------------------------------------------- the index
# A worker slice indexes nothing; the parent run does it once at the end.
if ($Of -le 1) {
    & py -3 (Join-Path $PSScriptRoot 'xcom_index.py') --library $libRoot
}
