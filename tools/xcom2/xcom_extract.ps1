<#
    xcom_extract.ps1 - pull raw art out of the XCOM 2 SDK, headlessly.

    The SDK ships the full UnrealEd editor (Binaries\Win64\XComGame.exe) and
    with it UE3's BatchExport commandlet, so no GUI session is needed:

        XComGame.com batchexport <Package> <ObjectClass> <Extension> <OutDir>

    Content in the SDK is UNCOOKED, which is why this is the source to pull
    from rather than the shipping game: meshes still carry full vertex data and
    textures still carry their top mip. The cooked game install
    (XComGame\CookedPCConsole) has both stripped.

    What comes out is raw and NOT yet usable - UE3 writes unindexed triangle
    soup with no vertex normals, in unreal units (96uu = one XCOM tile), plus
    fat uncompressed TGAs. Feed it to xcom_convert.py to get engine-ready
    assets.

    Example:
        .\tools\xcom2\xcom_extract.ps1 -Package CinderblockWallA

    Discover packages by name with:
        Get-ChildItem "<SdkRoot>\XComGame\Content" -Recurse -Filter *.upk |
            Where-Object Name -match 'Wall'
#>
[CmdletBinding()]
param(
    # Package name only, no path and no .upk - UE3 resolves it through the
    # Paths= entries in XComGame\Config\DefaultEngine.ini.
    [Parameter(Mandatory = $true)][string]$Package,

    [string]$SdkRoot = 'E:\SteamLibrary\steamapps\common\XCOM 2 SDK',

    # Raw drop zone. Defaults under workbench/ because it is .gitignore'd -
    # these are multi-megabyte intermediates, not assets.
    [string]$OutDir,

    # UE3 object classes to pull. StaticMesh->OBJ and Texture2D->TGA are the
    # two that matter for environment art.
    [switch]$NoMeshes,
    [switch]$NoTextures
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $OutDir) { $OutDir = Join-Path $repoRoot "workbench\xcom_raw\$Package" }

$editorDir = Join-Path $SdkRoot 'Binaries\Win64'
# The .com is the console wrapper around the editor .exe; it writes commandlet
# output to stdout instead of opening a window.
$editor = Join-Path $editorDir 'XComGame.com'
if (-not (Test-Path -LiteralPath $editor)) {
    throw "XCOM 2 SDK editor not found at '$editor'. Pass -SdkRoot to point at your install."
}

$meshDir = Join-Path $OutDir 'mesh'
$texDir  = Join-Path $OutDir 'tex'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function Invoke-BatchExport {
    param([string]$Class, [string]$Extension, [string]$Destination)

    New-Item -ItemType Directory -Force -Path $Destination | Out-Null

    # The commandlet must run with its own Binaries dir as CWD: the engine
    # resolves Config and the ..\..\XComGame\Content Paths= entries relative
    # to it. Restore the caller's location afterwards.
    $prev = Get-Location
    try {
        Set-Location -LiteralPath $editorDir
        # NOTE: BatchExport takes the output directory POSITIONALLY. Anything
        # placed here is treated as that path - passing a stray switch like
        # -nopause makes it create a folder literally named "-nopause" inside
        # the SDK install.
        $output = & $editor batchexport $Package $Class $Extension $Destination 2>&1
    }
    finally {
        Set-Location -LiteralPath $prev
    }

    $output | Where-Object { $_ -match 'Exported|error|warning|Failure' } |
        ForEach-Object { Write-Verbose $_ }

    $exported = @($output | Select-String -Pattern '^Exported ' )
    Write-Host ("  {0,-12} -> {1,3} object(s) in {2}" -f $Class, $exported.Count, $Destination)

    if ($output -match 'Failure') {
        throw "BatchExport failed for $Package/$Class. Re-run with -Verbose for the log."
    }
    return $exported.Count
}

Write-Host "Extracting '$Package' from the XCOM 2 SDK..."
if (-not $NoMeshes)   { [void](Invoke-BatchExport -Class 'StaticMesh' -Extension 'OBJ' -Destination $meshDir) }
if (-not $NoTextures) { [void](Invoke-BatchExport -Class 'Texture2D'  -Extension 'TGA' -Destination $texDir) }

# UnrealEd emits three OBJs per mesh: the plain one (UV channel 0, the one you
# want), *_UV1 (the lightmap unwrap) and *_Internal (editor-internal form).
# Drop the two that are never the right answer so the raw dir stays readable.
if (-not $NoMeshes -and (Test-Path -LiteralPath $meshDir)) {
    $junk = Get-ChildItem -LiteralPath $meshDir -Filter *.OBJ |
        Where-Object { $_.BaseName -match '_(UV1|Internal)$' }
    if ($junk) {
        $junk | Remove-Item -Force
        Write-Host ("  pruned       -> {0} lightmap/internal OBJ variant(s)" -f $junk.Count)
    }
}

Write-Host "Raw art in: $OutDir"
Write-Host "Next: py -3 tools\xcom2\xcom_convert.py --raw `"$OutDir`" --out assets\models\<name>"
