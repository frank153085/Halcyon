param(
    [string]$Exe = "$PSScriptRoot/../out/m6-clang/Examples/HalcyonExample03PbrScenes/HalcyonExample03PbrScenes.exe",
    [string]$OutputDirectory = "$PSScriptRoot/../out/captures/m6-lucy",
    [int]$Frames = 2100,
    [int]$Width = 1280,
    [int]$Height = 720,
    [float]$FixedDt = 0.016666,
    [switch]$SkipRuns
)

$ErrorActionPreference = "Stop"
$root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$output = [System.IO.Path]::GetFullPath((Join-Path $root $OutputDirectory))
$framesToRun = [Math]::Max(2100, $Frames)
$cache = Join-Path $root "assets/models/lucy/lucy.ply.halcyon.vgcache"
if (-not (Test-Path -LiteralPath $cache)) {
    throw "Lucy v4 cache is missing: $cache. Run HalcyonCooker first."
}
if (-not $SkipRuns -and -not (Test-Path -LiteralPath $Exe)) {
    throw "M6 acceptance executable not found: $Exe"
}
New-Item -ItemType Directory -Force -Path $output | Out-Null

$paths = @(
    @{ Name = "indexed"; Expected = "VirtualGeometryIndexed"; Args = @("--virtual-geometry") },
    @{ Name = "mesh"; Expected = "VirtualGeometryMeshShader"; Args = @("--virtual-geometry-mesh-required") }
)
$common = @("--scene", "lucy", "--frames", $framesToRun, "--width", $Width,
    "--height", $Height, "--fixed-dt", $FixedDt, "--exposure", 0.0,
    "--no-validation", "--no-vsync", "--no-taa")

if (-not $SkipRuns) {
    foreach ($path in $paths) {
        $csv = Join-Path $output "$($path.Name).csv"
        $png = Join-Path $output "$($path.Name).png"
        & $Exe @($common + $path.Args + @("--perf-csv", $csv, "--screenshot", $png))
        if ($LASTEXITCODE -ne 0) { throw "Lucy M6 $($path.Name) run failed: $LASTEXITCODE" }
    }
}

$rowsByPath = @{}
foreach ($path in $paths) {
    $csv = Join-Path $output "$($path.Name).csv"
    $png = Join-Path $output "$($path.Name).png"
    if (-not (Test-Path -LiteralPath $csv) -or -not (Test-Path -LiteralPath $png)) {
        throw "Missing M6 acceptance output for $($path.Name): $csv or $png"
    }
    $rows = @(Import-Csv -LiteralPath $csv | Where-Object { $_.frame -match '^\d+$' })
    # The application intentionally omits the 300-frame warmup from the CSV,
    # so a 2100-frame run contains exactly 1800 measured rows (300..2099).
    if ($rows.Count -lt 1800) {
        throw "$($path.Name) CSV has $($rows.Count) measured frame rows; expected at least 1800"
    }
    $lastFrame = [int](($rows | Select-Object -Last 1).frame)
    if ($lastFrame -lt ($framesToRun - 1)) {
        throw "$($path.Name) ends at frame $lastFrame; expected at least $($framesToRun - 1)"
    }
    $reported = @($rows | Select-Object -ExpandProperty render_path -Unique)
    if ($reported.Count -ne 1 -or $reported[0] -ne $path.Expected) {
        throw "$($path.Name) reported render path(s): $($reported -join ', ')"
    }
    $measured = @($rows | Where-Object { [int]$_.frame -ge 300 })
    if ($measured.Count -lt 1800) {
        throw "$($path.Name) has $($measured.Count) post-warmup rows; expected at least 1800"
    }
    $virtualRows = @($measured | Where-Object { $_.render_path -like 'VirtualGeometry*' })
    if ($virtualRows.Count -eq 0) { throw "$($path.Name) has no measured virtual-geometry rows" }
    if (@($virtualRows | Where-Object { [UInt64]$_.virtual_invalid_visibility_count -ne 0 }).Count -ne 0) {
        throw "$($path.Name) reported invalid virtual visibility records"
    }
    if (@($virtualRows | Where-Object {
        [UInt64]$_.virtual_visible_meshlet_count -eq 0 -or
        [UInt64]$_.virtual_indirect_command_count -eq 0
    }).Count -ne 0) {
        throw "$($path.Name) produced an empty virtual draw in the measured interval"
    }
    $rowsByPath[$path.Name] = $rows
}

$indexed = $rowsByPath["indexed"] | Where-Object { [int]$_.frame -ge 300 }
$mesh = $rowsByPath["mesh"] | Where-Object { [int]$_.frame -ge 300 }
foreach ($i in 0..([Math]::Min($indexed.Count, $mesh.Count) - 1)) {
    if ($indexed[$i].virtual_visible_meshlet_count -ne $mesh[$i].virtual_visible_meshlet_count -or
        $indexed[$i].virtual_selected_node_count -ne $mesh[$i].virtual_selected_node_count) {
        throw "Indexed/Mesh visible-set counters diverged at measured row $i"
    }
}
$hashes = @{}
foreach ($path in $paths) { $hashes[$path.Name] = (Get-FileHash (Join-Path $output "$($path.Name).png") -Algorithm SHA256).Hash }
if ($hashes["indexed"] -ne $hashes["mesh"]) {
    throw "Indexed and Mesh Shader screenshots differ: $($hashes["indexed"]) vs $($hashes["mesh"])"
}

$manifest = [ordered]@{
    fixture = "Stanford Lucy"
    cacheVersion = 4
    frames = $framesToRun
    warmupFrames = 300
    measurementFrames = 1800
    resolution = @{ width = $Width; height = $Height }
    fixedDeltaSeconds = $FixedDt
    paths = @("VirtualGeometryIndexed", "VirtualGeometryMeshShader")
    screenshotSha256 = $hashes
    generatedUtc = [DateTime]::UtcNow.ToString("o")
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output "acceptance-manifest.json") -Encoding UTF8
Write-Host "M6 Lucy acceptance outputs: $output"
