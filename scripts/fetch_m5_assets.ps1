param(
    [string]$Destination = "$PSScriptRoot/../out/m5-assets",
    [string]$PublishDirectory = "$PSScriptRoot/../assets/models/lucy",
    [switch]$Offline,
    [string]$ExpectedSha256 = ""
)

$ErrorActionPreference = "Stop"
# Stanford's repository serves the fixed 307 MB archive from /data. The
# digest is pinned so a mirror or partial download cannot silently enter CI.
$url = "https://graphics.stanford.edu/data/3Dscanrep/lucy.tar.gz"
$PinnedSha256 = "c4beb1f7bfa965643bbbf889bd1849a4b4b955e95c731941be61e6edac65616a"
$root = [System.IO.Path]::GetFullPath($Destination)
$archive = Join-Path $root "lucy.tar.gz"
$manifest = Join-Path $root "lucy.manifest.json"
$extract = Join-Path $root "lucy"
New-Item -ItemType Directory -Force -Path $root | Out-Null

if (-not (Test-Path $archive)) {
    if ($Offline) { Write-Host "M5 Lucy: skipped (offline and archive is missing)"; exit 0 }
    Invoke-WebRequest -Uri $url -OutFile $archive
}
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash.ToLowerInvariant()
$expected = if ($ExpectedSha256) { $ExpectedSha256.ToLowerInvariant() } else { $PinnedSha256 }
if ($hash -ne $expected) {
    throw "Lucy SHA-256 mismatch: expected $expected, got $hash"
}
if (Test-Path $extract) { Remove-Item -LiteralPath $extract -Recurse -Force }
New-Item -ItemType Directory -Force -Path $extract | Out-Null
tar -xzf $archive -C $extract
$ply = Get-ChildItem -LiteralPath $extract -Recurse -File -Filter *.ply |
    Select-Object -First 1
if ($null -eq $ply) { throw "Lucy archive did not contain a PLY source" }
$headerBuffer = New-Object byte[] (4MB)
$headerStream = [System.IO.File]::OpenRead($ply.FullName)
try { $headerLength = $headerStream.Read($headerBuffer, 0, $headerBuffer.Length) }
finally { $headerStream.Dispose() }
$headerText = [System.Text.Encoding]::ASCII.GetString($headerBuffer, 0, $headerLength)
$faceMatch = [regex]::Match($headerText, '(?m)^element\s+face\s+(\d+)\s*$')
if (-not $faceMatch.Success) { throw "Lucy PLY has no face element in its header" }
$faceCount = [UInt64]::Parse($faceMatch.Groups[1].Value,
    [Globalization.CultureInfo]::InvariantCulture)
if ($faceCount -lt 28000000) {
    throw "Lucy PLY is unexpectedly small: $faceCount face records (minimum 28000000)"
}
New-Item -ItemType Directory -Force -Path $PublishDirectory | Out-Null
Copy-Item -LiteralPath $ply.FullName -Destination (Join-Path $PublishDirectory "lucy.ply") -Force
$publishedPly = Join-Path $PublishDirectory "lucy.ply"
$plyHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $publishedPly).Hash.ToLowerInvariant()
$record = [ordered]@{
    name = "Stanford Lucy"
    version = "3D Scan Repository Lucy archive"
    source = "Stanford Computer Graphics Laboratory 3D Scan Repository"
    url = $url
    license = "Research use; free redistribution with attribution; no commercial use"
    sha256 = $hash
    archive = $archive
    extracted = $extract
    faceRecords = $faceCount
    publishedPly = [System.IO.Path]::GetFullPath($publishedPly)
    publishedPlySha256 = $plyHash
}
$record | ConvertTo-Json | Set-Content -LiteralPath $manifest -Encoding UTF8
Write-Host "Lucy ready: $extract (SHA-256 $hash)"
