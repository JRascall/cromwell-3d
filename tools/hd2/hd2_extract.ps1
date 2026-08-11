<#
    hd2_extract.ps1 - rebuild Helldivers 2's loose bundle tree, then index it.

    Helldivers 2 now ships its assets packed into 30 bundles.NN.nxa archives.
    Every existing extractor expects thousands of loose <hash16> files in data/
    and finds ~10 here, so nothing works out of the box. This unpacks the
    archives back into that layout and indexes what came out.

        .\tools\hd2\hd2_extract.ps1 -Pilot 50           # time a sample first
        .\tools\hd2\hd2_extract.ps1 -Kind bundle        # 5.4 GB - start here
        .\tools\hd2\hd2_extract.ps1                     # everything - 127 GB
        .\tools\hd2\hd2_extract.ps1 -IndexOnly

    MIND THE DISK. The archives are 28 GB because chunks are deduplicated
    across bundles; unpacked they expand to 127 GB. By kind:

        bundle           3254 files     5.4 GB   metadata, materials, units
        gpu_resources    2911 files    34.7 GB   vertex/index/texture payloads
        stream           2650 files    86.7 GB   streamed audio and top mips

    -Kind bundle is enough to build index.csv and see what the game contains;
    pull the payloads only for the bundles you actually want.

    Output goes to hd2_extracted/data/ at the project root, matching where
    xcom_bulk.ps1 puts its sweep - NOT under workbench/, which a clean wipes.

    RESUMABLE: a file already present at its expected size is skipped, so an
    interrupted run picks up where it stopped.

    PARALLELISM. One run uses one core and the work is LZ4 decompression, so it
    scales well. Split it with -Slice i -Of n and launch n copies; slices are
    disjoint by bundle and each bundle owns its output files, so workers never
    contend. Only the unsliced run writes the index, so index once at the end
    with -IndexOnly.

    The unpacked bundles are ordinary Stingray bundles. Converting them to
    meshes and textures is filediver's job (https://github.com/xypwn/filediver);
    point it at hd2_extracted/data as if it were the game's data directory.
#>
[CmdletBinding()]
param(
    [string]$GameRoot = 'E:\SteamLibrary\steamapps\common\Helldivers 2',
    [string]$OutDir,
    # bundle = just the bundle files (5.4 GB, enough to build index.csv);
    # all = bundles plus .gpu_resources and .stream payloads (127 GB).
    [ValidateSet('all', 'bundle', 'stream', 'gpu_resources')]
    [string]$Kind = 'all',
    [string]$Filter = '',
    [int]$Pilot = 0,
    [switch]$IndexOnly,
    [switch]$List,
    [switch]$WrapDsar,
    [int]$Slice = 0,
    [int]$Of = 1
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$dataDir = Join-Path $GameRoot 'data'
if (-not $OutDir) { $OutDir = Join-Path $repo 'hd2_extracted\data' }

if (-not (Test-Path -LiteralPath (Join-Path $dataDir 'bundles.nxa'))) {
    throw "No bundles.nxa under $dataDir - is -GameRoot right?"
}

# The lz4 package is what makes this take minutes instead of days. The Python
# side falls back to a pure decoder, but warn early rather than after an hour.
& py -3 -c "import lz4.block" 2>$null
if ($LASTEXITCODE -ne 0) {
    Write-Host "Installing the lz4 package (needed for usable speed)..." -ForegroundColor Cyan
    & py -3 -m pip install --quiet lz4
    if ($LASTEXITCODE -ne 0) { Write-Host "  pip install lz4 failed; falling back to the slow decoder." -ForegroundColor Yellow }
}

if ($List) {
    & py -3 (Join-Path $PSScriptRoot 'hd2_unpack.py') --data $dataDir --list
    return
}

if (-not $IndexOnly) {
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

    $unpackArgs = @('--data', $dataDir, '--out', $OutDir, '--kind', $Kind)
    if ($Filter) { $unpackArgs += @('--filter', $Filter) }
    if ($Pilot -gt 0) { $unpackArgs += @('--limit', $Pilot) }
    if ($Of -gt 1) { $unpackArgs += @('--slice', $Slice, '--of', $Of) }
    if ($WrapDsar) { $unpackArgs += '--wrap-dsar' }

    $started = Get-Date
    & py -3 (Join-Path $PSScriptRoot 'hd2_unpack.py') @unpackArgs
    if ($LASTEXITCODE -ne 0) { Write-Host "  some files failed; see the log above" -ForegroundColor Yellow }

    $took = (Get-Date) - $started
    Write-Host ("Unpacked in {0:hh\:mm\:ss} -> {1}" -f $took, $OutDir)
}

# A worker slice indexes nothing; the parent run does it once at the end.
if ($Of -le 1) {
    & py -3 (Join-Path $PSScriptRoot 'hd2_index.py') --library $OutDir
}
