[CmdletBinding()]
param(
    [string]$Project,
    [string[]]$Sources = @(),
    [string]$Output,
    [string]$SdkRoot = '',
    [string[]]$Defines = @(),
    [string[]]$IncludeDirectories = @(),
    [string[]]$LinkOptions = @(),
    [switch]$NoRuntime,
    [switch]$Clean
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (!$SdkRoot) { $SdkRoot = Join-Path $repoRoot 'external/ps2sdk' }
$builder = Join-Path $SdkRoot 'plugins/build-module.ps1'
if (!(Test-Path -LiteralPath $builder)) { throw "Update the ps2sdk submodule: module builder missing at $builder" }
$arguments = @{
    Sources = $Sources; Output = $Output; Defines = $Defines
    IncludeDirectories = @((Join-Path $repoRoot 'source/API')) + $IncludeDirectories
    LinkOptions = $LinkOptions; NoRuntime = $NoRuntime; Clean = $Clean
}
if ($Project) { $arguments.Project = $Project }
& $builder @arguments
