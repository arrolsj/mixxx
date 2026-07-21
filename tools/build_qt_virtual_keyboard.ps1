[CmdletBinding()]
param(
    [string] $BuildEnvironmentRoot,
    [string] $CacheRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$qtVersion = '6.5.3'
$sourceArchiveName = "qtvirtualkeyboard-everywhere-src-$qtVersion.zip"
$sourceDirectoryName = "qtvirtualkeyboard-everywhere-src-$qtVersion"
$sourceUri = (
    "https://download.qt.io/official_releases/qt/6.5/$qtVersion/" +
    "submodules/$sourceArchiveName"
)
$sourceSha256 = 'FD6DB718D8E1A982EBAFBBE5B72D58AB7C0E388FDF9ACEFBA1A7C3B24CE01BF4'

$mixxxRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if ([string]::IsNullOrWhiteSpace($BuildEnvironmentRoot)) {
    $buildEnvironment = Get-ChildItem -Path (
        Join-Path $mixxxRoot 'buildenv\mixxx-deps-2.5-x64-windows-*'
    ) -Directory -ErrorAction SilentlyContinue | Where-Object {
        Test-Path -LiteralPath (
            Join-Path $_.FullName 'installed\x64-windows\share\Qt6\Qt6Config.cmake'
        ) -PathType Leaf
    } | Select-Object -First 1
    if ($null -eq $buildEnvironment) {
        throw 'The Mixxx 2.5 x64 Windows build environment was not found.'
    }
    $BuildEnvironmentRoot = $buildEnvironment.FullName
}
$BuildEnvironmentRoot = (Resolve-Path -LiteralPath $BuildEnvironmentRoot).Path
$qtRoot = Join-Path $BuildEnvironmentRoot 'installed\x64-windows'

$qtVersionFile = Join-Path $qtRoot 'share\Qt6\Qt6ConfigVersionImpl.cmake'
$qtVersionText = [System.IO.File]::ReadAllText($qtVersionFile)
$versionMatch = [regex]::Match($qtVersionText, 'set\(PACKAGE_VERSION "([^"]+)"\)')
if (-not $versionMatch.Success -or $versionMatch.Groups[1].Value -ne $qtVersion) {
    throw "Qt $qtVersion is required; the selected build environment does not match."
}

if ([string]::IsNullOrWhiteSpace($CacheRoot)) {
    $CacheRoot = Join-Path $mixxxRoot "buildenv\lastcue-qvk-$qtVersion"
}
$CacheRoot = [System.IO.Path]::GetFullPath($CacheRoot)
New-Item -ItemType Directory -Force -Path $CacheRoot | Out-Null

$archivePath = Join-Path $CacheRoot $sourceArchiveName
if (-not (Test-Path -LiteralPath $archivePath -PathType Leaf)) {
    Write-Host "Downloading $sourceUri"
    Invoke-WebRequest -Uri $sourceUri -OutFile $archivePath
}
$actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $archivePath).Hash
if ($actualHash -ne $sourceSha256) {
    throw "Qt Virtual Keyboard source SHA256 mismatch: $actualHash"
}

$sourceRoot = Join-Path $CacheRoot $sourceDirectoryName
if (-not (Test-Path -LiteralPath $sourceRoot -PathType Container)) {
    Expand-Archive -LiteralPath $archivePath -DestinationPath $CacheRoot
}
$buildRoot = Join-Path $CacheRoot 'build'
New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null

