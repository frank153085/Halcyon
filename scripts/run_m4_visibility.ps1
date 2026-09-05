param(
    [string]$Exe = "out\build\m3-msvc-debug\HalcyonM3Demo.exe",
    [int]$InstanceCount = 100000,
    [int]$Frames = 12,
    [int]$FramesInFlight = 3,
    [string]$Output = "out\captures\m4-visibility.csv"
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

$parent = Split-Path -Parent $Output
if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
Remove-Item -LiteralPath $Output -ErrorAction SilentlyContinue

# The stress entry point enables this mode by default, but keep the switch
# explicit so this audit cannot silently regress to the legacy M3 path.
& $Exe --scene stress --instance-count $InstanceCount --frames $Frames `
    --gpu-driven --two-phase-occlusion --no-validation --perf-csv $Output
if ($LASTEXITCODE -ne 0) {
    throw "M4 visibility run failed with exit code $LASTEXITCODE"
}

$rows = @(Import-Csv -LiteralPath $Output |
    Where-Object { $_.frame -match '^\d+$' -and $_.gpu_visibility_validation_passed -eq '1' }
)
if (-not $rows) {
    throw "No completed visibility readback was recorded; increase -Frames."
}
$inactive = @($rows | Where-Object { [int]$_.gpu_driven_active -ne 1 })
if ($inactive.Count -ne 0) {
    throw "Stress visibility audit did not execute the GPU-driven path in $($inactive.Count) frame(s)."
}
$bad = @($rows | Where-Object { [int]$_.gpu_visibility_missing_count -ne 0 })
if ($bad.Count -ne 0) {
    throw "GPU visibility audit reported missing instances in $($bad.Count) frame(s)."
}
$invalid = @($rows | Where-Object { [int]$_.gpu_instance_id_invalid_pixels -ne 0 })
if ($invalid.Count -ne 0) {
    throw "InstanceId attachment reported invalid pixels in $($invalid.Count) frame(s)."
}
$slow = @($rows | Where-Object { [double]$_.cpu_visibility_ms -ge 1.0 })
if ($slow.Count -ne 0) {
    throw "CPU visibility exceeded 1 ms in $($slow.Count) completed frame(s)."
}
$materialBinds = @($rows | Where-Object { [int]$_.material_descriptor_bind_count -ne 0 })
if ($materialBinds.Count -ne 0) {
    throw "GPU-driven path recorded per-material descriptor binds in $($materialBinds.Count) frame(s)."
}

Write-Host "M4 visibility audit passed for $($rows.Count) completed frame(s)."
