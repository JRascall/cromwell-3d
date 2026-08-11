<#
    xcom_materials.ps1 - recover mesh -> texture assignments across the library.

    The bulk sweep cannot pair meshes with textures, because UE3's OBJ exporter
    merges material sections away. The SDK still knows, in two hops:

        pkginfo <pkg> -all      -> StaticMesh export -> its MaterialInstanceConstant
        batchexport <pkg> MaterialInstanceConstant T3D
                                -> that material -> its Diffuse/Normal/Masks

    This runs both per package, then joins them into
    xcom_extracted/models/materials.csv and (with -WriteMtl) real .mtl files.

        .\tools\xcom2\xcom_materials.ps1                    # full sweep + build
        .\tools\xcom2\xcom_materials.ps1 -BuildOnly         # re-join what is scanned
        .\tools\xcom2\xcom_materials.ps1 -Slice 0 -Of 6     # one parallel worker

    Materials frequently live in a DIFFERENT package than the mesh (DirtPileDeco
    paints with TextureLibrary_ClimateZones.WLD_Materials.WLD_MudA), so -Extra
    lets extra packages be pulled in for their materials even though they hold
    no meshes of their own.
#>
[CmdletBinding()]
param(
    [string]$SdkRoot = 'E:\SteamLibrary\steamapps\common\XCOM 2 SDK',
    [switch]$BuildOnly,
    [switch]$WriteMtl,
    [switch]$PatchObj,
    [string[]]$Extra,
    [string[]]$Only,
    [int]$Slice = 0,
    [int]$Of = 1
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$lib = Join-Path $repo 'xcom_extracted\models'
$mats = Join-Path $repo 'xcom_extracted\_materials'
$editorDir = Join-Path $SdkRoot 'Binaries\Win64'
$editor = Join-Path $editorDir 'XComGame.com'

New-Item -ItemType Directory -Force -Path (Join-Path $mats 'deps') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $mats 't3d')  | Out-Null
$tmp = Join-Path $mats 'tmp'
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

if (-not $BuildOnly) {
    $packages = @(Get-ChildItem $lib -Directory | Select-Object -ExpandProperty Name)
    # -Only narrows the library sweep; -Extra is applied AFTER it, because the
    # extras are packages that hold materials but no meshes and so are never in
    # the library listing -- filtering them out would defeat their purpose.
    if ($Only) {
        $keep = $Only -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ }
        $packages = $packages | Where-Object { $keep -contains $_ }
    }
    if ($Extra) { $packages += ($Extra -split ',' | ForEach-Object { $_.Trim() }) }
    $packages = $packages | Sort-Object -Unique
    if ($Of -gt 1) {
        $all = @($packages)
        $packages = @(for ($i = $Slice; $i -lt $all.Count; $i += $Of) { $all[$i] })
    }

    $n = 0
    foreach ($pkg in $packages) {
        $n++
        $depOut = Join-Path $mats "deps\$pkg.tsv"
        $t3dOut = Join-Path $mats "t3d\$pkg"
        if ((Test-Path $depOut) -and (Test-Path $t3dOut)) { continue }

        Write-Progress -Activity 'xcom_materials' -Status "$pkg ($n/$($packages.Count))" `
            -PercentComplete ([int](100 * $n / $packages.Count))

        $prev = Get-Location
        try {
            Set-Location -LiteralPath $editorDir

            if (-not (Test-Path $t3dOut)) {
                New-Item -ItemType Directory -Force -Path $t3dOut | Out-Null
                & $editor batchexport $pkg MaterialInstanceConstant T3D $t3dOut 2>&1 | Out-Null
            }

            # pkginfo costs ~8s (package load, not printing - -depends is no
            # cheaper). It only yields StaticMesh->material rows, so skip it
            # entirely for packages that contributed no meshes. They still get
            # their MICs exported above, because other packages reference them.
            $hasMeshes = @(Get-ChildItem (Join-Path $lib $pkg) -Filter *.obj -EA SilentlyContinue).Count -gt 0
            if (-not $hasMeshes) {
                New-Item -ItemType File -Path $depOut -Force | Out-Null
            }

            if (-not (Test-Path $depOut)) {
                # -all is the only mode that carries DependsMap, and its dumps
                # are large, so write to a scratch file, parse, delete.
                $dump = Join-Path $tmp "$pkg.txt"
                & $editor pkginfo $pkg -all 2>&1 | Out-File -Encoding utf8 $dump
                Set-Location -LiteralPath $prev
                & py -3 (Join-Path $PSScriptRoot 'xcom_materials.py') scan `
                    --pkg $pkg --dump $dump --out $depOut | Out-Null
                Remove-Item -LiteralPath $dump -Force -ErrorAction SilentlyContinue
                if (-not (Test-Path $depOut)) { New-Item -ItemType File -Path $depOut | Out-Null }
            }
        }
        catch { Write-Host "  FAILED $pkg : $_" -ForegroundColor Yellow }
        finally { Set-Location -LiteralPath $prev }
    }
    Write-Progress -Activity 'xcom_materials' -Completed
}

if ($Of -le 1) {
    $a = @('-3', (Join-Path $PSScriptRoot 'xcom_materials.py'), 'build',
           '--library', $lib, '--mats', $mats)
    if ($WriteMtl) { $a += '--write-mtl' }
    if ($PatchObj) { $a += '--patch-obj' }
    & py @a
}
