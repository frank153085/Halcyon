param(
    [string]$Exe = "out\build\m3-msvc-debug\HalcyonM3Demo.exe",
    [int]$InstanceCount = 100000,
    [int]$Frames = 4,
    [int]$FramesInFlight = 3,
    [string]$OutputDirectory = "out\captures\m4-instance-id",
    [double]$MinimumReductionFraction = 0.05
)

$ErrorActionPreference = "Stop"
if (-not (Test-Path -LiteralPath $Exe)) {
    throw "Demo executable not found: $Exe"
}
$minimumFrames = $FramesInFlight + 1
# VulkanGpuSceneBuffers receives the renderer's framesInFlight value (3 by
# default). Readback for a submitted slot is only available after its fence
# completes, so use at least framesInFlight + 1 frames. Pass a matching value
# here if the renderer's frames-in-flight configuration changes.
if ($Frames -lt $minimumFrames) {
    throw "Frames must be at least $minimumFrames (framesInFlight + 1) so a frame-slot readback can complete."
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$referencePath = Join-Path $OutputDirectory "reference.csv"
$occlusionPath = Join-Path $OutputDirectory "two-phase.csv"
$referencePerfPath = Join-Path $OutputDirectory "reference-perf.csv"
$occlusionPerfPath = Join-Path $OutputDirectory "two-phase-perf.csv"
$summaryPath = Join-Path $OutputDirectory "summary.csv"
Remove-Item -LiteralPath $referencePath,$occlusionPath,$referencePerfPath,$occlusionPerfPath,$summaryPath -ErrorAction SilentlyContinue

& $Exe --scene stress --instance-count $InstanceCount --frames $Frames `
    --fixed-dt 0.016666 --no-taa --gpu-driven --instance-id-report $referencePath `
    --perf-csv $referencePerfPath --no-validation
if ($LASTEXITCODE -ne 0) {
    throw "Reference visibility run failed with exit code $LASTEXITCODE"
}

& $Exe --scene stress --instance-count $InstanceCount --frames $Frames `
    --fixed-dt 0.016666 --no-taa --gpu-driven --two-phase-occlusion `
    --instance-id-report $occlusionPath --perf-csv $occlusionPerfPath --no-validation
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
$missingByFrame = @{}
foreach ($frame in $reference.Keys) {
    if (-not $occlusion.ContainsKey($frame)) {
        throw "Two-phase report is missing reference frame $frame."
    }
    ++$comparedFrames
    $frameMissing = 0
    foreach ($id in $reference[$frame]) {
        if (-not $occlusion[$frame].Contains($id)) {
            ++$missing
            ++$frameMissing
        }
    }
    $missingByFrame[$frame] = $frameMissing
}

"frame,reference_ids,two_phase_ids,missing_ids" | Set-Content -LiteralPath $summaryPath
foreach ($frame in $reference.Keys) {
    "$frame,$($reference[$frame].Count),$($occlusion[$frame].Count),$($missingByFrame[$frame])" |
        Add-Content -LiteralPath $summaryPath
}
if ($missing -ne 0) {
    throw "Two-phase InstanceId comparison found $missing missing visible object id(s)."
}

function Read-PerfRows([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return @{} }
    $result = @{}
    foreach ($row in Import-Csv -LiteralPath $Path) {
        if ($row.frame -notmatch '^\d+$') { continue }
        $result[[uint64]$row.frame] = $row
    }
    return $result
}

$referencePerf = Read-PerfRows $referencePerfPath
$occlusionPerf = Read-PerfRows $occlusionPerfPath
$reducedFrames = 0
$comparedPerfFrames = 0
$referenceTotal = [double]0
$occlusionTotal = [double]0
foreach ($frame in $referencePerf.Keys) {
    if (-not $occlusionPerf.ContainsKey($frame)) { continue }
    $referenceRow = $referencePerf[$frame]
    $occlusionRow = $occlusionPerf[$frame]
    if ([int]$referenceRow.gpu_driven_active -ne 1 -or
        [int]$occlusionRow.gpu_driven_active -ne 1) { continue }
    ++$comparedPerfFrames
    $referenceCount = [double]$referenceRow.visible_instance_count
    $occlusionCount = [double]$occlusionRow.indirect_draw_count
    $referenceTotal += $referenceCount
    $occlusionTotal += $occlusionCount
    if ($occlusionCount -lt $referenceCount) { ++$reducedFrames }
}
if ($comparedPerfFrames -eq 0) {
    throw "No matching GPU-driven performance frames were recorded."
}
$requiredTotal = $referenceTotal * (1.0 - $MinimumReductionFraction)
if ($occlusionTotal -ge $requiredTotal -or $reducedFrames -eq 0) {
    throw "Two-phase occlusion did not reduce indirect work: reference=$referenceTotal, two_phase=$occlusionTotal, reduced_frames=$reducedFrames."
}

Write-Host "M4 InstanceId comparison passed for $comparedFrames completed frame(s); two-phase reduced indirect work on $reducedFrames/$comparedPerfFrames frames."
