<#
    xcom_audio.ps1 - decode XCOM 2's Wwise audio to WAV.

    XCOM 2's sound is not in the .upk packages: the SoundNodeWave objects there
    are stubs that export as zero bytes. The real audio is Wwise-encoded in
    XComGame\Content\WwiseAudio - ~24,800 .wem files (Wwise Vorbis) named by
    numeric ID, with one .txt manifest per bank that maps those IDs to names.

    Decoding needs vgmstream, which is NOT part of the SDK and not vendored
    here. Point -VgmStream at vgmstream-cli.exe, or let the script find it on
    PATH.  https://github.com/vgmstream/vgmstream/releases

        .\tools\xcom2\xcom_audio.ps1 -VgmStream C:\tools\vgmstream\vgmstream-cli.exe
        .\tools\xcom2\xcom_audio.ps1 -Slice 0 -Of 4        # one parallel worker

    Output: <Dest>/<BankName>/<SoundName>.wav, plus _unnamed/<id>.wav for the
    ~13% of files no manifest covers.

    SIZE WARNING: Wwise Vorbis at ~62 kbps expands to 1536 kbps PCM, so 2.5 GB
    of .wem decodes to roughly 60 GB of WAV. Check free space first.
#>
[CmdletBinding()]
param(
    [string]$SdkRoot = 'E:\SteamLibrary\steamapps\common\XCOM 2 SDK',
    [string]$VgmStream,
    [string]$Dest,
    [int]$Slice = 0,
    [int]$Of = 1,
    [switch]$WhatIfSize,
    # Convert the audio EMBEDDED IN THE BANKS rather than the loose .wem files.
    # This is where the bulk lives: 24.8k .wem sit loose on disk (mostly voice)
    # while 286.7k more are packed inside the .bnk files - every weapon,
    # footstep, impact and environment sound among them.
    [switch]$Banks,
    # Keep only this localised VO folder. XCOM ships five (English(US),
    # French(France), German, Italian, Spanish(Spain)) and the other four are
    # ~75% of the voice audio. _common (SFX, music, ambience) is never skipped.
    [string]$Language
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$wwise = Join-Path $SdkRoot 'XComGame\Content\WwiseAudio'
if (-not $Dest) { $Dest = Join-Path $repo 'xcom_extracted\audio' }

if (-not $VgmStream) {
    $c = Get-Command 'vgmstream-cli.exe' -ErrorAction SilentlyContinue
    if ($c) { $VgmStream = $c.Source }
}
if (-not $VgmStream -or -not (Test-Path -LiteralPath $VgmStream)) {
    throw "vgmstream-cli.exe not found. Download it from https://github.com/vgmstream/vgmstream/releases and pass -VgmStream <path>."
}
if (-not (Test-Path -LiteralPath $wwise)) { throw "No WwiseAudio at $wwise" }

$langArg = if ($Language) { @('--language', $Language) } else { @() }
if ($Banks) {
    $planFile = Join-Path $repo "workbench\bnk_plan_$Slice.tsv"
    $stage = Join-Path $repo 'workbench\bnk_stage'
    & py -3 (Join-Path $PSScriptRoot 'xcom_audio.py') banks --wwise $wwise --stage $stage `
        --dest $Dest --slice $Slice --of $Of --out $planFile @langArg | Out-Null
} else {
    $planFile = Join-Path $repo "workbench\audio_plan_$Slice.tsv"
    & py -3 (Join-Path $PSScriptRoot 'xcom_audio.py') plan --wwise $wwise --dest $Dest `
        --slice $Slice --of $Of --out $planFile @langArg | Out-Null
}

$plan = @(Get-Content $planFile | Where-Object { $_ -match "`t" })
Write-Host ("worker {0}/{1}: {2} files" -f $Slice, $Of, $plan.Count)

if ($WhatIfSize) {
    # Decode a sample and extrapolate rather than guessing the ratio.
    $sample = $plan | Get-Random -Count ([Math]::Min(12, $plan.Count))
    $inB = 0; $outB = 0
    $tmp = Join-Path $env:TEMP 'xcom_audio_probe.wav'
    foreach ($row in $sample) {
        $src, $dst = $row -split "`t", 2
        & $VgmStream -o $tmp $src 2>&1 | Out-Null
        if (Test-Path $tmp) {
            $inB += (Get-Item $src).Length; $outB += (Get-Item $tmp).Length
            Remove-Item $tmp -Force
        }
    }
    if ($inB -gt 0) {
        $ratio = $outB / $inB
        $total = (Get-ChildItem $wwise -Recurse -Filter *.wem | Measure-Object Length -Sum).Sum
        Write-Host ("expansion {0:N1}x  ->  estimated {1:N1} GB of WAV" -f $ratio, ($total * $ratio / 1GB))
    }
    return
}

$done = 0; $failed = 0; $started = Get-Date
foreach ($row in $plan) {
    $src, $dst = $row -split "`t", 2
    $done++
    if (Test-Path -LiteralPath $dst) { continue }
    $dir = Split-Path -Parent $dst
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    try {
        & $VgmStream -o $dst $src 2>&1 | Out-Null
        if (-not (Test-Path -LiteralPath $dst)) { $failed++ }
    } catch { $failed++ }

    if ($done % 500 -eq 0) {
        $el = (Get-Date) - $started
        Write-Progress -Activity "xcom_audio $Slice/$Of" -Status "$done/$($plan.Count)" `
            -PercentComplete ([int](100 * $done / $plan.Count)) `
            -SecondsRemaining ($el.TotalSeconds / $done * ($plan.Count - $done))
    }
}
Write-Progress -Activity "xcom_audio $Slice/$Of" -Completed
Write-Host ("worker {0}: {1} converted, {2} failed, {3:hh\:mm\:ss}" -f `
    $Slice, ($done - $failed), $failed, ((Get-Date) - $started))
