# Builds the Windows side with MSVC + Ninja. Usage: scripts\build.ps1 [-Config Release|Debug] [-Test]
param(
    [ValidateSet('Release', 'Debug')] [string]$Config = 'Release',
    [switch]$Test,
    [switch]$Clean
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build\$($Config.ToLower())"

# Import the MSVC x64 environment once per shell.
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'Visual Studio Build Tools with the C++ workload not found.' }
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    cmd /c "`"$vcvars`" >nul && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
    }
}
# Prefer the CMake/Ninja shipped with Visual Studio when present.
$vsCmake = Get-ChildItem "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\*\Common7\IDE\CommonExtensions\Microsoft\CMake" -Directory -ErrorAction SilentlyContinue | Select-Object -First 1
if ($vsCmake) { $env:PATH = "$($vsCmake.FullName)\CMake\bin;$($vsCmake.FullName)\Ninja;$env:PATH" }

if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }
cmake -S $root -B $build -G Ninja "-DCMAKE_BUILD_TYPE=$Config" | Out-Host
if ($LASTEXITCODE) { throw "cmake configure failed ($LASTEXITCODE)" }
cmake --build $build | Out-Host
if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }
if ($Test) {
    ctest --test-dir $build --output-on-failure | Out-Host
    if ($LASTEXITCODE) { throw "tests failed ($LASTEXITCODE)" }
}
Write-Host "Build OK: $build"
