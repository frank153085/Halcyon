param(
    [string]$Exe = "out\build\m3-msvc-debug\HalcyonM3Demo.exe",
    [int]$InstanceCount = 100000,
    [int]$Frames = 12,
    [string]$Output = "out\captures\m4-visibility.csv"
)

$ErrorActionPreference = "Stop"
if (-not (Test-Path -LiteralPath $Exe)) {
    throw "Demo executable not found: $Exe"
}
if ($Frames -lt 4) {
    throw "Frames must be at least 4 so a frame-slot readback can complete."
}

$parent = Split-Path -Parent $Output
if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
Remove-Item -LiteralPath $Output -ErrorAction SilentlyContinue

& $Exe --scene stress --instance-count $InstanceCount --frames $Frames `
    --two-phase-occlusion --no-validation --perf-csv $Output
if ($LASTEXITCODE -ne 0) {
    throw "M4 visibility run failed with exit code $LASTEXITCODE"
}

$rows = @(Import-Csv -LiteralPath $Output |
    Where-Object { $_.frame -match '^\d+$' -and $_.gpu_visibility_validation_passed -eq '1' }
)
if (-not $rows) {
    throw "No completed visibility readback was recorded; increase -Frames."
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
