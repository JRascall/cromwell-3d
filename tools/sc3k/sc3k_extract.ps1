<#
    sc3k_extract.ps1 - pull SimCity 3000 Unlimited apart for the studies.

    SC3K's whole content library sits behind one container format and one
    compressor, both of which sc3k_dat.py and sc3k_refpack.py now read. This
    drives them over the install, and copies through the parts that need no
    decoding at all.

        .\tools\sc3k\sc3k_extract.ps1 -List             # inventory, extracts nothing
        .\tools\sc3k\sc3k_extract.ps1 -Kind sprites     # the interesting half
        .\tools\sc3k\sc3k_extract.ps1                   # everything
        .\tools\sc3k\sc3k_extract.ps1 -Kind audio -FFmpeg <path\ffmpeg.exe>

    Output goes to sc3k_extracted/ at the project root, gitignored, matching
    where xcom_bulk.ps1, hd2_extract.ps1 and r6_extract.ps1 put their sweeps.

    WHY THIS IS READ-ONLY RESEARCH. Maxis' art is not ours to redistribute and
    none of it is committed or shipped - the same rule this repo already
    applies to XCOM, Siege and UNIGINE. What is worth having is the *shape* of
    the data: a 1999 isometric city renderer that held tens of thousands of
    sprites in 16-bit colour, and the encoding it used to make that blit fast
    enough. See study/games/strategy/simcity3000.md.

    Resumable at archive granularity - a `.done` marker beside each output
    folder means that archive is skipped, so an interrupted run costs nothing.
#>
[CmdletBinding()]
param(
    [string]$GameRoot = "C:\Program Files\GOG Galaxy\Games\SimCity 3000 Unlimited",
    [string]$OutDir,

    # sprites   - Apps\Res\Sprites, the city itself: buildings, roads, vehicles,
    #             people, landmarks, disasters, effects. The bulk of the value.
    # ui        - Apps\Res\UI and the BA interface, flat 16-bit rasters.
    # buildings - Buildings\*.bld, the Building Architect kits.
    # art       - loose BMP/TGA source art from the BA kit; no decoding needed.
    # data      - the text tables: .SII manifests, .KEY, .plt, .met, .fsc, READMEs.
    # audio     - .XA music/speech and .WAV effects. Needs -FFmpeg for the .XA.
    [ValidateSet('sprites', 'ui', 'buildings', 'art', 'data', 'audio', 'all')]
    [string]$Kind = 'all',

    # Print what is there and stop.
    [switch]$List,
    # Decode only the first N archives of a kind - use it to time a sweep
    # before committing to it.
    [int]$Pilot = 0,
    # Re-decode archives that already have a .done marker.
    [switch]$Force,
    # EA's .XA is ADPCM and needs a decoder; ffmpeg has one. Not vendored.
    [string]$FFmpeg
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $OutDir) { $OutDir = Join-Path $root 'sc3k_extracted' }
$py = Join-Path $PSScriptRoot 'sc3k_dat.py'

if (-not (Test-Path $GameRoot)) {
    Write-Error "No SimCity 3000 install at $GameRoot. Pass -GameRoot."
}
$res = Join-Path $GameRoot 'Apps\Res'

# Each kind is (source directory, file filter, output subdirectory). Keeping
# them in one table is what makes -List and the sweep agree by construction
# rather than by both being edited.
$kinds = [ordered]@{
    sprites   = @{ Src = (Join-Path $res 'Sprites'); Filter = @('*.dat'); Out = 'sprites' }
    ui        = @{ Src = (Join-Path $res 'UI');      Filter = @('*.ixf', '*.dat'); Out = 'ui' }
    buildings = @{ Src = (Join-Path $GameRoot 'Buildings'); Filter = @('*.bld'); Out = 'buildings' }
    art       = @{ Src = (Join-Path $res 'BA');      Filter = @('*.bmp', '*.tga'); Out = 'art' }
    data      = @{ Src = $GameRoot; Filter = @('*.sii', '*.key', '*.plt', '*.met', '*.fsc', '*.txt', '*.ini'); Out = 'data' }
    audio     = @{ Src = (Join-Path $res 'Sound');   Filter = @('*.xa', '*.wav'); Out = 'audio' }
}
$wanted = if ($Kind -eq 'all') { $kinds.Keys } else { , $Kind }

