<#
    ba_extract.ps1 - pull Broken Arrow apart for the studies.

    Broken Arrow is Unity 2022.3.62f3 / IL2CPP, and unusually for this
    directory nothing here needed a format reversed. Everything except the
    audio is a solved problem with maintained tools; the job is knowing which
    tool answers which of the game's four storage schemes, and driving them
    over the install without re-doing work.

        .\tools\ba\ba_extract.ps1 -List              # inventory, extracts nothing
        .\tools\ba\ba_extract.ps1 -Kind texture      # Texture2D -> PNG
        .\tools\ba\ba_extract.ps1 -Kind model        # rigged FBX + animation
        .\tools\ba\ba_extract.ps1 -Kind audio        # FMOD banks -> wav
        .\tools\ba\ba_extract.ps1                    # everything but video and granite

    Output goes to ba_extracted/ at the project root, gitignored, matching
    where xcom_bulk.ps1, hd2_extract.ps1, r6_extract.ps1, sc3k_extract.ps1,
    mercs_extract.ps1 and wic_extract.ps1 put their sweeps. Pass -Out to put it
    elsewhere; the full sweep is large enough that a drive with 100 GB spare is
    the honest requirement, and E: does not have it.

    WHY THIS IS READ-ONLY RESEARCH. Steel Balalaika's art and audio are not ours
    to redistribute and none of it is committed or shipped - the same rule this
    repo already applies to XCOM, Siege, UNIGINE, SimCity, Mercenaries and World
    in Conflict. What is worth having here is the one shipped RTS whose scale
    problem is this project's scale problem: a thousand-plus units on a 30 km
    map, with air, artillery and infantry sharing one simulation.

    ---- THE FOUR STORAGE SCHEMES -----------------------------------------

    1. ADDRESSABLE BUNDLES, StreamingAssets/aa/PC/*.bundle, 9.9 GB over 77
       files. Meshes, textures, animators, prefabs, ScriptableObject data.
       Read with AssetStudioModCLI. `units_assets_all` alone is 3.2 GB and
       holds 4,443 meshes, 2,708 textures and 427 Animators - it is most of
       what anybody wants and it is one file, so the per-bundle resume below
       matters more than it looks.

    2. THE PLAYER DATA FILE, BrokenArrow_Data/data.unity3d, 6.9 GB. Scenes and
       everything not addressable. Same tool.

    3. FMOD STUDIO BANKS, StreamingAssets/*.bank, ~3 GB over 31 files, holding
       13,700-odd Vorbis samples. Encrypted, which is why no off-the-shelf tool
       opens them - see ba_bank.py, which explains the cipher and how the key
       was recovered without touching the binary.

    4. GRANITE VIRTUAL TEXTURES, StreamingAssets/*.gts + *.gtp, 27 GB - by
       volume the biggest thing in the install by a wide margin. Graphine's
       tile-streaming format, which Unity bought and shelved. Read with
       Nenkai's GraniteTextureReader. NOT in the default sweep: it is the
       terrain megatexture, it decodes to far more than it stores, and it is
       the least likely of the four to be what you came for. -Kind granite.

    The cutscene bundles - 2.5 GB of the addressables - are MP4 video and are
    excluded from -Kind all for the same reason. -Kind video if you want them.

    ---- RESUME ------------------------------------------------------------

    Resumable at bundle-and-kind granularity: a marker under _done/ means "this
    bundle has been swept for this kind", so an interrupted run restarts for
    free. This is not politeness. A single pass over units_assets_all is tens of
    minutes because AssetStudio loads the whole 3.2 GB before it writes
    anything, and a sweep that has to start from nothing after every
    interruption is a sweep that never finishes.

    Tools are fetched on first use into <out>/_tools rather than vendored -
    same handling as vgmstream in mercs_extract.ps1. Versions are pinned so a
    re-run a year from now behaves the same way.
#>
[CmdletBinding()]
param(
    [string]$GameRoot = "C:\Program Files (x86)\Steam\steamapps\common\broken_arrow",
    [string]$OutDir,

    # texture - Texture2D -> PNG. The bulk of the useful output.
    # mesh    - Mesh -> OBJ. Complete geometry coverage, no rig.
    # model   - Animator -> FBX, with skeleton and bound AnimationClips. This
    #           is the one to use for anything that moves.
    # data    - TextAsset and MonoBehaviour -> JSON. Unit stats, weapon tables,
    #           the ScriptableObject layer. Text, and the most quotable part.
    # audio   - FMOD banks -> FSB -> wav, plus any Unity AudioClips.
    # video   - the MP4 cutscene bundles. 2.5 GB, excluded from 'all'.
    # granite - .gts/.gtp virtual texture tiles -> PNG. 27 GB in, more out.
    #           Excluded from 'all'.
    [ValidateSet('texture', 'mesh', 'model', 'data', 'audio', 'video', 'granite', 'all')]
    [string]$Kind = 'all',

    # Substring match on the bundle file name, for working one subsystem at a
    # time - "-Filter units" is the common case and it is the difference
    # between a 20-minute run and an overnight one.
    [string]$Filter,

    [switch]$List,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent (Split-Path -Parent $here)
if (-not $OutDir) { $OutDir = Join-Path $root 'ba_extracted' }

if (-not (Test-Path $GameRoot)) { throw "Game root not found: $GameRoot" }
$dataDir = Join-Path $GameRoot 'BrokenArrow_Data'
$streaming = Join-Path $dataDir 'StreamingAssets'
$bundleDir = Join-Path $streaming 'aa\PC'
if (-not (Test-Path $bundleDir)) { throw "No addressable bundles at $bundleDir" }

$py = Get-Command py -ErrorAction SilentlyContinue
if (-not $py) { throw "Python launcher 'py' not found on PATH." }

# ---- tools ---------------------------------------------------------------
# Pinned. AssetStudioMod is the fork that still gets Unity-version work;
# upstream AssetStudio has been dormant since 2023 and does not read 2022.3
# bundles. AssetRipper is the other candidate and is better at reconstructing a
# whole Unity project (materials, prefabs, shaders) - but its free build ships
# only a GUI, so it cannot be driven from a script, and this needs to be.
$toolDir = Join-Path $OutDir '_tools'
$tools = @{
    'AssetStudioModCLI' = @{
        Url  = 'https://github.com/aelurum/AssetStudioMod/releases/download/v0.19.0/AssetStudioModCLI_net9_win64.zip'
        Exe  = 'AssetStudioModCLI\AssetStudioModCLI_net9_win64\AssetStudioModCLI.exe'
        Zip  = $true
    }
    'vgmstream'         = @{
        Url  = 'https://github.com/vgmstream/vgmstream/releases/download/r2117/vgmstream-win64.zip'
        Exe  = 'vgmstream\vgmstream-cli.exe'
        Zip  = $true
    }
    'GraniteTextureReader' = @{
        Url  = 'https://github.com/Nenkai/GraniteTextureReader/releases/download/1.1.5/GraniteTextureReader.exe'
        Exe  = 'GraniteTextureReader.exe'
        Zip  = $false
    }
}

function Get-Tool([string]$name) {
    $spec = $tools[$name]
    $exe = Join-Path $toolDir $spec.Exe
    if (Test-Path $exe) { return $exe }

    New-Item -ItemType Directory -Force $toolDir | Out-Null
    if ($spec.Zip) {
        $zip = Join-Path $toolDir "$name.zip"
        Write-Host "  fetching $name" -ForegroundColor DarkGray
        curl.exe -sL -o $zip $spec.Url
        Expand-Archive $zip -DestinationPath (Join-Path $toolDir $name) -Force
    }
    else {
        Write-Host "  fetching $name" -ForegroundColor DarkGray
        curl.exe -sL -o $exe $spec.Url
    }
    if (-not (Test-Path $exe)) { throw "Failed to fetch $name from $($spec.Url)" }
    return $exe
}

# ---- sources -------------------------------------------------------------
# data.unity3d comes last on purpose. It is the biggest single load and the
# least interesting - the addressables hold the units - so an interrupted
# overnight run has done the valuable part first.
$isVideo = { param($n) $n -match 'cutscene' }
$bundles = Get-ChildItem $bundleDir -Filter '*.bundle' -File | Sort-Object Length
$player = Get-Item (Join-Path $dataDir 'data.unity3d') -ErrorAction SilentlyContinue

if ($Kind -eq 'video') {
    $sources = @($bundles | Where-Object { & $isVideo $_.Name })
}
else {
    $sources = @($bundles | Where-Object { -not (& $isVideo $_.Name) })
    if ($player) { $sources += $player }
}
if ($Filter) { $sources = @($sources | Where-Object { $_.Name -like "*$Filter*" }) }

# ---- inventory only ------------------------------------------------------
if ($List) {
    $cli = Get-Tool 'AssetStudioModCLI'
    $listDir = Join-Path $OutDir '_lists'
    New-Item -ItemType Directory -Force $listDir | Out-Null
    foreach ($s in $sources) {
        $out = Join-Path $listDir ($s.BaseName + '.txt')
        if ((Test-Path $out) -and -not $Force) { continue }
        Write-Host $s.Name -ForegroundColor Cyan
        & $cli $s.FullName -m info --log-level warning | Tee-Object -FilePath $out | Out-Null
        Get-Content $out | Where-Object { $_ -match '^#' }
    }
    Write-Host "`nBank inventory:" -ForegroundColor Cyan
    & py -3 (Join-Path $here 'ba_bank.py') $streaming --list
    Write-Host "`nListings written to $listDir"
    return
}

# ---- granite -------------------------------------------------------------
# Its own path entirely: not Unity assets, not driven by AssetStudio, and the
# only kind here where the output is much larger than the input.
if ($Kind -eq 'granite') {
    $gtr = Get-Tool 'GraniteTextureReader'
    $gOut = Join-Path $OutDir 'granite'
    foreach ($gts in Get-ChildItem $streaming -Filter '*.gts' -File) {
        $dst = Join-Path $gOut $gts.BaseName
        if ((Test-Path $dst) -and -not $Force) {
            Write-Host "  $($gts.Name) already done" -ForegroundColor DarkGray
            continue
        }
        Write-Host $gts.Name -ForegroundColor Cyan
        New-Item -ItemType Directory -Force $dst | Out-Null
        # -1 is every layer: albedo, normal, and the two RGB mask maps. Taking
        # only albedo halves the time and loses the half that says how the
        # material system is actually authored.
        & $gtr extract-all -t $gts.FullName -l -1 -o $dst
    }
    Write-Host "`nDone. Output under $gOut" -ForegroundColor Green
    return
}

# ---- audio ---------------------------------------------------------------
# The banks are not Unity assets and do not go through AssetStudio at all.
# Unity AudioClips are swept separately below as part of the bundle pass -
# there are few of them, because the game routes essentially everything
# through FMOD.
if ($Kind -eq 'audio' -or $Kind -eq 'all') {
    Write-Host "`n=== FMOD banks ===" -ForegroundColor Cyan
    $vgm = Get-Tool 'vgmstream'
    $audioOut = Join-Path $OutDir 'audio'
    New-Item -ItemType Directory -Force $audioOut | Out-Null
    $bankArgs = @($streaming, '--out', $audioOut, '--wav', '--vgmstream', $vgm)
    if ($Force) { $bankArgs += '--force' }
    # -u, unbuffered. Python buffers stdout when it is redirected to a file,
    # which for a job this long means the log sits on the stage header for an
    # hour while the work is actually happening. A progress log that only
    # updates when a 4 KB buffer fills is worse than no log: it reads as hung.
    & py -3 -u (Join-Path $here 'ba_bank.py') @bankArgs
    if ($Kind -eq 'audio') {
        # Still fall through to the bundle pass for Unity AudioClips.
        $kinds = @('audio')
    }
}

# ---- Unity assets --------------------------------------------------------
# One AssetStudio invocation per (source, kind). Splitting by kind rather than
# asking for everything at once costs a reload of the bundle per kind, which is
# the expensive part - but it is what makes the resume markers meaningful, and
# it means "just the textures" is a thing you can ask for without waiting for
# 4,000 OBJ files you did not want.
$cliArgs = @{
    texture = @('-m', 'export', '-t', 'tex2d', '--image-format', 'png')
    mesh    = @('-m', 'export', '-t', 'mesh')
    # Animator mode walks the Animator, its skeleton and the AnimationClips
    # bound to it, and writes one FBX per animator. 'auto' binds the clips
    # AssetStudio can associate with the model; 'all' would bind every clip in
    # the bundle to every model, which for a 427-animator bundle is a
    # combinatorial mess rather than a result.
    model   = @('-m', 'animator', '--fbx-animation', 'auto')
    data    = @('-m', 'export', '-t', 'textAsset,monoBehaviour')
    audio   = @('-m', 'export', '-t', 'audio')
    video   = @('-m', 'export', '-t', 'video')
}
if (-not $kinds) {
    $kinds = if ($Kind -eq 'all') { @('texture', 'mesh', 'model', 'data', 'audio') } else { @($Kind) }
}

$cli = Get-Tool 'AssetStudioModCLI'
$markers = Join-Path $OutDir '_done'
New-Item -ItemType Directory -Force $markers | Out-Null

foreach ($k in $kinds) {
    Write-Host "`n=== $k ===" -ForegroundColor Cyan
    $kOut = Join-Path $OutDir $k
    foreach ($s in $sources) {
        $mark = Join-Path $markers ("{0}.{1}.done" -f $s.BaseName, $k)
        if ((Test-Path $mark) -and -not $Force) {
            Write-Host "  $($s.Name) already swept" -ForegroundColor DarkGray
            continue
        }
        Write-Host "  $($s.Name)  $([math]::Round($s.Length / 1MB, 1)) MB"
        # Not $args: that is an automatic variable, and a script that assigns to
        # it works right up until somebody moves this loop into a function.
        $call = @($s.FullName) + $cliArgs[$k] + @(
            '-o', $kOut,
            # containerFull keeps the game's own addressable paths, so the
            # output tree reads like the project rather than like a heap.
            '-g', 'containerFull',
            # Names alone collide constantly - every unit has a mesh called
            # "body" - and a collision here silently drops an asset. The
            # PathID suffix is ugly and it is the reason the count adds up.
            '-f', 'assetName_pathID',
            '--log-level', 'warning'
        )
        if ($Force) { $call += '-r' }
        & $cli @call
        if ($LASTEXITCODE -ne 0) {
            Write-Warning "  $($s.Name): AssetStudio exited $LASTEXITCODE - not marking done"
            continue
        }
        New-Item -ItemType File -Force $mark | Out-Null
    }
}

Write-Host "`nDone. Output under $OutDir" -ForegroundColor Green
