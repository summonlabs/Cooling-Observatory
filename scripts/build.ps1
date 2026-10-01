<#
.SYNOPSIS
  Configure and build the Cooling Observatory with the MSVC toolchain.

.DESCRIPTION
  A CMake configure needs the MSVC environment (INCLUDE, LIB, PATH) to be in
  place, and on a machine without a Developer Command Prompt that means finding
  the toolchain first. This script locates the compiler with vswhere, sets the
  environment for this process only, and then runs CMake and the build.

  It sets no persistent state: no registry writes, no user environment changes,
  no files outside the build directory it is given.

.PARAMETER BuildType
  Release or Debug.

.PARAMETER BuildDirectory
  Where to configure. Defaults to build/<lower-case build type>.

.PARAMETER Generator
  Ninja (single configuration, the default) or a Visual Studio generator.

.PARAMETER Asan
  Build with AddressSanitizer. Requires the Debug or Release configuration and
  the MSVC runtime shipped with the toolset.

.PARAMETER Analyze
  Run the MSVC static analyser over first-party targets.

.PARAMETER Target
  Build one target instead of everything.

.EXAMPLE
  pwsh -File scripts/build.ps1 -BuildType Release
.EXAMPLE
  pwsh -File scripts/build.ps1 -BuildType Debug -Asan -Target unit_foundations
#>
[CmdletBinding()]
param(
  [ValidateSet('Release', 'Debug')]
  [string]$BuildType = 'Release',
  [string]$BuildDirectory = '',
  [string]$Generator = 'Ninja',
  [switch]$Asan,
  [switch]$Analyze,
  [string]$Target = '',
  [switch]$SkipConfigure,
  [switch]$Test,
  [string]$InstallPrefix = ''
)

$ErrorActionPreference = 'Stop'

function Find-VsInstall {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (Test-Path $vswhere) {
    # -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 keeps a build
    # tool install without the C++ workload from being selected.
    $path = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($path) { return $path.Trim() }
  }
  foreach ($candidate in @(
      'C:\Program Files\Microsoft Visual Studio\2022\Community',
      'C:\Program Files\Microsoft Visual Studio\2022\Professional',
      'C:\Program Files\Microsoft Visual Studio\2022\Enterprise',
      'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools')) {
    if (Test-Path (Join-Path $candidate 'VC\Tools\MSVC')) { return $candidate }
  }
  throw 'No Visual Studio installation with the C++ toolset was found.'
}

function Set-MsvcEnvironment([string]$vsPath) {
  $msvcRoot = Join-Path $vsPath 'VC\Tools\MSVC'
  $toolset = Get-ChildItem $msvcRoot -Directory | Sort-Object Name | Select-Object -Last 1
  if (-not $toolset) { throw "No MSVC toolset under $msvcRoot" }

  $kitsInclude = 'C:\Program Files (x86)\Windows Kits\10\Include'
  $kitsLib = 'C:\Program Files (x86)\Windows Kits\10\Lib'
  $kitsBin = 'C:\Program Files (x86)\Windows Kits\10\bin'
  $sdkInclude = Get-ChildItem $kitsInclude -Directory -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1
  $sdkLib = Get-ChildItem $kitsLib -Directory -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1
  $sdkBin = Get-ChildItem $kitsBin -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -like '10.*' } | Sort-Object Name | Select-Object -Last 1
  if (-not $sdkInclude -or -not $sdkLib) { throw 'No Windows SDK was found.' }

  $binRoot = Join-Path $toolset.FullName 'bin\Hostx64\x64'
  $env:INCLUDE = @(
    (Join-Path $toolset.FullName 'include'),
    (Join-Path $sdkInclude.FullName 'ucrt'),
    (Join-Path $sdkInclude.FullName 'um'),
    (Join-Path $sdkInclude.FullName 'shared'),
    (Join-Path $sdkInclude.FullName 'winrt'),
    (Join-Path $sdkInclude.FullName 'cppwinrt')) -join ';'
  $env:LIB = @(
    (Join-Path $toolset.FullName 'lib\x64'),
    (Join-Path $sdkLib.FullName 'ucrt\x64'),
    (Join-Path $sdkLib.FullName 'um\x64')) -join ';'
  $pathParts = @($binRoot)
  if ($sdkBin) { $pathParts += (Join-Path $sdkBin.FullName 'x64') }
  $pathParts += $env:PATH
  $env:PATH = $pathParts -join ';'
  return (Join-Path $binRoot 'cl.exe')
}

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $BuildDirectory) {
  $BuildDirectory = Join-Path $repoRoot ('build/' + $BuildType.ToLowerInvariant())
} elseif (-not [System.IO.Path]::IsPathRooted($BuildDirectory)) {
  $BuildDirectory = Join-Path $repoRoot $BuildDirectory
}

$vsPath = Find-VsInstall
$clPath = Set-MsvcEnvironment $vsPath
Write-Host "toolchain: $clPath"
Write-Host "build dir: $BuildDirectory"
Write-Host "generator: $Generator"

$configureArgs = @(
  '-S', $repoRoot,
  '-B', $BuildDirectory,
  "-DCMAKE_BUILD_TYPE=$BuildType",
  "-DCMAKE_C_COMPILER=$clPath",
  "-DCMAKE_CXX_COMPILER=$clPath",
  '-DCOOLING_OBSERVATORY_WARNINGS_AS_ERRORS=ON'
)
if ($Generator -eq 'Ninja') {
  $configureArgs += @('-G', 'Ninja')
} else {
  $configureArgs += @('-G', $Generator, '-A', 'x64')
}
if ($Asan) { $configureArgs += '-DCOOLING_OBSERVATORY_ENABLE_ASAN=ON' }
if ($Analyze) { $configureArgs += '-DCOOLING_OBSERVATORY_ENABLE_ANALYZE=ON' }

if (-not $SkipConfigure) {
  & cmake @configureArgs
  if ($LASTEXITCODE -ne 0) { throw "cmake configure failed with exit code $LASTEXITCODE" }
}

$buildArgs = @('--build', $BuildDirectory, '--config', $BuildType)
if ($Target) { $buildArgs += @('--target', $Target) }
& cmake @buildArgs
if ($LASTEXITCODE -ne 0) { throw "cmake build failed with exit code $LASTEXITCODE" }

Write-Host 'build completed'

# Running the tests from inside this process matters for one reason: a
# sanitizer-instrumented build needs the sanitizer runtime on PATH, and that
# directory is part of the environment this script prepared for itself.
if ($Test) {
  & ctest --test-dir $BuildDirectory --output-on-failure
  if ($LASTEXITCODE -ne 0) { throw "ctest failed with exit code $LASTEXITCODE" }
  Write-Host 'tests passed'
}

if ($InstallPrefix) {
  & cmake --install $BuildDirectory --config $BuildType --prefix $InstallPrefix
  if ($LASTEXITCODE -ne 0) { throw "cmake install failed with exit code $LASTEXITCODE" }
  Write-Host "installed to $InstallPrefix"
}
