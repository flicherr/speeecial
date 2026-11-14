param(
    [switch]$Ninja,
    [switch]$Run,
    [switch]$Fresh
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$configurePreset = if ($Ninja) { 'ninja-release' } else { 'windows-msvc' }
$buildPreset = if ($Ninja) { 'ninja-release' } else { 'windows-release' }

Push-Location $projectRoot
try {
    if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
        throw 'CMake 3.24+ was not found in PATH.'
    }
    if ($Fresh) { & cmake --fresh --preset $configurePreset }
    else { & cmake --preset $configurePreset }
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
    if ($Fresh) { & cmake --build --preset $buildPreset --clean-first }
    else { & cmake --build --preset $buildPreset }
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
    & ctest --preset $buildPreset
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
    $relativeExe = if ($Ninja) { 'build\ninja-release\speeecial.exe' } else { 'build\windows-msvc\Release\speeecial.exe' }
    $executable = Join-Path $projectRoot $relativeExe
    Write-Host "Built: $executable"
    if ($Run) { Start-Process -FilePath $executable }
}
finally {
    Pop-Location
}
