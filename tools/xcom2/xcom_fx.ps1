<#
    xcom_fx.ps1 - sweep particle system definitions out of the SDK.

        .\tools\xcom2\xcom_fx.ps1                    # every swept package
        .\tools\xcom2\xcom_fx.ps1 -Slice 0 -Of 8     # one parallel worker
        .\tools\xcom2\xcom_fx.ps1 -BuildOnly         # re-join existing scans

    `batchexport <pkg> ParticleSystem T3D` writes the complete definition of
    each system - every emitter, its LOD levels, and every module with its
    baked distribution values. xcom_fx.py turns those into two CSVs.

    The TEXTURES those systems draw with are already extracted: the model
    sweep pulled 1,939 of them from 476 FX_* packages, so this adds the
    recipe, not the art.

    Output:
        xcom_extracted/fx_systems.csv           package, system, emitters, modules
        xcom_extracted/fx_systems_emitters.csv  per emitter: type, material,
                                                spawn rate, lifetime, start
                                                size/velocity, module list
#>
[CmdletBinding()]
param(
    [string]$SdkRoot = 'E:\SteamLibrary\steamapps\common\XCOM 2 SDK',
    [switch]$BuildOnly,
    # A newline-separated list of package names to visit. Loading a package in
    # the editor costs ~80s, so visiting all 1759 wastes hours on packages that
    # hold no particles at all. UE3 keeps its name table at the START of a
    # .upk, so grepping the first few MB of each file for "ParticleSystem"
    # narrows 1759 -> 911 in about 20 seconds. See the README.
    [string]$PackageList,
    [int]$Slice = 0,
    [int]$Of = 1
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$lib = Join-Path $repo 'xcom_extracted\models'
$scans = Join-Path $repo 'xcom_extracted\_fx_scans'
$tmp = Join-Path $repo 'workbench\fx_t3d'
$editor = Join-Path $SdkRoot 'Binaries\Win64\XComGame.com'

New-Item -ItemType Directory -Force -Path $scans, $tmp | Out-Null

if (-not $BuildOnly) {
    if (-not (Test-Path -LiteralPath $editor)) { throw "SDK editor not found at $editor" }

    # Particles are not confined to FX_*: weapons, characters and destructibles
    # all carry their own systems, so sweep every package the model pass saw.
    if ($PackageList -and (Test-Path -LiteralPath $PackageList)) {
        $packages = @(Get-Content $PackageList | ForEach-Object { $_.Trim() } | Where-Object { $_ }) | Sort-Object
    } else {
        $packages = @(Get-ChildItem $lib -Directory | Select-Object -ExpandProperty Name) | Sort-Object
    }
    if ($Of -gt 1) {
        $all = @($packages)
        $packages = @(for ($i = $Slice; $i -lt $all.Count; $i += $Of) { $all[$i] })
    }

    $n = 0
    foreach ($pkg in $packages) {
        $n++
        $tsv = Join-Path $scans "$pkg.tsv"
        if (Test-Path -LiteralPath $tsv) { continue }
        Write-Progress -Activity 'xcom_fx' -Status "$pkg ($n/$($packages.Count))" `
            -PercentComplete ([int](100 * $n / $packages.Count))

        $dump = Join-Path $tmp $pkg
        $prev = Get-Location
        try {
            Set-Location -LiteralPath (Split-Path -Parent $editor)
            & $editor batchexport $pkg ParticleSystem T3D $dump 2>&1 | Out-Null
        } catch { } finally { Set-Location -LiteralPath $prev }

        if ((Test-Path -LiteralPath $dump) -and
            @(Get-ChildItem $dump -Filter *.T3D -EA SilentlyContinue).Count -gt 0) {
            & py -3 (Join-Path $PSScriptRoot 'xcom_fx.py') scan `
                --pkg $pkg --dump $dump --out $tsv | Out-Null
        }
        Remove-Item -LiteralPath $dump -Recurse -Force -ErrorAction SilentlyContinue
        # Empty marker so packages with no particles are not retried.
        if (-not (Test-Path -LiteralPath $tsv)) { New-Item -ItemType File -Path $tsv -Force | Out-Null }
    }
    Write-Progress -Activity 'xcom_fx' -Completed
}

if ($Of -le 1) {
    & py -3 (Join-Path $PSScriptRoot 'xcom_fx.py') build `
        --scans $scans --out (Join-Path $repo 'xcom_extracted\fx_systems.csv')
}
