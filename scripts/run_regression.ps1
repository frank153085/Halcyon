param(
    [string]$Demo = "out/build/m3-msvc-debug/HalcyonM3Demo.exe",
    [string]$GoldenCompare = "out/build/m3-msvc-debug/HalcyonGoldenCompare.exe",
    [string]$GoldenDirectory = "Tests/GoldenImages",
    [string]$CaptureDirectory = "out/captures/regression"
)

$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path $CaptureDirectory | Out-Null

foreach ($scene in @("damaged-helmet", "sponza")) {
    $actual = Join-Path $CaptureDirectory "$scene.png"
    $csv = Join-Path $CaptureDirectory "$scene.csv"
    $golden = Join-Path $GoldenDirectory "$scene-golden.png"
    & $Demo --scene $scene --golden $golden --frames 120 --fixed-dt 0.016666 `
        --no-taa --screenshot $actual --perf-csv $csv --no-validation
    if ($LASTEXITCODE -ne 0) {
        throw "M3 demo failed for scene '$scene' (exit code $LASTEXITCODE)"
    }
    if (Test-Path $GoldenCompare) {
        & $GoldenCompare --actual $actual --golden $golden
        if ($LASTEXITCODE -ne 0) {
            throw "Golden comparison failed for scene '$scene' (exit code $LASTEXITCODE)"
        }
    }
}

Write-Host "M3 regression completed for damaged-helmet and sponza."
