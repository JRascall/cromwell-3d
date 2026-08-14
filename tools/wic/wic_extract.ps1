<#
    wic_extract.ps1 - pull World in Conflict apart for the studies.

    WiC's whole content library sits behind one container format, `RYS` .sdf,
    which wic_sdf.py reads. This drives it over the install, decompiles the
    Python gameplay layer, and reconstructs the headerless textures.

        .\tools\wic\wic_extract.ps1 -List               # inventory, extracts nothing
        .\tools\wic\wic_extract.ps1 -Kind data          # the text formats - the interesting half
        .\tools\wic\wic_extract.ps1 -Kind script        # .pyo -> readable Python
        .\tools\wic\wic_extract.ps1                     # everything except audio
        .\tools\wic\wic_extract.ps1 -Kind audio

    Output goes to wic_extracted/ at the project root, gitignored, matching
    where xcom_bulk.ps1, hd2_extract.ps1, r6_extract.ps1, sc3k_extract.ps1 and
    mercs_extract.ps1 put their sweeps.

    WHY THIS IS READ-ONLY RESEARCH. Massive's art and audio are not ours to
    redistribute and none of it is committed or shipped - the same rule this
    repo already applies to XCOM, Siege, UNIGINE, SimCity and Mercenaries. What
    is worth having here is unusually specific: WiC ships its *shaders as
    commented plain text* and its 2,187 particle effects as key-value files, so
    the thing being read is the engine's own description of itself. See
    study/games/strategy/world_in_conflict/.

    Resumable at file granularity - an output that already exists is skipped,
    so an interrupted run costs nothing. The Python decompile is the slow step
    (~4 minutes for 375 files) and is separately resumable.

    NOTE ON TEXTURES. The packer strips the 128-byte DDS header and records the
    format nowhere, so -Kind texture runs a search (wic_tex.py): candidates are
    enumerated by payload size, then decoded and scored, because size alone is
    ambiguous. Results carry a confidence margin and low-margin files are
    listed rather than silently trusted.
#>
[CmdletBinding()]
param(
    [string]$GameRoot = "E:\World in Conflict",
    [string]$OutDir,

    # data     - the text formats: .sur surfaces (inline HLSL), .pe particle
    #            effects, .txt/.dat/.raw config, .slot attachment points.
    #            Small, and the whole reason this game is worth reading.
    # script   - .pyo Python 2.3 bytecode -> readable source (needs uncompyle6).
    # entity   - .ice object definitions, .gety LightWave wreck geometry.
    # model    - .mrb render meshes, .sdw shadow meshes, .mot/.mmb animation.
    # texture  - .dds/.tga, header reconstructed, written as PNG beside the raw.
    # audio    - .mp3/.wav. 12,922 files and most of the install's bulk.
    [ValidateSet('data', 'script', 'entity', 'model', 'texture', 'audio', 'all')]
    [string]$Kind = 'all',

    [switch]$List,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent (Split-Path -Parent $here)
if (-not $OutDir) { $OutDir = Join-Path $root 'wic_extracted' }

$py = (Get-Command py -ErrorAction SilentlyContinue)
if (-not $py) { throw "Python launcher 'py' not found on PATH." }

if (-not (Test-Path $GameRoot)) { throw "Game root not found: $GameRoot" }
# ORDER MATTERS AND IT IS NOT ALPHABETICAL. wic1..wic5 are the shipped content;
# wic20, wic25, wic30 ... wic65 are patches that RE-ISSUE files from the earlier
# archives, so extraction is last-writer-wins and has to run in release order.
# Sorted by name, wic20 lands before wic3 and a 2007 file overwrites its 2009
# replacement - silently, because both are valid and only the content differs.
$archives = Get-ChildItem -Path $GameRoot -Filter '*.sdf' |
    Sort-Object @{ Expression = { if ($_.BaseName -match '^wicloc') { 1 } else { 0 } } },
                @{ Expression = { [int]($_.BaseName -replace '\D', '') } }
if (-not $archives) { throw "No .sdf archives under $GameRoot" }
Write-Verbose ("order: " + ($archives.BaseName -join ' '))
Write-Host ("Found {0} archives in {1}" -f $archives.Count, $GameRoot)

# ---- inventory only ------------------------------------------------------
if ($List) {
    $listDir = Join-Path $OutDir '_lists'
    New-Item -ItemType Directory -Force $listDir | Out-Null
    foreach ($a in $archives) {
        $out = Join-Path $listDir ($a.BaseName + '.txt')
        & py -3 (Join-Path $here 'wic_sdf.py') $a.FullName "--list=$out"
    }
    Write-Host "Listings written to $listDir"
    return
}

# ---- glob sets per kind --------------------------------------------------
$globs = @{
    data    = @('*.sur', '*.pe', '*.txt', '*.dat', '*.raw', '*.slot', '*.nnn', '*.sli')
    script  = @('*.pyo')
    entity  = @('*.ice', '*.gety')
    model   = @('*.mrb', '*.sdw', '*.mot', '*.mmb', '*.sur')
    texture = @('*.dds', '*.tga')
    audio   = @('*.mp3', '*.wav', '*.bik')
}
$kinds = if ($Kind -eq 'all') { @('data', 'script', 'entity', 'model', 'texture') } else { @($Kind) }

$raw = Join-Path $OutDir 'raw'
New-Item -ItemType Directory -Force $raw | Out-Null

# Resumable at archive granularity, not per file: a marker means "this archive
# has been applied for this kind". Per-file skipping cannot be used because the
# later archives deliberately overwrite the earlier ones (see the sort above).
$markers = Join-Path $OutDir '_done'
New-Item -ItemType Directory -Force $markers | Out-Null

foreach ($k in $kinds) {
    Write-Host "`n=== $k ===" -ForegroundColor Cyan
    foreach ($a in $archives) {
        $mark = Join-Path $markers ("{0}.{1}.done" -f $a.BaseName, $k)
        if ((Test-Path $mark) -and -not $Force) {
            Write-Host ("  {0} already applied" -f $a.BaseName) -ForegroundColor DarkGray
            continue
        }
        foreach ($g in $globs[$k]) {
            & py -3 (Join-Path $here 'wic_sdf.py') $a.FullName "--glob=$g" "--out=$raw"
        }
        New-Item -ItemType File -Force $mark | Out-Null
    }
}

# ---- Python bytecode -> source ------------------------------------------
if ($kinds -contains 'script') {
    Write-Host "`n=== decompiling Python 2.3 bytecode ===" -ForegroundColor Cyan
    & py -3 (Join-Path $here 'wic_pyo.py') $raw (Join-Path $OutDir 'script')
}

# ---- textures -> PNG -----------------------------------------------------
if ($kinds -contains 'texture') {
    Write-Host "`n=== reconstructing texture headers ===" -ForegroundColor Cyan
    & py -3 -W ignore (Join-Path $here 'wic_tex.py') $raw (Join-Path $OutDir 'texture')
}

# ---- meshes -> OBJ -------------------------------------------------------
# Partial by design: see wic_mrb.py. Prop/building meshes come out; unit and
# character meshes use a packed vertex format that is not cracked yet, and are
# reported as "no-mesh" rather than being guessed at.
if ($kinds -contains 'model') {
    Write-Host "`n=== converting meshes to OBJ ===" -ForegroundColor Cyan
    & py -3 -W ignore (Join-Path $here 'wic_mrb.py') $raw (Join-Path $OutDir 'model')
}

Write-Host "`nDone. Output under $OutDir" -ForegroundColor Green
