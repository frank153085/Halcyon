param(
    [string]$BuildDir = "out/build/m3-msvc-debug",
    [ValidateSet("Debug", "RelWithDebInfo")]
    [string]$BuildType = "Debug",
    [switch]$Build,
    [switch]$FetchAssets
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..\")).Path
$buildPath = [System.IO.Path]::GetFullPath((Join-Path $repo $BuildDir))
New-Item -ItemType Directory -Force -Path $buildPath | Out-Null

# A previous MinGW configure can leave CMAKE_LINKER pointing at ld.exe.  Do
# not reuse that cache for an MSVC build: remove only CMake's generated cache
# files in the explicitly requested build directory.
$cache = Join-Path $buildPath "CMakeCache.txt"
if (Test-Path -LiteralPath $cache) {
    $staleLinker = Select-String -LiteralPath $cache -Pattern "CMAKE_LINKER:FILEPATH=.*(?:mingw|/|\\)ld\.exe" -Quiet
    if ($staleLinker) {
        Write-Warning "Removing stale MinGW linker cache from $buildPath"
        Remove-Item -LiteralPath $cache -Force
        $generated = Join-Path $buildPath "CMakeFiles"
        if (Test-Path -LiteralPath $generated) { Remove-Item -LiteralPath $generated -Recurse -Force }
    }
}

$vsDevCmd = @(
    "D:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat",
    "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"
) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $vsDevCmd) { throw "VsDevCmd.bat was not found; install the MSVC C++ workload." }

if ($FetchAssets) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repo "scripts/fetch_m3_assets.ps1") `
        -Destination (Join-Path $repo "assets/m3")
}

$source = $repo.TrimEnd('\')
$binary = $buildPath.TrimEnd('\')
$type = $BuildType
# The repository paths are intentionally kept free of spaces in the supported
# layout.  Avoid nested quotes here because cmd.exe consumes the first quoted
# argument after /c as the complete command string.
$cmakeCommand = "cmake -S $source -B $binary -G Ninja -DCMAKE_BUILD_TYPE=$type -DHALCYON_BUILD_TESTS=ON -DHALCYON_BUILD_EXPERIMENTAL_M2=ON"
Write-Host $cmakeCommand
$commandLine = "/c call `"$vsDevCmd`" -arch=x64 && set VSLANG=1033&& $cmakeCommand"
& cmd.exe $commandLine
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# CMake 4.4 decodes /showIncludes using the host locale before writing
# CMakeFiles/rules.ninja. On this machine that produces a mojibake prefix,
# while cl writes UTF-8 to Ninja's pipe. Repair the generated rule at the byte
# level so incremental builds remain ABI-safe.
$rawPrefix = [byte[]](0xE6, 0xB3, 0xA8, 0xE6, 0x84, 0x8F, 0x3A, 0x20,
    0xE5, 0x8C, 0x85, 0xE5, 0x90, 0xAB, 0xE6, 0x96, 0x87, 0xE4,
    0xBB, 0xB6, 0x3A, 0x20, 0x20)
$marker = [byte[]][Text.Encoding]::ASCII.GetBytes("msvc_deps_prefix = ")
$rulesPath = Join-Path $buildPath "CMakeFiles\rules.ninja"
if (Test-Path -LiteralPath $rulesPath) {
    $rulesBytes = [IO.File]::ReadAllBytes($rulesPath)
    $markerStart = -1
    for ($i = 0; $i -le $rulesBytes.Length - $marker.Length; $i++) {
        $match = $true
        for ($j = 0; $j -lt $marker.Length; $j++) {
            if ($rulesBytes[$i + $j] -ne $marker[$j]) { $match = $false; break }
        }
        if ($match) { $markerStart = $i; break }
    }
    if ($markerStart -ge 0) {
        $lineStart = $markerStart + $marker.Length
        $lineEnd = $lineStart
        while ($lineEnd -lt $rulesBytes.Length -and $rulesBytes[$lineEnd] -ne 0x0A -and
            $rulesBytes[$lineEnd] -ne 0x0D) { $lineEnd++ }
        $fixed = New-Object 'System.Collections.Generic.List[byte]'
        if ($lineStart -gt 0) { $fixed.AddRange([byte[]]$rulesBytes[0..($lineStart - 1)]) }
        $fixed.AddRange($rawPrefix)
        if ($lineEnd -lt $rulesBytes.Length) { $fixed.AddRange([byte[]]$rulesBytes[$lineEnd..($rulesBytes.Length - 1)]) }
        [IO.File]::WriteAllBytes($rulesPath, $fixed.ToArray())
    }
}

if ($Build) {
    $buildCommand = "cmake --build $binary --config $type"
    Write-Host $buildCommand
    $commandLine = "/c call `"$vsDevCmd`" -arch=x64 && set VSLANG=1033&& $buildCommand"
    & cmd.exe $commandLine
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
