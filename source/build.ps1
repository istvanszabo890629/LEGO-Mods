param(
  [ValidateSet('x86', 'x64', 'All')][string]$Architecture = 'All',
  [switch]$CompileTests,
  [switch]$Test
)
$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Build Tools with Desktop C++ are required.' }
$vsInstall = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstall) { throw 'No compatible Visual Studio C++ installation found.' }
$toolsetVersion = (Get-Content -LiteralPath (Join-Path $vsInstall 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt') -Raw).Trim()
$vcRoot = Join-Path $vsInstall "VC\Tools\MSVC\$toolsetVersion"
$sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$sdkVersion = (Get-ChildItem -LiteralPath (Join-Path $sdkRoot 'Include') -Directory | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'um\Windows.h') } | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1).Name
if (-not $sdkVersion) { throw 'Windows 10/11 SDK not found.' }
if ($Test -and $Architecture -eq 'x64') { throw 'Native fixtures are x86-only; select -Architecture x86 or All.' }
$buildOriginalPath = $env:PATH
$buildOriginalInclude = $env:INCLUDE
$buildOriginalLib = $env:LIB

function Build-LegoModsArchitecture([string]$targetArchitecture) {
  $buildRoot = Join-Path $projectRoot "build\$targetArchitecture"
  New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null
  $compilerRoot = Join-Path $vcRoot "bin\Hostx64\$targetArchitecture"
  $compiler = Join-Path $compilerRoot 'cl.exe'
  $env:INCLUDE = @((Join-Path $vcRoot 'include'),(Join-Path $sdkRoot "Include\$sdkVersion\ucrt"),(Join-Path $sdkRoot "Include\$sdkVersion\shared"),(Join-Path $sdkRoot "Include\$sdkVersion\um")) -join ';'
  $env:LIB = @((Join-Path $vcRoot "lib\$targetArchitecture"),(Join-Path $sdkRoot "Lib\$sdkVersion\ucrt\$targetArchitecture"),(Join-Path $sdkRoot "Lib\$sdkVersion\um\$targetArchitecture")) -join ';'
  $env:PATH = "$compilerRoot;$buildOriginalPath"
  Push-Location $buildRoot
  try {
    $common = @('/nologo','/O2','/W4','/MT','/DUNICODE','/D_UNICODE','/DWIN32_LEAN_AND_MEAN','/DNOMINMAX','/D_WIN32_WINNT=0x0602',"/I$projectRoot\third_party\minhook\include")
    $decoder = if ($targetArchitecture -eq 'x64') { 'hde64' } else { 'hde32' }
    $cSources = @('buffer','hook','trampoline') | ForEach-Object { "$projectRoot\third_party\minhook\src\$_.c" }
    $cSources += "$projectRoot\third_party\minhook\src\hde\$decoder.c"
    $vendorOptions = $common | Where-Object { $_ -ne '/W4' }
    & $compiler @vendorOptions '/W0' '/c' @cSources
    if ($LASTEXITCODE) { throw "MinHook $targetArchitecture compilation failed." }
    $minhookObjects = @('buffer.obj','hook.obj','trampoline.obj',"$decoder.obj")
    $cppSources = @('asi','bootstrap','game_profiles','redirector') | ForEach-Object { "$projectRoot\src\$_.cpp" }
    $cppSources += "$projectRoot\src\nufile_extended.cpp"
    $cppSources += "$projectRoot\src\runtime_settings.cpp"
    if ($targetArchitecture -eq 'x64') {
      $cppSources += "$projectRoot\src\nufile64.cpp"
      $cppSources += "$projectRoot\src\nttfile64.cpp"
    } else {
      $cppSources += "$projectRoot\src\english_plus.cpp"
      $cppSources += "$projectRoot\src\nufile_redirector.cpp"
    }
    & $compiler @common '/std:c++17' '/EHsc' '/utf-8' '/LD' @cppSources @minhookObjects '/link' "/DEF:$projectRoot\src\LegoMods.def" '/OUT:LegoMods.asi' '/NOIMPLIB' '/DYNAMICBASE' '/NXCOMPAT' 'bcrypt.lib' 'user32.lib'
    if ($LASTEXITCODE) { throw "LegoMods $targetArchitecture compilation failed." }
    if ($CompileTests -or $Test) {
      & $compiler @common '/std:c++17' '/EHsc' '/utf-8' '/c' "$projectRoot\tests\hardening_tests.cpp" '/Fo:hardening_tests.obj'
      if ($LASTEXITCODE) { throw 'Hardening fixture compilation failed.' }
    }
    if ($targetArchitecture -eq 'x86' -and ($CompileTests -or $Test)) {
      & $compiler @common '/std:c++17' '/EHsc' '/utf-8' '/DBATMAN_MODS_TESTING' '/c' "$projectRoot\tests\redirector_tests.cpp" '/Fo:redirector_tests.obj'
      if ($LASTEXITCODE) { throw 'Test source compilation failed.' }
      & $compiler @common '/std:c++17' '/EHsc' '/utf-8' '/DBATMAN_MODS_TESTING' '/c' "$projectRoot\src\redirector.cpp" '/Fo:redirector_testing.obj'
      if ($LASTEXITCODE) { throw 'Test core compilation failed.' }
      & $compiler @common '/std:c++17' '/EHsc' '/utf-8' '/DBATMAN_MODS_TESTING' '/c' "$projectRoot\tests\english_plus_tests.cpp" '/Fo:english_plus_tests.obj'
      if ($LASTEXITCODE) { throw 'INI test source compilation failed.' }
      & $compiler @common '/std:c++17' '/EHsc' '/utf-8' '/DBATMAN_MODS_TESTING' '/c' "$projectRoot\src\english_plus.cpp" '/Fo:english_plus_testing.obj'
      if ($LASTEXITCODE) { throw 'INI test core compilation failed.' }
    }
    if ($targetArchitecture -eq 'x86' -and $Test) {
      & $compiler '/nologo' 'redirector_tests.obj' 'redirector_testing.obj' 'hardening_tests.obj' 'game_profiles.obj' 'runtime_settings.obj' @minhookObjects '/Fe:redirector_tests.exe' '/link' 'user32.lib'
      if ($LASTEXITCODE) { throw 'Test executable linking failed.' }
      & '.\redirector_tests.exe'
      if ($LASTEXITCODE) { throw 'Redirector tests failed.' }
      & $compiler '/nologo' 'english_plus_tests.obj' 'english_plus_testing.obj' 'game_profiles.obj' 'runtime_settings.obj' '/Fe:english_plus_tests.exe' '/link' 'user32.lib'
      if ($LASTEXITCODE) { throw 'INI test executable linking failed.' }
      & '.\english_plus_tests.exe'
      if ($LASTEXITCODE) { throw 'INI tests failed.' }
    }
    Write-Output "Built $targetArchitecture LegoMods.asi. Native test execution occurs only with -Test, on x86."
  } finally { Pop-Location }
}

try {
  if ($Architecture -eq 'All') {
    Build-LegoModsArchitecture 'x86'
    Build-LegoModsArchitecture 'x64'
  } else {
    Build-LegoModsArchitecture $Architecture
  }
} finally {
  $env:PATH = $buildOriginalPath
  $env:INCLUDE = $buildOriginalInclude
  $env:LIB = $buildOriginalLib
}
