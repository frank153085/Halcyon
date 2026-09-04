param(
    [string]$Destination = "assets/m3"
)

$ErrorActionPreference = "Stop"
$commit = "9429648735279342b4c32b8745f7904196607379"
$rawRoot = "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/$commit"
$destinationRoot = [System.IO.Path]::GetFullPath($Destination)
New-Item -ItemType Directory -Force -Path $destinationRoot | Out-Null

function Get-Asset([string]$relativePath, [string]$outputPath) {
    $url = "$rawRoot/$relativePath"
    $parent = Split-Path -Parent $outputPath
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    if (Test-Path -LiteralPath $outputPath) {
        $item = Get-Item -LiteralPath $outputPath
        if ($item.Length -gt 0) {
            Write-Host "Keeping existing $relativePath"
            return
        }
        Remove-Item -LiteralPath $outputPath -Force
    }
    Write-Host "Downloading $relativePath"
    $temporary = "$outputPath.download"
    try {
        Invoke-WebRequest -Uri $url -OutFile $temporary -UseBasicParsing
        if (-not (Test-Path -LiteralPath $temporary) -or (Get-Item -LiteralPath $temporary).Length -eq 0) {
            throw "Download returned an empty file: $url"
        }
        Move-Item -LiteralPath $temporary -Destination $outputPath -Force
    }
    finally {
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
    }
}

$manifest = @()
function Add-Manifest([string]$relativePath, [string]$localPath) {
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $localPath).Hash.ToLowerInvariant()
    $script:manifest += [ordered]@{ path = $relativePath; sha256 = $hash }
}

$helmetRelative = "Models/DamagedHelmet/glTF-Binary/DamagedHelmet.glb"
$helmetLocal = Join-Path $destinationRoot "DamagedHelmet.glb"
Get-Asset $helmetRelative $helmetLocal
Add-Manifest $helmetRelative $helmetLocal
$helmetLicense = Join-Path $destinationRoot "DamagedHelmet-LICENSE.md"
Get-Asset "Models/DamagedHelmet/LICENSE.md" $helmetLicense
Add-Manifest "Models/DamagedHelmet/LICENSE.md" $helmetLicense

$sponzaRoot = Join-Path $destinationRoot "Sponza"
$sponzaRelative = "Models/Sponza/glTF/Sponza.gltf"
$sponzaLocal = Join-Path $sponzaRoot "Sponza.gltf"
Get-Asset $sponzaRelative $sponzaLocal
$json = Get-Content -Raw -LiteralPath $sponzaLocal | ConvertFrom-Json
$uris = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$uris.Add("Sponza.gltf") | Out-Null
$json.buffers | ForEach-Object { if ($_.uri) { [void]$uris.Add([string]$_.uri) } }
$json.images | ForEach-Object { if ($_.uri) { [void]$uris.Add([string]$_.uri) } }
foreach ($uri in $uris) {
    $decoded = [System.Uri]::UnescapeDataString($uri).Replace("\\", "/")
    if ($decoded.Contains("..") -or [System.IO.Path]::IsPathRooted($decoded)) {
        throw "Unsafe Sponza URI: $uri"
    }
    $relative = "Models/Sponza/glTF/$decoded"
    $local = Join-Path $sponzaRoot ($decoded -replace '/', [System.IO.Path]::DirectorySeparatorChar)
    if (-not (Test-Path -LiteralPath $local)) { Get-Asset $relative $local }
    Add-Manifest $relative $local
}
$sponzaLicense = Join-Path $destinationRoot "Sponza-LICENSE.md"
Get-Asset "Models/Sponza/LICENSE.md" $sponzaLicense
Add-Manifest "Models/Sponza/LICENSE.md" $sponzaLicense

$manifestPath = Join-Path $destinationRoot "manifest.json"
[ordered]@{
    source = "KhronosGroup/glTF-Sample-Assets"
    commit = $commit
    files = @($manifest)
} | ConvertTo-Json -Depth 5 | Set-Content -Encoding UTF8 -LiteralPath $manifestPath
Write-Host "M3 assets are ready under $destinationRoot"
