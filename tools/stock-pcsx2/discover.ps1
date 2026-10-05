[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$Pdb,
    [Parameter(Mandatory = $true)][string]$Version,
    [Parameter(Mandatory = $true)][string]$Output,
    [string]$Provenance,
    [string]$VisualStudioRoot
)
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
if (-not $VisualStudioRoot) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $VisualStudioRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $VisualStudioRoot) { throw 'Visual Studio C++ tools were not found.' }
}
$dia = Join-Path $VisualStudioRoot 'DIA SDK/bin/amd64/msdia140.dll'
$msbuild = Join-Path $VisualStudioRoot 'MSBuild/Current/Bin/MSBuild.exe'
if (-not (Test-Path -LiteralPath $dia)) { throw 'Visual Studio DIA SDK was not found.' }
& $msbuild (Join-Path $PSScriptRoot 'PdbQuery.vcxproj') /p:Configuration=Release /p:Platform=x64 /nologo /v:minimal
if ($LASTEXITCODE -ne 0) { throw 'DIA query helper build failed.' }
$discoveryArgs = @(
    (Join-Path $PSScriptRoot 'discover.py'), '--exe', $Executable, '--pdb', $Pdb,
    '--helper', (Join-Path $repoRoot 'build/stock-pcsx2/PdbQuery.exe'), '--dia', $dia,
    '--version', $Version, '--output', $Output,
    '--reference', (Join-Path $repoRoot 'configs/stock-pcsx2/reference-abi.json')
)
if ($Provenance) { $discoveryArgs += @('--provenance', $Provenance) }
& python @discoveryArgs
if ($LASTEXITCODE -ne 0) { throw 'Executable/PDB discovery failed.' }