$vsToolsRoot = Get-ChildItem -Path (
    Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\2022\*\Common7\Tools'
) -Directory -ErrorAction SilentlyContinue | Where-Object {
    Test-Path -LiteralPath (Join-Path $_.FullName 'Launch-VsDevShell.ps1') -PathType Leaf
} | Select-Object -First 1
if ($null -eq $vsToolsRoot) {
    throw 'Visual Studio 2022 Build Tools with Launch-VsDevShell.ps1 is required.'
}
& (Join-Path $vsToolsRoot.FullName 'Launch-VsDevShell.ps1') `
    -Arch amd64 `
    -HostArch amd64 `
    -SkipAutomaticLocation

$vsRoot = Split-Path -Parent (Split-Path -Parent $vsToolsRoot.FullName)
$cmakePath = Join-Path $vsRoot (
    'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
)
$ninjaDirectory = Join-Path $vsRoot (
    'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
)
$qtToolsDirectory = Join-Path $qtRoot 'tools\Qt6\bin'
$qtToolchain = Join-Path $qtRoot 'share\Qt6\qt.toolchain.cmake'
foreach ($requiredPath in @($cmakePath, $ninjaDirectory, $qtToolchain)) {
    if (-not (Test-Path -LiteralPath $requiredPath)) {
        throw "Required build tool is unavailable: $requiredPath"
    }
}

$env:PATH = [string]::Join(
    [System.IO.Path]::PathSeparator,
    @(
        $qtToolsDirectory,
        (Split-Path -Parent $cmakePath),
        $ninjaDirectory,
        (Join-Path $qtRoot 'bin'),
        $env:PATH
    )
)

$languageSetupPath = Join-Path $sourceRoot 'cmake\QtVirtualKeyboardSetup.cmake'
$languageSetup = [System.IO.File]::ReadAllText($languageSetupPath)
$validLanguageBlock = [regex]::Match(
    $languageSetup,
    'set\(valid_languages(?<languages>.*?)\)',
    [System.Text.RegularExpressions.RegexOptions]::Singleline
)
if (-not $validLanguageBlock.Success) {
    throw 'Unable to read the Qt Virtual Keyboard language manifest.'
}
$disabledLanguages = [regex]::Matches(
    $validLanguageBlock.Groups['languages'].Value,
    '\b[a-z]{2}_[A-Z]{2}\b'
) | ForEach-Object Value | Where-Object { $_ -ne 'en_US' }

$cmakeArguments = @(
    '-S', $sourceRoot,
    '-B', $buildRoot,
    '-G', 'Ninja',
    "-DCMAKE_TOOLCHAIN_FILE=$qtToolchain",
    '-DCMAKE_BUILD_TYPE=Release',
    "-DCMAKE_INSTALL_PREFIX=$qtRoot",
    '-DQT_BUILD_TESTS=OFF',
    '-DQT_BUILD_EXAMPLES=OFF',
    '-DFEATURE_vkb_desktop=OFF',
    '-DFEATURE_vkb_arrow_keynavigation=OFF',
    '-DINPUT_vkb_hunspell=no',
    '-DFEATURE_hunspell=OFF',
    '-DFEATURE_3rdparty_hunspell=OFF',
    '-DFEATURE_system_hunspell=OFF',
    '-DFEATURE_openwnn=OFF',
    '-DFEATURE_pinyin=OFF',
    '-DFEATURE_tcime=OFF',
    '-DFEATURE_zhuyin=OFF',
    '-DFEATURE_cangjie=OFF',
    '-DFEATURE_hangul=OFF',
    '-DFEATURE_thai=OFF',
    '-DFEATURE_vkb_no_bundle_pinyin=ON',
    '-DFEATURE_vkb_no_bundle_tcime=ON',
    '-DFEATURE_vkb_lang_en_US=ON'
)
foreach ($language in $disabledLanguages) {
    $cmakeArguments += "-DFEATURE_vkb_lang_$language=OFF"
}

& $cmakePath @cmakeArguments
if ($LASTEXITCODE -ne 0) {
    throw "Qt Virtual Keyboard configuration failed with exit code $LASTEXITCODE."
}

& $cmakePath --build $buildRoot --parallel
if ($LASTEXITCODE -ne 0) {
    throw "Qt Virtual Keyboard build failed with exit code $LASTEXITCODE."
}
& $cmakePath --install $buildRoot
if ($LASTEXITCODE -ne 0) {
    throw "Qt Virtual Keyboard install failed with exit code $LASTEXITCODE."
}

$requiredArtifacts = @(
    (Join-Path $qtRoot 'bin\Qt6VirtualKeyboard.dll'),
    (Join-Path $qtRoot 'Qt6\qml\QtQuick\VirtualKeyboard\qmldir'),
    (Join-Path $qtRoot (
        'Qt6\plugins\platforminputcontexts\qtvirtualkeyboardplugin.dll'
    ))
)
foreach ($artifact in $requiredArtifacts) {
    if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
        throw "Qt Virtual Keyboard installed artifact is missing: $artifact"
    }
}

Write-Host "Qt Virtual Keyboard $qtVersion installed for LastCUE P1."
Write-Host "Source SHA256: $sourceSha256"
$requiredArtifacts | ForEach-Object { Write-Host "  $_" }
