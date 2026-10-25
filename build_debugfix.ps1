[CmdletBinding()]
param(
    [string]$QtDir = (Join-Path $PSScriptRoot '.tools/Qt/5.15.2/msvc2019_64'),
    [string]$PackageName = 'RedPanda-3.5-beta2-r5',
    [switch]$RunTests,
    [switch]$Installer,
    [switch]$PackageOnly,
    [string]$ToolchainRoot = 'D:/Program Files/RedPanda-Cpp'
)
$ErrorActionPreference = 'Stop'
$version = Get-Content "$PSScriptRoot/version.json" -Raw | ConvertFrom-Json
$appVersion = "$($version.major).$($version.minor).$($version.patch)-$($version.preRelease)"
$displayVersion = "$($version.major).$($version.minor)"
if ($version.patch -ne 0) { $displayVersion += ".$($version.patch)" }
if ($version.preRelease -match '^beta\.(\d+)$') { $displayVersion += " Beta $($Matches[1])" }
elseif ($version.preRelease) { $displayVersion += "-$($version.preRelease)" }
if ($PackageName -notmatch '^[A-Za-z0-9][A-Za-z0-9_.-]*$') { throw 'Invalid package name.' }
$qtBin = Join-Path $QtDir 'bin'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsDir = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vsDir 'VC/Auxiliary/Build/vcvars64.bat'
$toolEnvironment = & $env:ComSpec /D /C ('call "{0}" >nul && set' -f $vcvars)
if ($LASTEXITCODE -ne 0) { throw 'Cannot initialize MSVC.' }
foreach ($line in $toolEnvironment) {
    $separator = $line.IndexOf('=')
    if ($separator -gt 0) {
        [Environment]::SetEnvironmentVariable($line.Substring(0,$separator), $line.Substring($separator+1), 'Process')
    }
}
$env:PATH = "$qtBin;$env:PATH"
$env:VSLANG = '1033'
$taskTemp = Join-Path $PSScriptRoot 'build/temp-beta35'
New-Item -ItemType Directory -Path $taskTemp,(Join-Path $PSScriptRoot 'artifacts') -Force | Out-Null
$env:TEMP = $taskTemp
$env:TMP = $taskTemp
$buildDir = Join-Path $PSScriptRoot 'build/msvc-beta35'
$pkgDir = Join-Path $PSScriptRoot "dist/$PackageName"
$jom = 'D:/Qt/Tools/QtCreator/bin/jom/jom.exe'
function Check-Exit([string]$step) {
    if ($LASTEXITCODE -ne 0) { throw "$step failed (exit $LASTEXITCODE)." }
}
$buildTools = & "$PSScriptRoot/packages/msvc/prepare_build_tools.ps1" -ProjectRoot $PSScriptRoot
$cmakeExe = Join-Path $buildTools.CMakeRoot 'bin/cmake.exe'
function Copy-BuildTools([string]$Destination) {
    $toolsDir = Join-Path $Destination 'tools'
    New-Item -ItemType Directory -Path $toolsDir -Force | Out-Null
    foreach ($name in @('cmake','ninja')) {
        $source = if ($name -eq 'cmake') { $buildTools.CMakeRoot } else { $buildTools.NinjaRoot }
        & robocopy.exe $source (Join-Path $toolsDir $name) /E /R:2 /W:1 /NFL /NDL /NJH /NJS
        if ($LASTEXITCODE -ge 8) { throw "Cannot stage build tool: $name" }
    }
    Copy-Item -LiteralPath $buildTools.ManifestPath -Destination (Join-Path $toolsDir 'build-tools-manifest.json')
}
if (-not $PackageOnly) {
    & $cmakeExe -S $PSScriptRoot -B $buildDir -G 'NMake Makefiles JOM' "-DCMAKE_MAKE_PROGRAM=$jom" `
        '-DCMAKE_BUILD_TYPE=Release' "-DCMAKE_PREFIX_PATH=$QtDir" "-DCMAKE_INSTALL_PREFIX=$pkgDir" `
        '-DCMAKE_CXX_FLAGS=/utf-8 /MP8' '-DPORTABLE_CONFIG=registry' '-DDEBUGWATCH_TESTS=ON' `
        "-DREDPANDA_DISPLAY_VERSION=$displayVersion" `
        '-DCMAKE_C_FLAGS_DEBUG=/Z7 /Ob0 /Od /RTC1' '-DCMAKE_CXX_FLAGS_DEBUG=/Z7 /Ob0 /Od /RTC1'
    Check-Exit 'Configure'
    & $cmakeExe --build $buildDir --target RedPandaIDE ConsolePauser --parallel 8
    Check-Exit 'Build'
    if ($RunTests) {
        & $cmakeExe --build $buildDir --target all-test-targets --parallel 8
        Check-Exit 'Build regression tests'
        Copy-BuildTools (Join-Path $buildDir 'RedPandaIDE')
        $env:REDPANDA_TEST_MAKE = Join-Path $ToolchainRoot 'mingw64/bin/mingw32-make.exe'
        $originalCmakeHome = [Environment]::GetEnvironmentVariable('CMAKE_HOME','Process')
        Remove-Item Env:CMAKE_HOME -ErrorAction SilentlyContinue
        $env:QT_QPA_PLATFORM = 'offscreen'
        $env:QT_QPA_FONTDIR = "$env:WINDIR/Fonts"
        & "$buildDir/RedPandaIDE/test-debugwatch.exe" -o "$PSScriptRoot/artifacts/debugwatch-beta35-tests.txt,txt"
        Check-Exit 'Debugger tests'
        # Shared editor geometry tests need the product's native font backend.
        # The focused suite above already ran once and saved its detailed report.
        $env:QT_QPA_PLATFORM = 'windows'
        Remove-Item Env:QT_QPA_FONTDIR
        & (Join-Path $buildTools.CMakeRoot 'bin/ctest.exe') --test-dir $buildDir -E '^test-debugwatch$' --output-on-failure --timeout 180
        Check-Exit 'Editor and parser regressions'
        Remove-Item Env:QT_QPA_PLATFORM
        if ($null -ne $originalCmakeHome) { $env:CMAKE_HOME = $originalCmakeHome }
    }
    & $cmakeExe --install $buildDir
    Check-Exit 'Install staging'
    & "$qtBin/windeployqt.exe" --release --compiler-runtime "$pkgDir/RedPandaIDE.exe"
    Check-Exit 'Deploy Qt'
    $crtDir = Get-ChildItem "$vsDir/VC/Redist/MSVC" -Directory | Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName 'x64/Microsoft.VC143.CRT' } |
        Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $crtDir) { throw 'MSVC release runtime was not found.' }
    Copy-Item "$crtDir/*.dll" $pkgDir
    Copy-Item "$ToolchainRoot/astyle.exe" $pkgDir
}
Copy-BuildTools $pkgDir
if ($Installer) {
    # Use the upstream full-install layout: the old uninstaller removes bundled
    # compilers, so include replacement compilers in the new full installer.
    $compilerFlags = @()
    foreach ($name in @('mingw32','mingw64')) {
        if ((Test-Path "$ToolchainRoot/$name/bin/gcc.exe") -and
            (Test-Path "$ToolchainRoot/$name/bin/gdb.exe")) {
            & robocopy.exe "$ToolchainRoot/$name" "$pkgDir/$name" /E /R:2 /W:1 /NFL /NDL /NJH /NJS
            if ($LASTEXITCODE -ge 8) { throw "Cannot stage $name." }
            $compilerFlags += "/DHAVE_$($name.ToUpperInvariant())"
        } else {
            Write-Output "Skipping incomplete or absent toolchain: $name"
        }
    }
    if ($compilerFlags.Count -eq 0) { throw 'A full installer requires a complete GCC/GDB toolchain.' }
    foreach ($name in @('redpanda.nsi','lang.nsh','utils.nsh')) {
        Copy-Item "$PSScriptRoot/platform/windows/installer-scripts/$name" $pkgDir
    }
    Copy-Item "$PSScriptRoot/RedPandaIDE/images/devcpp.ico" $pkgDir
    # Upstream MinGW packages use static Qt; the MSVC build deploys shared Qt
    # and MSVC DLLs. Record their exact paths for install and uninstall.
    $runtimeFiles = @()
    $runtimeUninstall = @()
    foreach ($file in Get-ChildItem $pkgDir -File -Filter '*.dll') {
        $runtimeFiles += ('File "{0}"' -f $file.Name)
        $runtimeUninstall += ('Delete "$INSTDIR\{0}"' -f $file.Name)
    }
    foreach ($name in @('bearer','iconengines','imageformats','platforms','printsupport','styles','translations')) {
        if (Test-Path "$pkgDir/$name") {
            $runtimeFiles += ('SetOutPath "$INSTDIR\{0}"' -f $name)
            $runtimeFiles += ('File /r "{0}\*"' -f $name)
            foreach ($file in Get-ChildItem "$pkgDir/$name" -Recurse -File) {
                $relative = [IO.Path]::GetRelativePath($pkgDir, $file.FullName)
                $runtimeUninstall += ('Delete "$INSTDIR\{0}"' -f $relative)
            }
            $runtimeUninstall += ('RMDir "$INSTDIR\{0}"' -f $name)
        }
    }
    $runtimeFiles += 'SetOutPath "$INSTDIR"'
    Set-Content "$pkgDir/runtime-files.nsh" $runtimeFiles -Encoding utf8
    Set-Content "$pkgDir/runtime-uninstall.nsh" $runtimeUninstall -Encoding utf8
    $nsis = Join-Path $PSScriptRoot '.tools/nsis-3.12/makensis.exe'
    Push-Location $pkgDir
    try {
        & $nsis /V3 "/DAPP_VERSION=$appVersion" /DARCH=x64 "/DFINALNAME=$PSScriptRoot/dist/$PackageName-Setup.exe" `
            "/DAPP_DISPLAY_VERSION=$displayVersion" `
            '/DMINGW32_COMPILER_NAME=MinGW-w64 GCC 11.5 (32-bit)' '/DMINGW64_COMPILER_NAME=MinGW-w64 GCC 11.5 (64-bit)' `
            @compilerFlags /DHAVE_SHARED_RUNTIME /DHAVE_BUILD_TOOLS /DREQUIRED_WINDOWS_BUILD=7601 `
            '/DREQUIRED_WINDOWS_NAME=Windows 7 SP1' redpanda.nsi
        Check-Exit 'NSIS installer'
    } finally { Pop-Location }
}
Write-Output "Package: $pkgDir"
