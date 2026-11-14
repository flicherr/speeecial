param([string]$Executable = '')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $Executable) { $Executable = Join-Path $projectRoot 'build\windows-msvc\Release\speeecial.exe' }
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$versionHeader = Get-Content -LiteralPath (Join-Path $projectRoot 'src\version.hpp') -Raw
if ($versionHeader -notmatch '#define SPEEECIAL_VERSION "([0-9]+\.[0-9]+\.[0-9]+)"') { throw 'Cannot read application version.' }
$releaseVersion = $Matches[1]
$info = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($Executable)
$embeddedVersion = '{0}.{1}.{2}' -f $info.FileMajorPart, $info.FileMinorPart, $info.FileBuildPart
if ($embeddedVersion -ne $releaseVersion -or $info.FilePrivatePart -ne 0) { throw "EXE version $embeddedVersion does not match $releaseVersion. Rebuild first." }
$stream = [System.IO.File]::OpenRead($Executable)
$reader = [System.IO.BinaryReader]::new($stream)
try {
    if ($reader.ReadUInt16() -ne 0x5A4D) { throw 'Not an executable.' }
    $stream.Position = 0x3C
    $peOffset = $reader.ReadUInt32()
    $stream.Position = $peOffset
    if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x8664) { throw 'The updater requires a Windows x64 executable.' }
} finally { $reader.Dispose(); $stream.Dispose() }
$destination = Join-Path $projectRoot "dist\$releaseVersion"
New-Item -ItemType Directory -Force -Path $destination | Out-Null
$asset = Join-Path $destination 'speeecial-windows-x64.exe'
Copy-Item -LiteralPath $Executable -Destination $asset -Force
# ZIP keeps the familiar executable name even if the input was renamed.
$zipInput = Join-Path $destination 'speeecial.exe'
Copy-Item -LiteralPath $Executable -Destination $zipInput -Force
try {
    $zip = Join-Path $destination "speeecial-$releaseVersion-windows-x64.zip"
    Compress-Archive -LiteralPath $zipInput -DestinationPath $zip -Force
} finally { Remove-Item -LiteralPath $zipInput }
$hashes = @($asset, $zip) | ForEach-Object {
    $hash = Get-FileHash -LiteralPath $_ -Algorithm SHA256
    '{0}  {1}' -f $hash.Hash.ToLowerInvariant(), (Split-Path -Leaf $_)
}
$hashes | Set-Content -LiteralPath (Join-Path $destination 'SHA256SUMS.txt') -Encoding ascii
Write-Host "Release tag: v$releaseVersion"
Write-Host "Upload assets from: $destination"
Write-Host 'Keep speeecial-windows-x64.exe as a separate release asset; ZIP alone cannot be installed by the updater.'
