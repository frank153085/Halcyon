param(
    [string]$Destination = "$PSScriptRoot/../out/m5-assets",
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
$record = [ordered]@{
    name = "Stanford Lucy"
    source = "Stanford Computer Graphics Laboratory 3D Scan Repository"
    url = $url
    license = "Research use; free redistribution with attribution; no commercial use"
    sha256 = $hash
    archive = $archive
    extracted = $extract
}
$record | ConvertTo-Json | Set-Content -LiteralPath $manifest -Encoding UTF8
Write-Host "Lucy ready: $extract (SHA-256 $hash)"
