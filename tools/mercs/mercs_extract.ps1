<#
    mercs_extract.ps1 - pull Mercenaries: Playground of Destruction apart for the studies.

    Mercenaries (Pandemic / LucasArts, PS2, 2004) runs on RedEngine. Its whole
    content library is three .DSK archives of ucfb chunks plus two unrelated
    PS2 audio systems, all of which mercs_dsk.py and mercs_snd.py now read.
    This drives them over a disc image and copies through the parts that need
    no decoding at all.

        .\tools\mercs\mercs_extract.ps1 -List              # inventory, extracts nothing
        .\tools\mercs\mercs_extract.ps1 -Kind audio        # the fully solved half
        .\tools\mercs\mercs_extract.ps1                    # everything
        .\tools\mercs\mercs_extract.ps1 -Iso <path\to.iso> # re-rip the disc first

    Output goes to mercs_extracted/ at the project root, gitignored, matching
    where xcom_bulk.ps1, hd2_extract.ps1, r6_extract.ps1 and sc3k_extract.ps1
    put their sweeps.

    WHY THIS IS READ-ONLY RESEARCH. Pandemic's art and audio are not ours to
    redistribute and none of it is committed or shipped - the same rule this
    repo already applies to XCOM, Siege, UNIGINE and SimCity 3000. What is
    worth having is the shape of the data: a 2004 open-world game that ran a
    whole destructible city off a DVD in 32 MB of RAM, and the budgets that
    took - 4643 textures of which half are 32x32, a mesh format built out of
    triangle strips, and terrain and world placement small enough (23 MB
    together) to say something about how much of the map was ever resident.
    See study/games/openworld/mercenaries.md.

    STATE OF PLAY. Audio, names, animation, skeletons and every text table
    come out complete. Texture pixels and mesh geometry do not: both sit
    behind one bespoke LZ compressor that is not yet read, so -Kind textures
    yields dimensions, palettes and names but compressed pixel data. The
    header comment in mercs_dsk.py says what is known about it.

    Resumable at step granularity - a `.done` marker beside each output folder
    means that step is skipped, so an interrupted run costs nothing.
#>
[CmdletBinding()]
param(
    # A ripped disc tree. If it does not exist and -Iso is given, it is made.
    [string]$DiscRoot,
    [string]$Iso,
    [string]$OutDir,

    # archives  - the .DSK contents: chunk inventory + unique payload export.
    # names     - every recoverable asset name, as a .tsv. Cheap, do it first.
    # textures  - tex_ chunks: dimensions, palettes, names. Pixels still packed.
    # models    - modl skeletons and CSEG geometry chunks, as-is.
    # audio     - .MIB/.MIH streams and the .MSB/.MSH banks, decoded to .wav.
    # data      - the plain-text tables: .INI, .CFG, .TXT, .LST. No decoding.
    [ValidateSet('archives', 'names', 'textures', 'models', 'audio', 'data', 'all')]
    [string]$Kind = 'all',

    [switch]$List,
    [switch]$Force,
    # vgmstream-cli.exe. Not vendored - it is a third-party binary and the
    # licence is theirs. Fetched by -FetchTools if absent.
    [string]$VgmStream,
    [switch]$FetchTools
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $OutDir) { $OutDir = Join-Path $root 'mercs_extracted' }
if (-not $DiscRoot) { $DiscRoot = Join-Path $OutDir 'disc' }
$py = 'C:\Python312\python.exe'
if (-not (Test-Path $py)) { $py = 'py' }
$dskPy = Join-Path $PSScriptRoot 'mercs_dsk.py'
$sndPy = Join-Path $PSScriptRoot 'mercs_snd.py'
$toolDir = Join-Path $OutDir '_tools'

