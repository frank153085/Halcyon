param(
    [string]$Exe = "out\build\m3-msvc-debug\HalcyonM3Demo.exe",
    [int]$InstanceCount = 100000,
    [int]$Frames = 4,
    [string]$OutputDirectory = "out\captures\m4-instance-id"
)

$ErrorActionPreference = "Stop"
if (-not (Test-Path -LiteralPath $Exe)) {
    throw "Demo executable not found: $Exe"
}
if ($Frames -lt 4) {
    throw "Frames must be at least 4 so a frame-slot readback can complete."
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$referencePath = Join-Path $OutputDirectory "reference.csv"
$occlusionPath = Join-Path $OutputDirectory "two-phase.csv"
$summaryPath = Join-Path $OutputDirectory "summary.csv"
Remove-Item -LiteralPath $referencePath,$occlusionPath,$summaryPath -ErrorAction SilentlyContinue

& $Exe --scene stress --instance-count $InstanceCount --frames $Frames `
    --fixed-dt 0.016666 --no-taa --gpu-driven --instance-id-report $referencePath `
    --no-validation
if ($LASTEXITCODE -ne 0) {
    throw "Reference visibility run failed with exit code $LASTEXITCODE"
}

& $Exe --scene stress --instance-count $InstanceCount --frames $Frames `
    --fixed-dt 0.016666 --no-taa --two-phase-occlusion `
    --instance-id-report $occlusionPath --no-validation
if ($LASTEXITCODE -ne 0) {
    throw "Two-phase visibility run failed with exit code $LASTEXITCODE"
}

function Read-IdReport([string]$Path) {
    $frames = @{}
    if (-not (Test-Path -LiteralPath $Path)) { return $frames }
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $parts = $line.Split(',')
        if ($parts.Count -eq 0) { continue }
        $frame = [uint64]$parts[0]
        $set = [System.Collections.Generic.HashSet[uint32]]::new()
        for ($index = 1; $index -lt $parts.Count; ++$index) {
            [void]$set.Add([uint32]$parts[$index])
        }
        $frames[$frame] = $set
    }
    return $frames
}

$reference = Read-IdReport $referencePath
$occlusion = Read-IdReport $occlusionPath
if ($reference.Count -eq 0 -or $occlusion.Count -eq 0) {
    throw "No completed InstanceId frame was reported; increase -Frames."
}

$missing = 0
$comparedFrames = 0
foreach ($frame in $reference.Keys) {
    if (-not $occlusion.ContainsKey($frame)) {
        throw "Two-phase report is missing reference frame $frame."
    }
    ++$comparedFrames
    foreach ($id in $reference[$frame]) {
        if (-not $occlusion[$frame].Contains($id)) { ++$missing }
    }
}

"frame,reference_ids,two_phase_ids,missing_ids" | Set-Content -LiteralPath $summaryPath
foreach ($frame in $reference.Keys) {
    "$frame,$($reference[$frame].Count),$($occlusion[$frame].Count),$missing" |
        Add-Content -LiteralPath $summaryPath
}
if ($missing -ne 0) {
    throw "Two-phase InstanceId comparison found $missing missing visible object id(s)."
}

Write-Host "M4 InstanceId comparison passed for $comparedFrames completed frame(s)."
