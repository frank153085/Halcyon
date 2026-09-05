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

    & $Demo --scene $scene --width 640 --height 360 --frames $Frames `
        --fixed-dt 0.016666 --no-taa --screenshot $baseline --no-validation
    if ($LASTEXITCODE -ne 0) {
        throw "Legacy baseline failed for scene '$scene' (exit code $LASTEXITCODE)"
    }

    & $Demo --scene $scene --width 640 --height 360 --frames $Frames `
        --fixed-dt 0.016666 --no-taa --gpu-driven --screenshot $gpuCapture `
        --golden $baseline --no-validation
    if ($LASTEXITCODE -ne 0) {
        throw "GPU-driven A/B comparison failed for scene '$scene' (exit code $LASTEXITCODE)"
    }
}

Write-Host "M4 A/B comparison passed for damaged-helmet and sponza."
