<#
    ba_anim.ps1 - drive the animation half of the Broken Arrow extraction.

    The bundle sweep (ba_extract.ps1) gets meshes, textures, audio and data.
    It does NOT get usable animation, and cannot: AssetStudio only reads the
    legacy curve arrays, which are empty in this game. See README.md for the
    full diagnosis. This script runs the three stages that do work:

        1. AssetRipper, headless, over the bundle    -> a Unity project
        2. BaBake.cs, in Unity                       -> .batrk per rig+clip
        3. BaExport.cs, in Unity                     -> FBX per rig

        .\tools\ba\ba_anim.ps1 -Stage rip
        .\tools\ba\ba_anim.ps1 -Stage bake
        .\tools\ba\ba_anim.ps1 -Stage fbx
        .\tools\ba\ba_anim.ps1                      # all three, in order

    WHY THE FBX STAGE LOOPS. Unity's FBX exporter creates an FbxManager per
    ExportObject call and the native side never gives it back - after roughly
    170 rigs the editor dies inside FbxManager_Create with an access violation.
    Nothing in the managed API exposes that lifetime, so it cannot be fixed
    from BaExport.cs; the only lever is process lifetime. The export skips any
    FBX that already exists, so relaunching resumes exactly where the crash
    left off, and this loop just keeps relaunching until a pass adds nothing.

    That is also the termination condition, and it is deliberately "a pass
    added no files" rather than "we reached 422". A rig that crashes the
    exporter every single time would otherwise spin here forever; instead it
    stops, and the count in the summary says how many are missing.