function Get-Files($spec) {
    if (-not (Test-Path $spec.Src)) { return @() }
    Get-ChildItem -Path $spec.Src -Recurse -File -Include $spec.Filter |
        Sort-Object FullName
}

# ---------------------------------------------------------------- inventory
if ($List) {
    Write-Host "SimCity 3000 at $GameRoot`n" -ForegroundColor Cyan
    $rows = foreach ($k in $kinds.Keys) {
        $f = Get-Files $kinds[$k]
        [pscustomobject]@{
            kind  = $k
            files = $f.Count
            MB    = [math]::Round((($f | Measure-Object Length -Sum).Sum) / 1MB, 1)
            from  = $kinds[$k].Src.Replace($GameRoot, '.')
        }
    }
    $rows | Format-Table -AutoSize
    return
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

foreach ($k in $wanted) {
    $spec = $kinds[$k]
    $files = Get-Files $spec
    if ($Pilot -gt 0) { $files = $files | Select-Object -First $Pilot }
    if (-not $files) { Write-Warning "nothing to do for '$k'"; continue }
    $dest = Join-Path $OutDir $spec.Out
    Write-Host "== $k  ($($files.Count) files) -> $dest" -ForegroundColor Cyan
    New-Item -ItemType Directory -Force -Path $dest | Out-Null

    switch ($k) {
        { $_ -in 'sprites', 'ui', 'buildings' } {
            # Decoded, one output folder per archive.
            foreach ($f in $files) {
                $stem = [IO.Path]::GetFileNameWithoutExtension($f.Name)
                $done = Join-Path $dest "$stem.done"
                if ((Test-Path $done) -and -not $Force) {
                    Write-Host "  $($f.Name) - already done" -ForegroundColor DarkGray
                    continue
                }
                & py -3 $py $f.FullName --extract --out $dest
                if ($LASTEXITCODE -ne 0) { Write-Warning "decode failed: $($f.Name)"; continue }
                Set-Content -Path $done -Value $f.LastWriteTimeUtc.ToString('o')
            }
        }

        { $_ -in 'art', 'data' } {
            # Already readable; copy through, preserving the tree because the
            # BA kit's folder names ARE the taxonomy (Traditional/gothic,
            # PROPS/VEHICLES) and are the only labelling the art carries.
            foreach ($f in $files) {
                $rel = $f.FullName.Substring($spec.Src.Length).TrimStart('\')
                $to = Join-Path $dest $rel
                New-Item -ItemType Directory -Force -Path (Split-Path $to) | Out-Null
                Copy-Item $f.FullName $to -Force
            }
            Write-Host "  copied $($files.Count) files" -ForegroundColor DarkGray
        }

        'audio' {
            foreach ($f in $files) {
                $rel = $f.FullName.Substring($spec.Src.Length).TrimStart('\')
                $to = Join-Path $dest $rel
                New-Item -ItemType Directory -Force -Path (Split-Path $to) | Out-Null
                if ($f.Extension -ieq '.wav') {
                    Copy-Item $f.FullName $to -Force
                    continue
                }
                if (-not $FFmpeg) { continue }
                $wav = [IO.Path]::ChangeExtension($to, '.wav')
                if ((Test-Path $wav) -and -not $Force) { continue }
                & $FFmpeg -hide_banner -loglevel error -y -i $f.FullName $wav
                if ($LASTEXITCODE -ne 0) { Write-Warning "ffmpeg failed: $($f.Name)" }
            }
            if (-not $FFmpeg) {
                Write-Warning ".XA left undecoded - pass -FFmpeg <path> to convert them."
            }
        }
    }
}

Write-Host "`nwrote $OutDir" -ForegroundColor Green
