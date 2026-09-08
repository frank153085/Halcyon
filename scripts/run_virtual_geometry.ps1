param(
    [string]$Ply = "",
    [string]$Output = "",
    [string]$Build = "$PSScriptRoot/../out/m5clang"
)
$ErrorActionPreference = "Stop"
if (-not $Ply) {
    $Ply = Join-Path "$PSScriptRoot/../out/m5-assets/lucy" "lucy.ply"
}
if (-not $Output) { $Output = "$Ply.halcyon.vgcache" }
$cooker = Join-Path $Build "HalcyonCooker.exe"
if (-not (Test-Path $cooker)) { throw "HalcyonCooker not found: $cooker" }
if (-not (Test-Path $Ply)) { Write-Host "M5 Lucy: skipped (PLY is missing: $Ply)"; exit 0 }
& $cooker --input $Ply --output $Output --lod-count 3 --max-vertices 64 --max-triangles 124
if ($LASTEXITCODE -ne 0) { throw "HalcyonCooker failed with exit code $LASTEXITCODE" }
Write-Host "Virtual geometry cache: $Output"
