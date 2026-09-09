param(
    [string]$Exe = "$PSScriptRoot/../out/m5fg2/Examples/HalcyonExample03PbrScenes/HalcyonExample03PbrScenes.exe",
    [string]$GoldenCompare = "$PSScriptRoot/../out/m5fg2/HalcyonGoldenCompare.exe",
    [string]$CaptureDirectory = "$PSScriptRoot/../out/captures/lucy-final",
    # The application records performance rows after a 300-frame warmup and
    # for the following 1800 measurement frames. Keep the default long enough
    # to produce a real CSV while still allowing callers to request a longer run.
    [int]$Frames = 2100,
    [int]$Width = 1280,
    [int]$Height = 720,
    [float]$FixedDt = 0.016666,
    [string]$FlipExecutable = "",
    [double]$SsimThreshold = 0.995,
    [string]$RenderDocExecutable = "C:\Program Files\RenderDoc\renderdoccmd.exe",
    [switch]$RenderDoc,
    [switch]$RequireImageMetrics,
    [switch]$Offline
)

$ErrorActionPreference = "Stop"
if ($SsimThreshold -lt 0.0 -or $SsimThreshold -gt 1.0) {
    throw "SsimThreshold must be in the inclusive range [0, 1]."
}
$root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$captureRoot = if ([System.IO.Path]::IsPathRooted($CaptureDirectory)) {
    [System.IO.Path]::GetFullPath($CaptureDirectory)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $root $CaptureDirectory))
}
$lucyPly = Join-Path $root "assets/models/lucy/lucy.ply"
if (-not (Test-Path -LiteralPath $lucyPly)) {
    if ($Offline) { Write-Host "M5 Lucy acceptance: skipped (assets/models/lucy/lucy.ply is missing)"; exit 0 }
    throw "Lucy PLY is missing: $lucyPly. Run scripts/fetch_m5_assets.ps1 first."
}
$lucyHeaderBuffer = New-Object byte[] (4MB)
$lucyHeaderStream = [System.IO.File]::OpenRead($lucyPly)
try { $lucyHeaderLength = $lucyHeaderStream.Read($lucyHeaderBuffer, 0, $lucyHeaderBuffer.Length) }
finally { $lucyHeaderStream.Dispose() }
$lucyHeader = [System.Text.Encoding]::ASCII.GetString($lucyHeaderBuffer, 0, $lucyHeaderLength)
$lucyFaceMatch = [regex]::Match($lucyHeader, '(?m)^element\s+face\s+(\d+)\s*$')
if (-not $lucyFaceMatch.Success) { throw "Lucy PLY has no face element in its header" }
$lucyFaceCount = [UInt64]::Parse($lucyFaceMatch.Groups[1].Value,
    [Globalization.CultureInfo]::InvariantCulture)
if ($lucyFaceCount -lt 28000000) {
    throw "Lucy PLY is unexpectedly small: $lucyFaceCount face records (minimum 28000000)"
}
if (-not (Test-Path -LiteralPath $Exe)) { throw "Lucy acceptance executable not found: $Exe" }
New-Item -ItemType Directory -Force -Path $captureRoot | Out-Null

$runFrames = [Math]::Max($Frames, 2100)
$common = @("--scene", "lucy", "--width", $Width, "--height", $Height,
    "--frames", $runFrames, "--fixed-dt", $FixedDt, "--exposure", 0.0,
    "--no-validation", "--no-vsync")
$paths = @(
    @{ Name = "deferred"; ExpectedPath = "DeferredIndexed"; Args = @("--no-gpu-driven") },
    @{ Name = "gpu_driven"; ExpectedPath = "GpuDrivenIndexed"; Args = @("--gpu-driven") },
    @{ Name = "virtual"; ExpectedPath = "VirtualGeometryIndexed"; Args = @("--virtual-geometry") }
)

foreach ($path in $paths) {
    $png = Join-Path $captureRoot "$($path.Name).png"
    $csv = Join-Path $captureRoot "$($path.Name).csv"
    $runArgs = @($common + $path.Args + @("--screenshot", $png, "--perf-csv", $csv))
    & $Exe @runArgs
    if ($LASTEXITCODE -ne 0) { throw "Lucy $($path.Name) run failed with exit code $LASTEXITCODE" }
    if (-not (Test-Path -LiteralPath $png) -or -not (Test-Path -LiteralPath $csv)) {
        throw "Lucy $($path.Name) did not produce both PNG and CSV outputs."
    }
    $csvHeader = Get-Content -LiteralPath $csv -TotalCount 1
    foreach ($requiredColumn in @("render_path", "virtual_visible_meshlet_count", "virtual_indirect_command_count", "virtual_invalid_visibility_count")) {
        if ($csvHeader -notmatch ("(^|,)" + [regex]::Escape($requiredColumn) + "(,|$)")) {
            throw "Lucy $($path.Name) performance CSV is missing column '$requiredColumn'."
        }
    }
    $csvRows = Import-Csv -LiteralPath $csv
    if ($csvRows.Count -eq 0) {
        throw "Lucy $($path.Name) performance CSV contains no measured rows."
    }
    $reportedPaths = @($csvRows | ForEach-Object { $_.render_path } |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Select-Object -Unique)
    if ($reportedPaths.Count -ne 1 -or $reportedPaths[0] -ne $path.ExpectedPath) {
        throw "Lucy $($path.Name) reported unexpected render path(s): $($reportedPaths -join ', ')"
    }
    $virtualRows = @($csvRows | Where-Object { $_.render_path -eq "VirtualGeometryIndexed" })
    if ($path.ExpectedPath -eq "VirtualGeometryIndexed") {
        if ($virtualRows.Count -eq 0) {
            throw "Lucy virtual performance CSV contains no VirtualGeometryIndexed rows."
        }
        $visibleRows = @($virtualRows | Where-Object {
            [UInt64]::Parse($_.virtual_visible_meshlet_count) -gt 0 -and
                [UInt64]::Parse($_.virtual_indirect_command_count) -gt 0
        })
        if ($visibleRows.Count -eq 0) {
            throw "Lucy virtual path produced no visible meshlets or indirect commands."
        }
        $invalidRows = @($virtualRows | Where-Object {
            [UInt64]::Parse($_.virtual_invalid_visibility_count) -ne 0
        })
        if ($invalidRows.Count -ne 0) {
            throw "Lucy virtual path reported malformed visibility records in $($invalidRows.Count) measured row(s)."
        }
        $overfullRows = @($virtualRows | Where-Object {
            [UInt64]::Parse($_.virtual_indirect_command_count) -gt
                [UInt64]::Parse($_.virtual_visible_meshlet_count)
        })
        if ($overfullRows.Count -ne 0) {
            throw "Lucy virtual path reported more indirect commands than visible meshlets."
        }
    }
}

