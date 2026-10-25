[CmdletBinding()]
param(
    [string]$ProjectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
)
$ErrorActionPreference = 'Stop'
$manifestPath = Join-Path $ProjectRoot 'platform/windows/build-tools.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$downloads = Join-Path $ProjectRoot '.tools/build-tools-downloads'
$cache = Join-Path $ProjectRoot '.tools/build-tools'
New-Item -ItemType Directory -Path $downloads,$cache -Force | Out-Null

function Get-VerifiedDownload([string]$Name, [string]$Url, [string]$Sha256) {
    if ($Name -ne [IO.Path]::GetFileName($Name) -or $Sha256 -notmatch '^[0-9a-f]{64}$') {
        throw 'Invalid build-tool manifest entry.'
    }
    $path = Join-Path $downloads $Name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        Invoke-WebRequest -Uri $Url -OutFile $path
    }
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $Sha256) {
        throw "Build-tool checksum mismatch: $path"
    }
    return $path
}

$cmakeArchive = Get-VerifiedDownload $manifest.cmake.archive $manifest.cmake.url $manifest.cmake.sha256
$ninjaArchive = Get-VerifiedDownload $manifest.ninja.archive $manifest.ninja.url $manifest.ninja.sha256
$ninjaLicense = Get-VerifiedDownload $manifest.ninja.licenseArchive $manifest.ninja.licenseUrl $manifest.ninja.licenseSha256
$cmakeDestination = Join-Path $cache "cmake-$($manifest.cmake.version)"
$ninjaDestination = Join-Path $cache "ninja-$($manifest.ninja.version)"
$cmakeRoot = Join-Path $cmakeDestination $manifest.cmake.root
if (-not (Test-Path -LiteralPath (Join-Path $cmakeRoot 'bin/cmake.exe'))) {
    Expand-Archive -LiteralPath $cmakeArchive -DestinationPath $cmakeDestination -Force
}
if (-not (Test-Path -LiteralPath (Join-Path $ninjaDestination 'ninja.exe'))) {
    Expand-Archive -LiteralPath $ninjaArchive -DestinationPath $ninjaDestination -Force
}
Copy-Item -LiteralPath $ninjaLicense -Destination (Join-Path $ninjaDestination 'LICENSE.txt')
foreach ($relative in @('bin/cmake.exe','bin/ctest.exe',$manifest.cmake.module,$manifest.cmake.license)) {
    if (-not (Test-Path -LiteralPath (Join-Path $cmakeRoot $relative) -PathType Leaf)) {
        throw "Incomplete official CMake package: $relative"
    }
}
[PSCustomObject]@{
    CMakeRoot = $cmakeRoot
    NinjaRoot = $ninjaDestination
    ManifestPath = $manifestPath
}
