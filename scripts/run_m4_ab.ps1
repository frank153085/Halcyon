param(
    [string]$Demo = "out\build\m3-msvc-debug\HalcyonM3Demo.exe",
    [string]$CaptureDirectory = "out\captures\m4-ab",
    [int]$Frames = 120
)

$ErrorActionPreference = "Stop"
if (-not (Test-Path -LiteralPath $Demo)) {
    throw "Demo executable not found: $Demo"
}
if ($Frames -lt 2) {
    throw "Frames must be at least 2."
}

New-Item -ItemType Directory -Force -Path $CaptureDirectory | Out-Null
foreach ($scene in @("damaged-helmet", "sponza")) {
    $baseline = Join-Path $CaptureDirectory "$scene-legacy.png"
    $gpuCapture = Join-Path $CaptureDirectory "$scene-gpu.png"
    $gpuPerf = Join-Path $CaptureDirectory "$scene-gpu-perf.csv"

    & $Demo --scene $scene --width 640 --height 360 --frames $Frames `
        --fixed-dt 0.016666 --no-taa --screenshot $baseline --no-validation
    if ($LASTEXITCODE -ne 0) {
        throw "Legacy baseline failed for scene '$scene' (exit code $LASTEXITCODE)"
    }

    & $Demo --scene $scene --width 640 --height 360 --frames $Frames `
        --fixed-dt 0.016666 --no-taa --gpu-driven --screenshot $gpuCapture `
        --golden $baseline --perf-csv $gpuPerf --no-validation
    if ($LASTEXITCODE -ne 0) {
        throw "GPU-driven A/B comparison failed for scene '$scene' (exit code $LASTEXITCODE)"
    }

    $rows = @(Import-Csv -LiteralPath $gpuPerf |
        Where-Object { $_.frame -match '^\d+$' })
    if (-not $rows -or @($rows | Where-Object { [int]$_.gpu_driven_active -eq 1 }).Count -eq 0) {
        throw "GPU-driven path was not active for scene '$scene'."
    }
    if (@($rows | Where-Object { [int]$_.indirect_draw_count -gt 0 }).Count -eq 0) {
        throw "Scene '$scene' produced no completed GPU indirect work."
    }
    if ($scene -eq "sponza" -and
        @($rows | Where-Object { [int]$_.gpu_fallback_instance_count -gt 0 }).Count -eq 0) {
        throw "Sponza did not exercise the expected CPU fallback material path."
    }
    if ($scene -eq "sponza" -and @($rows | Where-Object {
            [int]$_.gpu_fallback_instance_count -gt 0 -and
            [int]$_.indirect_draw_count -gt 0
        }).Count -eq 0) {
        throw "Sponza did not produce a frame containing both GPU indirect and CPU fallback work."
    }
    if (@($rows | Where-Object { [int]$_.material_descriptor_bind_count -gt 0 }).Count -gt 0 -and
        @($rows | Where-Object { [int]$_.gpu_fallback_instance_count -gt 0 }).Count -eq 0) {
        throw "Scene '$scene' reported material descriptor binds without CPU fallback instances."
    }
}

Write-Host "M4 A/B comparison passed for damaged-helmet and sponza."
