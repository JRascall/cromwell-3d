<#
    xcom_anim.ps1 - export skinned meshes and animations with umodel.

    The SDK cannot do this. Its FBX exporter writes 0-byte files (there is no
    FBX SDK DLL in Binaries) and PSK/PSA/ASE export nothing, so SkeletalMesh
    and AnimSequence are unreachable through BatchExport. umodel reads the
    .upk directly and does not care.

        .\tools\xcom2\xcom_anim.ps1 -UModel C:\tools\umodel\umodel_64.exe
        .\tools\xcom2\xcom_anim.ps1 -UModel <path> -Slice 0 -Of 4     # one worker
        .\tools\xcom2\xcom_anim.ps1 -UModel <path> -List              # what would run

    umodel is third party and NOT vendored here:
    https://www.gildor.org/en/projects/umodel  (the site blocks hotlinking -
    a browser Referer header is needed if you fetch it programmatically).

    USE THE 64-BIT BUILD. umodel.exe (32-bit) crashes on these packages with
    exit 255 and no output; umodel_64.exe from the same zip is fine.

    Output: xcom_extracted/anim/<Package>/
        AnimSet/*.psa        ActorX animation - BONENAMES, ANIMINFO, ANIMKEYS
        SkeletalMesh/*.psk   skinned mesh with its skeleton
        StaticMesh/*.pskx    static counterparts

    Textures are skipped by default (-notex): the SDK sweep already produced
    every Texture2D as PNG, and re-exporting them as TGA costs ~150 MB per
    package for no gain. Pass -WithTextures to include them anyway.

    NOTE: .psa carries no AnimNotify events. Clip names, lengths, frame counts
    and notify timings come from xcom_anim.py instead - the two are
    complementary, so run both.
#>
[CmdletBinding()]
param(
    [string]$SdkRoot = 'E:\SteamLibrary\steamapps\common\XCOM 2 SDK',
    [string]$UModel,
    [switch]$WithTextures,
    [switch]$WithStatic,
    # Second pass: pull AnimNotify events (footstep / weapon-fire / IK timings)
    # from the SDK instead of umodel. ActorX .psa carries no notifies at all,
    # so this is the only route to them. Does not need umodel.
    [switch]$Notifies,
    [switch]$List,
    [int]$Slice = 0,
    [int]$Of = 1
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$content = Join-Path $SdkRoot 'XComGame\Content'
$dest = Join-Path $repo 'xcom_extracted\anim'

if (-not $Notifies) {
    if (-not $UModel) {
        $c = Get-Command 'umodel_64.exe' -ErrorAction SilentlyContinue
        if ($c) { $UModel = $c.Source }
    }
    if (-not $UModel -or -not (Test-Path -LiteralPath $UModel)) {
        throw "umodel_64.exe not found. Get it from https://www.gildor.org/en/projects/umodel and pass -UModel <path>."
    }
}

# Everything that can hold a skeleton or a curve: the ANIM packages plus the
# character, weapon and vehicle trees. Environment packages are pure
# StaticMesh and already covered by xcom_bulk.ps1.
$packages = @(Get-ChildItem $content -Recurse -File -Filter *.upk | Where-Object {
    $_.BaseName -match 'ANIM' -or
    $_.DirectoryName -match '\\(Characters|Weapons|Vehicles)(\\|$)'
}) | Sort-Object FullName -Unique

if ($List) {
    $packages | Select-Object @{n='MB';e={[math]::Round($_.Length/1MB,1)}},BaseName,
        @{n='Where';e={$_.DirectoryName.Replace("$content\",'')}} |
        Sort-Object MB -Descending | Format-Table -AutoSize
    "{0} packages, {1:N2} GB" -f $packages.Count, (($packages|Measure-Object Length -Sum).Sum/1GB)
    return
}

if ($Of -gt 1) {
    $all = @($packages)
    $packages = @(for ($i = $Slice; $i -lt $all.Count; $i += $Of) { $all[$i] })
}

# ------------------------------------------------------ notify events pass
if ($Notifies) {
    $editor = Join-Path $SdkRoot 'Binaries\Win64\XComGame.com'
    if (-not (Test-Path -LiteralPath $editor)) { throw "SDK editor not found at $editor" }
    $scans = Join-Path $dest '_notify_scans'
    $tmp = Join-Path $repo 'workbench\anim_t3d'
    New-Item -ItemType Directory -Force -Path $scans, $tmp | Out-Null

    $n = 0
    foreach ($p in $packages) {
        $n++
        $tsv = Join-Path $scans "$($p.BaseName).tsv"
        if (Test-Path -LiteralPath $tsv) { continue }
        Write-Progress -Activity 'xcom_anim notifies' -Status "$($p.BaseName) ($n/$($packages.Count))" `
            -PercentComplete ([int](100 * $n / $packages.Count))

        $dump = Join-Path $tmp $p.BaseName
        $prev = Get-Location
        try {
            Set-Location -LiteralPath (Split-Path -Parent $editor)
            & $editor batchexport $p.BaseName AnimSequence T3D $dump 2>&1 | Out-Null
        } finally { Set-Location -LiteralPath $prev }

        if (Test-Path -LiteralPath $dump) {
            & py -3 (Join-Path $PSScriptRoot 'xcom_anim.py') scan `
                --pkg $p.BaseName --dump $dump --out $tsv | Out-Null
            Remove-Item -LiteralPath $dump -Recurse -Force -ErrorAction SilentlyContinue
        }
        if (-not (Test-Path -LiteralPath $tsv)) { New-Item -ItemType File -Path $tsv -Force | Out-Null }
    }
    Write-Progress -Activity 'xcom_anim notifies' -Completed

    if ($Of -le 1) {
        & py -3 (Join-Path $PSScriptRoot 'xcom_anim.py') build `
            --mats $scans --out (Join-Path $repo 'xcom_extracted\anim_notifies.csv')
    }
    return
}

$flags = @('-export', "-out=$dest", "-path=$content")
if (-not $WithTextures) { $flags += '-notex' }
# umodel 1590 cannot deserialise XCOM's StaticMesh variant - it dies with
# "RawArray item size mismatch" and takes the whole package with it, including
# the skeletal meshes and animations we actually came for. Static geometry is
# already covered by xcom_bulk.ps1, so skip it unless explicitly asked.
if (-not $WithStatic) { $flags += '-nostat' }

New-Item -ItemType Directory -Force -Path $dest | Out-Null
$doneDir = Join-Path $dest '_done'
New-Item -ItemType Directory -Force -Path $doneDir | Out-Null
$started = Get-Date
$done = 0; $skipped = 0; $failed = @()

foreach ($p in $packages) {
    $done++
    # Resume on an explicit marker, not on the output folder: plenty of
    # packages legitimately export nothing (they hold only static meshes) and
    # would otherwise be retried on every run.
    $marker = Join-Path $doneDir "$($p.BaseName).ok"
    if (Test-Path -LiteralPath $marker) { $skipped++; continue }

    Write-Progress -Activity 'xcom_anim' -Status "$($p.BaseName) ($done/$($packages.Count))" `
        -PercentComplete ([int](100 * $done / $packages.Count))

    $prev = Get-Location
    try {
        # umodel resolves its own data paths relative to its directory.
        Set-Location -LiteralPath (Split-Path -Parent $UModel)
        $log = & $UModel @flags $p.FullName 2>&1
        if ($LASTEXITCODE -ne 0) { throw ($log | Select-Object -Last 3 | Out-String) }
        New-Item -ItemType File -Path $marker -Force | Out-Null
    }
    catch {
        $failed += [pscustomobject]@{ Package = $p.BaseName; Error = "$_" }
        Write-Host "  FAILED $($p.BaseName)" -ForegroundColor Yellow
    }
    finally { Set-Location -LiteralPath $prev }
}
Write-Progress -Activity 'xcom_anim' -Completed

$took = (Get-Date) - $started
Write-Host ("`nSwept {0} packages in {1:hh\:mm\:ss} ({2} skipped, {3} failed)" -f `
    $packages.Count, $took, $skipped, $failed.Count)
if ($failed) {
    # Per-slice filename: parallel workers sharing one path would each
    # overwrite the others and only the last one's failures would survive.
    $csv = Join-Path $dest "failed_$Slice.csv"
    $failed | Export-Csv $csv -NoTypeInformation
    Write-Host "  failures in $csv"
}