#>
[CmdletBinding()]
param(
    [string]$GameRoot = "C:\Program Files (x86)\Steam\steamapps\common\broken_arrow",
    [string]$OutDir = "D:\ba_extracted",
    [string]$UnityExe = "E:\Unity\Editor\6000.5.0f1\Editor\Unity.exe",

    [ValidateSet('rip', 'bake', 'fbx', 'glb', 'all')]
    [string]$Stage = 'all',

    # Parallel Blender instances for the glb stage. Conversion is ~95 s a rig
    # and single-threaded, so this is the difference between two hours and
    # eleven.
    [int]$Jobs = 6,

    [string]$BlenderExe = "E:\Blender Foundation\Blender 3.6\blender.exe",

    # Substring match on the rig/prefab path, for working one unit at a time.
    [string]$Filter,

    # Ceiling on relaunches, so a pathological rig cannot loop forever.
    [int]$MaxPasses = 12,

    [int]$Port = 17654
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$project = Join-Path $OutDir '_bake'
$ripped = Join-Path $OutDir '_ripper_units'

function Invoke-Unity([string]$method, [string[]]$extra, [string]$log) {
    $call = @('-batchmode', '-quit', '-nographics',
              '-projectPath', $project, '-executeMethod', $method,
              '-logFile', $log) + $extra
    $p = Start-Process $UnityExe -PassThru -WindowStyle Hidden -ArgumentList $call
    $p.WaitForExit()
    return $p.ExitCode
}

# ---- 1. AssetRipper ------------------------------------------------------
# Its free build looks GUI-only. It is a local web server with an OpenAPI
# document, so --headless plus two form posts is the whole integration.
if ($Stage -in 'rip', 'all') {
    Write-Host "`n=== AssetRipper ===" -ForegroundColor Cyan
    $exe = Join-Path $OutDir '_tools\AssetRipper\AssetRipper.GUI.Free.exe'
    if (-not (Test-Path $exe)) { throw "AssetRipper not found at $exe - run ba_extract.ps1 first" }
    if (Test-Path $ripped) {
        Write-Host "  already ripped to $ripped" -ForegroundColor DarkGray
    }
    else {
        $bundle = Get-ChildItem (Join-Path $GameRoot 'BrokenArrow_Data\StreamingAssets\aa\PC') `
                    -Filter 'units_assets_all*.bundle' | Select-Object -First 1
        if (-not $bundle) { throw "units bundle not found" }
        $srv = Start-Process $exe -PassThru -WindowStyle Hidden -ArgumentList '--headless', '--port', $Port
        try {
            # Give the host a moment to bind before posting at it.
            for ($i = 0; $i -lt 30; $i++) {
                try { Invoke-WebRequest "http://localhost:$Port/" -UseBasicParsing -TimeoutSec 2 | Out-Null; break }
                catch { Start-Sleep -Seconds 1 }
            }
            Write-Host "  loading $($bundle.Name)"
            curl.exe -s -X POST "http://localhost:$Port/LoadFile" --data-urlencode "path=$($bundle.FullName)" | Out-Null
            Write-Host "  exporting project"
            curl.exe -s -X POST "http://localhost:$Port/Export/UnityProject" --data-urlencode "path=$ripped" | Out-Null
        }
        finally { $srv | Stop-Process -Force -ErrorAction SilentlyContinue }
    }
}

# ---- project assembly ----------------------------------------------------
# Only the folders the bake and export actually read. Copying the whole 12 GB
# export means importing 7.2 GB of textures nothing here looks at.
if ($Stage -in 'bake', 'fbx', 'all') {
    if (-not (Test-Path (Join-Path $project 'ProjectSettings\ProjectVersion.txt'))) {
        Write-Host "`n=== creating Unity project ===" -ForegroundColor Cyan
        Start-Process $UnityExe -Wait -WindowStyle Hidden -ArgumentList @(
            '-batchmode', '-quit', '-nographics', '-createProject', $project,
            '-logFile', (Join-Path $OutDir '_bake_create.log'))
        # The FBX exporter is not in a default project.
        $manifest = Join-Path $project 'Packages\manifest.json'
        $m = Get-Content $manifest -Raw | ConvertFrom-Json
        if (-not $m.dependencies.'com.unity.formats.fbx') {
            $m.dependencies | Add-Member -NotePropertyName 'com.unity.formats.fbx' -NotePropertyValue '5.1.1'
            $m | ConvertTo-Json -Depth 10 | Set-Content $manifest -Encoding utf8
        }
    }
    $assets = Join-Path $project 'Assets'
    New-Item -ItemType Directory -Force (Join-Path $assets 'Editor') | Out-Null
    Copy-Item (Join-Path $here 'unity\*.cs') (Join-Path $assets 'Editor') -Force
    # Mesh and Material are only needed by the FBX stage - without them you get
    # skeletons with no geometry, which exports perfectly happily.
    foreach ($d in 'AnimationClip', 'Avatar', 'AnimatorController', 'GameObject', 'Models', 'Resources_moved', 'Mesh', 'Material') {
        $s = Join-Path "$ripped\ExportedProject\Assets" $d
        $t = Join-Path $assets $d
        if ((Test-Path $s) -and -not (Test-Path $t)) {
            Write-Host "  copying $d" -ForegroundColor DarkGray
            Copy-Item -LiteralPath $s -Destination $t -Recurse -Force
        }
    }
}

$extra = @()
if ($Filter) { $extra += @('-baFilter', $Filter) }

# ---- 2. bake -------------------------------------------------------------
if ($Stage -in 'bake', 'all') {
    Write-Host "`n=== bake (muscle curves -> bone transforms) ===" -ForegroundColor Cyan
    $log = Join-Path $OutDir '_bake_full.log'
    $code = Invoke-Unity 'BaBake.Bake' ($extra + @('-baOut', (Join-Path $OutDir 'anim').Replace('\', '/'))) $log
    Select-String -Path $log -Pattern 'BABAKE-DONE' | ForEach-Object { Write-Host "  $($_.Line)" }
    if ($code -ne 0) { Write-Warning "  Unity exited $code - see $log" }
}

# ---- 3. FBX --------------------------------------------------------------
if ($Stage -in 'fbx', 'all') {
    Write-Host "`n=== FBX (relaunching around the exporter's native leak) ===" -ForegroundColor Cyan
    $fbxDir = Join-Path $OutDir 'fbx'
    New-Item -ItemType Directory -Force $fbxDir | Out-Null
    $before = -1
    for ($pass = 1; $pass -le $MaxPasses; $pass++) {
        $count = (Get-ChildItem $fbxDir -Filter '*.fbx' -ErrorAction SilentlyContinue).Count
        if ($count -eq $before) {
            Write-Host "  pass $pass added nothing - stopping at $count" -ForegroundColor Yellow
            break
        }
        $before = $count
        $log = Join-Path $OutDir "_export_pass$pass.log"
        Write-Host "  pass $pass (have $count)"
        Invoke-Unity 'BaExport.Export' ($extra + @('-baOut', $fbxDir.Replace('\', '/'))) $log | Out-Null
        Select-String -Path $log -Pattern 'BAEXPORT-DONE' | ForEach-Object { Write-Host "    $($_.Line)" }
    }
    $final = Get-ChildItem $fbxDir -Filter '*.fbx' -ErrorAction SilentlyContinue
    $gb = [math]::Round(($final | Measure-Object Length -Sum).Sum / 1GB, 2)
    Write-Host "`n$($final.Count) FBX, $gb GB, under $fbxDir" -ForegroundColor Green
}

# ---- 4. glb, for the asset browser --------------------------------------
# The browser reads .obj and .glb and plays animation from any rigged .glb, so
# the FBX have to become glTF to be usable there at all. See ba_glb.py.
if ($Stage -in 'glb', 'all') {
    Write-Host "`n=== glb ===" -ForegroundColor Cyan
    $fbxDir = Join-Path $OutDir 'fbx'
    $glbDir = Join-Path $OutDir 'glb'
    New-Item -ItemType Directory -Force $glbDir | Out-Null
    if (-not (Test-Path $BlenderExe)) { throw "Blender not found at $BlenderExe" }

    $procs = @()
    for ($i = 0; $i -lt $Jobs; $i++) {
        $a = "-b --python `"$(Join-Path $here 'ba_glb.py')`" -- `"$fbxDir`" `"$glbDir`" --shard $i --shards $Jobs"
        $procs += Start-Process $BlenderExe -PassThru -WindowStyle Hidden -ArgumentList $a `
                    -RedirectStandardOutput (Join-Path $OutDir "_glb_$i.log") `
                    -RedirectStandardError (Join-Path $OutDir "_glb_$i.err")
    }
    Write-Host "  $Jobs shards running"
    $procs | ForEach-Object { $_.WaitForExit() }

    # BLENDER CANNOT IMPORT ALL OF THEM. Seven rigs - the transport aircraft and
    # helicopters, plus one infantry variant - die in io_scene_fbx with
    # `KeyError: root` at `mesh.armature_setup[self]`, and every combination of
    # import options fails the same way including use_anim=False, so it is
    # structural rather than a setting. The cause is visible in the converted
    # file's material list: `RU_Pilot_Plane` sits inside `RU_AN72P`, so the
    # airframe carries a nested crew rig and the importer cannot decide which
    # armature a mesh is skinned to.
    #
    # FBX2glTF reads them without complaint, so it picks up the remainder. It is
    # the fallback rather than the default because it has no LOD handling - its
    # output carries every LOD mesh, where the Blender path drops all but the
    # highest.
    $missing = Get-ChildItem $fbxDir -Filter '*.fbx' | Where-Object {
        -not (Test-Path (Join-Path $glbDir ($_.BaseName + '.glb'))) }
    if ($missing) {
        Write-Host "  $($missing.Count) Blender could not import - falling back to FBX2glTF" -ForegroundColor Yellow
        $conv = Join-Path $OutDir '_tools\FBX2glTF.exe'
        if (-not (Test-Path $conv)) {
            curl.exe -sL -o $conv 'https://github.com/facebookincubator/FBX2glTF/releases/download/v0.9.7/FBX2glTF-windows-x64.exe'
        }
        foreach ($m in $missing) {
            & $conv --input $m.FullName --output (Join-Path $glbDir $m.BaseName) --binary | Out-Null
            $ok = Test-Path (Join-Path $glbDir ($m.BaseName + '.glb'))
            Write-Host ("    {0,-28} {1}" -f $m.BaseName, $(if ($ok) { 'ok' } else { 'FAILED' }))
        }
    }

    $g = Get-ChildItem $glbDir -Filter '*.glb' -ErrorAction SilentlyContinue
    $gb = [math]::Round(($g | Measure-Object Length -Sum).Sum / 1GB, 2)
    Write-Host "`n$($g.Count) glb, $gb GB, under $glbDir" -ForegroundColor Green
    Write-Host "Now rebuild the browser catalogue: py -3 tools/asset_browser/asset_browser.py --refresh"
}
