<#
    unigine_extract.ps1 - decode UNIGINE 2's core textures for the studies.

    UNIGINE ships its whole core art library unpacked, as `.texture` files -
    its own container, which no third-party tool reads. unigine_texture.py
    decodes it. This drives that over the sets the studies actually need.

        .\tools\unigine_extract.ps1 -Info          # header table, decodes nothing
        .\tools\unigine_extract.ps1                # clouds + water -> PNG
        .\tools\unigine_extract.ps1 -Set clouds -Slices

    Output goes to unigine_extracted/ at the project root, gitignored, matching
    where xcom_bulk.ps1 and hd2_extract.ps1 put their sweeps.

    WHY THIS IS READ-ONLY RESEARCH. UNIGINE's art is licensed for use in
    UNIGINE projects; it is not ours to redistribute and none of it is
    committed or shipped. The point is to read what each channel holds - which
    the shaders in the same SDK document precisely - and then generate our own
    equivalents. See study/unigine_clouds.md.

    The SDK path is wherever the SDK Browser installed it; the default below is
    a Community 2.17 install. `data/core/textures/` is the art root.
#>
[CmdletBinding()]
param(
    [string]$SdkRoot = "$env:LOCALAPPDATA\unigine\browser\sdks\community_windows_2.17.0.1_bin",
    [string]$OutDir,
    # clouds = volumetric cloud noise, coverage, shapes; water = ocean detail,
    # foam, caustics; all = both.
    [ValidateSet('clouds', 'water', 'all')]
    [string]$Set = 'all',
    # Print the header table (dimensions, format, mip count) and stop.
    [switch]$Info,
    # Also write every z slice of a volume texture as its own PNG. A 256^3
    # volume is 256 files; the contact sheet is usually enough.
    [switch]$Slices
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $root 'unigine_extracted' }

$textures = Join-Path $SdkRoot 'data\core\textures'
if (-not (Test-Path $textures)) {
    Write-Error "No SDK at $SdkRoot (looked for data\core\textures). Pass -SdkRoot."
}

$sets = @{
    clouds = 'clouds'
    water  = 'water_global'
}
$wanted = if ($Set -eq 'all') { 'clouds', 'water' } else { , $Set }

$script = Join-Path $PSScriptRoot 'unigine_texture.py'
foreach ($name in $wanted) {
    $src = Join-Path $textures $sets[$name]
    if (-not (Test-Path $src)) {
        Write-Warning "skipping '$name': $src not found in this SDK"
        continue
    }
    Write-Host "== $name  ($src)" -ForegroundColor Cyan

    $pyArgs = @($script, $src)
    if ($Info) {
        $pyArgs += '--info'
    } else {
        $pyArgs += @('-o', (Join-Path $OutDir $name))
        if ($Slices) { $pyArgs += '--slices' }
    }

    & py -3 @pyArgs
    if ($LASTEXITCODE -ne 0) { Write-Error "decode failed for '$name'" }
}

if (-not $Info) { Write-Host "`nwrote $OutDir" -ForegroundColor Green }