$lucyCache = "${lucyPly}.halcyon.vgcache"
if (-not (Test-Path -LiteralPath $lucyCache)) {
    throw "Lucy virtual-geometry cache was not produced: $lucyCache"
}

$metrics = [System.Collections.Generic.List[object]]::new()
$reference = Join-Path $captureRoot "deferred.png"
foreach ($path in $paths) {
    $png = Join-Path $captureRoot "$($path.Name).png"
    $ssim = "skipped"
    if (Test-Path -LiteralPath $GoldenCompare) {
        $compare = & $GoldenCompare --actual $png --golden $reference --threshold $SsimThreshold 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) {
            throw "Lucy $($path.Name) failed SSIM threshold $SsimThreshold. Output: $($compare.Trim())"
        }
        $match = [regex]::Match($compare, "SSIM\s*[:=]?\s*([0-9]+(?:\.[0-9]+)?)")
        if ($match.Success) { $ssim = $match.Groups[1].Value }
    }
    $flip = "skipped"
    if ($FlipExecutable -and (Test-Path -LiteralPath $FlipExecutable)) {
        # FLIP tools differ in CLI shape; pass the two images through the
        # conventional --reference/--test interface and retain raw output.
        $flipOutput = & $FlipExecutable --reference $reference --test $png 2>&1 | Out-String
        $flip = $flipOutput.Trim()
        if ([string]::IsNullOrWhiteSpace($flip)) { $flip = "skipped" }
        Set-Content -LiteralPath (Join-Path $captureRoot "$($path.Name).flip.txt") -Value $flip
    }
    if ($RequireImageMetrics -and $ssim -eq "skipped") {
        throw "SSIM was not produced for Lucy $($path.Name); provide a working GoldenCompare executable."
    }
    if ($RequireImageMetrics -and $flip -eq "skipped") {
        throw "FLIP was not produced for Lucy $($path.Name); provide -FlipExecutable."
    }
    $metrics.Add([pscustomobject]@{ path = $path.Name; reference = "deferred"; ssim = $ssim; flip = $flip })
}
$metrics | Export-Csv -LiteralPath (Join-Path $captureRoot "image-metrics.csv") -NoTypeInformation

$manifest = [ordered]@{
    fixture = "Stanford Lucy"
    scene = "lucy"
    resolution = @{ width = $Width; height = $Height }
    frames = $runFrames
    sourceFaceRecords = $lucyFaceCount
    fixedDeltaSeconds = $FixedDt
    exposure = 0.0
    taa = $true
    paths = @("DeferredIndexed", "GpuDrivenIndexed", "VirtualGeometryIndexed")
    outputs = @("deferred.png", "gpu_driven.png", "virtual.png", "image-metrics.csv")
    executable = [System.IO.Path]::GetFullPath($Exe)
    generatedUtc = [DateTime]::UtcNow.ToString("o")
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $captureRoot "acceptance-manifest.json") -Encoding UTF8

if ($RenderDoc) {
    $renderDoc = [System.IO.Path]::GetFullPath($RenderDocExecutable)
    if (-not (Test-Path -LiteralPath $renderDoc)) { throw "RenderDoc command line tool not found: $renderDoc" }
    foreach ($path in $paths) {
        $capture = Join-Path $captureRoot "$($path.Name).rdc"
        # Capture a short deterministic window for each path. Keeping the
        # same scene/camera arguments as the screenshot run makes RenderDoc
        # events directly comparable with the CSV and image artifacts.
        $captureCommon = @("--scene", "lucy", "--width", $Width, "--height", $Height,
            "--frames", 8, "--fixed-dt", $FixedDt, "--exposure", 0.0,
            "--no-validation", "--no-vsync")
        $captureArgs = @($captureCommon + $path.Args)
        & $renderDoc capture --working-dir $root --capture-file $capture --wait-for-exit --opt-disallow-vsync `
            $Exe @captureArgs
        if ($LASTEXITCODE -ne 0) {
            throw "RenderDoc Lucy $($path.Name) capture failed with exit code $LASTEXITCODE"
        }
        if (-not (Test-Path -LiteralPath $capture)) {
            throw "RenderDoc Lucy $($path.Name) capture did not produce $capture"
        }
    }
}

Write-Host "Lucy M5 acceptance outputs: $captureRoot"