# ------------------------------------------------------------- disc ripping
# 7-Zip reads ISO9660 directly, so there is no need to mount anything. The
# image is 4.37 GB but only ~2 GB of files: the rest is DVD padding, and the
# duplication that makes a level load contiguous is INSIDE the archives, not
# on the filesystem.
function Ensure-Disc {
    if (Test-Path (Join-Path $DiscRoot 'DATAPS2')) { return }
    if (-not $Iso) {
        Write-Error "No disc tree at $DiscRoot. Pass -Iso <path to .iso> to rip one."
    }
    $sz = 'C:\Program Files\7-Zip\7z.exe'
    if (-not (Test-Path $sz)) { Write-Error "7-Zip not found at $sz." }
    Write-Host "== ripping $Iso -> $DiscRoot" -ForegroundColor Cyan
    New-Item -ItemType Directory -Force $DiscRoot | Out-Null
    & $sz x $Iso "-o$DiscRoot" -y -bso0 -bsp0
    if ($LASTEXITCODE -ne 0) { Write-Error "7-Zip failed on $Iso" }
}

function Get-VgmStream {
    if ($VgmStream -and (Test-Path $VgmStream)) { return $VgmStream }
    $local = Join-Path $toolDir 'vgmstream\vgmstream-cli.exe'
    if (Test-Path $local) { return $local }
    if (-not $FetchTools) { return $null }
    Write-Host "== fetching vgmstream" -ForegroundColor Cyan
    New-Item -ItemType Directory -Force $toolDir | Out-Null
    $zip = Join-Path $toolDir 'vgmstream-win64.zip'
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -UseBasicParsing -OutFile $zip `
        -Uri 'https://github.com/vgmstream/vgmstream/releases/latest/download/vgmstream-win64.zip'
    Expand-Archive -Path $zip -DestinationPath (Join-Path $toolDir 'vgmstream') -Force
    if (Test-Path $local) { return $local }
    return $null
}

function Step-Done($name) { Join-Path $OutDir "$name.done" }
function Skip-Step($name) {
    if ((Test-Path (Step-Done $name)) -and -not $Force) {
        Write-Host "  $name - already done" -ForegroundColor DarkGray
        return $true
    }
    return $false
}
function Mark-Step($name) { Set-Content -Path (Step-Done $name) -Value (Get-Date -Format 'o') }

Ensure-Disc
$data = Join-Path $DiscRoot 'DATAPS2'
$dsks = @('STREAMED.DSK', 'ASSETS.DSK', 'LOCKED.DSK', 'ENGLISH.DSK') |
    ForEach-Object { Join-Path $data $_ } | Where-Object { Test-Path $_ }

# ---------------------------------------------------------------- inventory
if ($List) {
    Write-Host "Mercenaries disc at $DiscRoot`n" -ForegroundColor Cyan
    & $py $dskPy @dsks --inventory
    $mib = Get-ChildItem (Join-Path $data 'SOUND') -Recurse -Filter *.MIB -ErrorAction SilentlyContinue
    $msh = Get-ChildItem (Join-Path $data 'SOUND') -Recurse -Filter *.MSH -ErrorAction SilentlyContinue
    Write-Host ("`naudio: {0} MIB streams ({1:N1} MB), {2} MSB banks" -f `
            $mib.Count, (($mib | Measure-Object Length -Sum).Sum / 1MB), $msh.Count)
    return
}

New-Item -ItemType Directory -Force $OutDir | Out-Null
$wanted = if ($Kind -eq 'all') { @('names', 'archives', 'textures', 'models', 'audio', 'data') } else { , $Kind }

foreach ($k in $wanted) {
    switch ($k) {

        'names' {
            if (Skip-Step 'names') { break }
            Write-Host "== names" -ForegroundColor Cyan
            & $py $dskPy @dsks --names (Join-Path $OutDir 'asset_names.tsv')
            Mark-Step 'names'
        }

        'archives' {
            if (Skip-Step 'archives') { break }
            Write-Host "== archives (unique payloads, deduped on content)" -ForegroundColor Cyan
            & $py $dskPy @dsks --extract (Join-Path $OutDir 'chunks')
            Mark-Step 'archives'
        }

        'textures' {
            if (Skip-Step 'textures') { break }
            Write-Host "== textures" -ForegroundColor Cyan
            & $py $dskPy @dsks --extract (Join-Path $OutDir 'textures') --only tex_
            Mark-Step 'textures'
        }

        'models' {
            if (Skip-Step 'models') { break }
            Write-Host "== models (modl skeletons + CSEG geometry, still packed)" -ForegroundColor Cyan
            & $py $dskPy @dsks --extract (Join-Path $OutDir 'models') --only modl --only CSEG --only anim
            Mark-Step 'models'
        }

        'data' {
            if (Skip-Step 'data') { break }
            # Already readable; copy through, preserving the tree. RS.INI,
            # VEHICLES.INI, LEVELS.INI and MSH2TEMP.INI are the design tables,
            # and RSM.CFG is the audio manifest that names every stream and
            # gives its sample rate.
            Write-Host "== data (plain-text tables)" -ForegroundColor Cyan
            $dest = Join-Path $OutDir 'data'
            New-Item -ItemType Directory -Force $dest | Out-Null
            $n = 0
            Get-ChildItem $DiscRoot -Recurse -File -Include '*.INI', '*.CFG', '*.TXT', '*.LST' |
                ForEach-Object {
                    $rel = $_.FullName.Substring($DiscRoot.Length).TrimStart('\')
                    $to = Join-Path $dest $rel
                    New-Item -ItemType Directory -Force (Split-Path $to) | Out-Null
                    Copy-Item $_.FullName $to -Force
                    $n++
                }
            Write-Host "  copied $n files" -ForegroundColor DarkGray
            Mark-Step 'data'
        }

        'audio' {
            if (Skip-Step 'audio') { break }
            $vg = Get-VgmStream
            if (-not $vg) {
                Write-Warning "vgmstream not found - pass -VgmStream <path> or -FetchTools. Skipping audio."
                break
            }
            $dest = Join-Path $OutDir 'audio'
            New-Item -ItemType Directory -Force $dest | Out-Null
            $sound = Join-Path $data 'SOUND'

            # 1. .MIB streams. vgmstream reads the MIH+MIB pair natively; the
            #    .MIH beside it carries rate, channels and interleave, so
            #    nothing has to be told to it on the command line.
            Write-Host "== audio: MIB streams" -ForegroundColor Cyan
            $mibs = Get-ChildItem $sound -Recurse -Filter *.MIB
            $i = 0
            foreach ($m in $mibs) {
                $i++
                $rel = $m.FullName.Substring($sound.Length).TrimStart('\')
                $to = Join-Path (Join-Path $dest 'streams') ([IO.Path]::ChangeExtension($rel, '.wav'))
                New-Item -ItemType Directory -Force (Split-Path $to) | Out-Null
                if ((Test-Path $to) -and -not $Force) { continue }
                & $vg -o $to $m.FullName | Out-Null
                if ($i % 25 -eq 0) { Write-Host "  $i/$($mibs.Count)" -ForegroundColor DarkGray }
            }
            Write-Host "  $($mibs.Count) streams -> $dest\streams" -ForegroundColor DarkGray

            # 2. .MSB banks. vgmstream does not know the format, so split them
            #    into .VAG with mercs_snd.py first - a repackage, not a decode -
            #    then let vgmstream do the ADPCM properly.
            Write-Host "== audio: MSB banks" -ForegroundColor Cyan
            $vagRoot = Join-Path $dest '_vag'
            foreach ($h in Get-ChildItem $sound -Recurse -Filter *.MSH) {
                $rel = $h.FullName.Substring($sound.Length).TrimStart('\')
                $sub = Join-Path $vagRoot (Split-Path $rel)
                & $py $sndPy $h.FullName --out $sub
            }
            $vags = Get-ChildItem $vagRoot -Recurse -Filter *.vag -ErrorAction SilentlyContinue
            $i = 0
            foreach ($v in $vags) {
                $i++
                $rel = $v.FullName.Substring($vagRoot.Length).TrimStart('\')
                $to = Join-Path (Join-Path $dest 'banks') ([IO.Path]::ChangeExtension($rel, '.wav'))
                New-Item -ItemType Directory -Force (Split-Path $to) | Out-Null
                if ((Test-Path $to) -and -not $Force) { continue }
                & $vg -o $to $v.FullName | Out-Null
                if ($i % 250 -eq 0) { Write-Host "  $i/$($vags.Count)" -ForegroundColor DarkGray }
            }
            Write-Host "  $($vags.Count) bank samples -> $dest\banks" -ForegroundColor DarkGray
            Mark-Step 'audio'
        }
    }
}

Write-Host "`nwrote $OutDir" -ForegroundColor Green
