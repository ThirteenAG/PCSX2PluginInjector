[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ForkRoot,
    [string]$UpstreamRef = 'upstream/master',
    [string]$Output = (Join-Path $PSScriptRoot '../../configs/stock-pcsx2/source-target.json')
)
$ErrorActionPreference = 'Stop'
$forkPath = (Resolve-Path -LiteralPath $ForkRoot).Path
$forkRevision = & git -C $forkPath rev-parse HEAD
if ($LASTEXITCODE -ne 0) { throw 'Fork HEAD could not be read.' }
$upstreamCommit = & git -C $forkPath merge-base HEAD $UpstreamRef
if ($LASTEXITCODE -ne 0) { throw 'Upstream merge base could not be determined. Fetch upstream refs before binding.' }
[string[]]$tags = @(& git -C $forkPath tag --points-at $upstreamCommit) | Where-Object { $_ -match '^v\d+\.\d+\.\d+$' }
if ($LASTEXITCODE -ne 0 -or $tags.Count -ne 1) { throw 'Expected one published upstream version tag at the merge base.' }
$origin = & git -C $forkPath remote get-url origin
if ($LASTEXITCODE -ne 0 -or $origin -notmatch '^https://github\.com/([^/]+/[^/]+?)(?:\.git)?$') { throw 'Expected a GitHub fork origin URL.' }
$forkRepository = $Matches[1]
$target = [ordered]@{
    schema_version = 1
    fork_repository = $forkRepository
    fork_revision = $forkRevision
    upstream_commit = $upstreamCommit
    upstream_tag = $tags[0]
    policy = 'Target the original upstream sources used by the fork. Newer dev releases are discovery reports only until this source target and adapter are updated together.'
}
$json = $target | ConvertTo-Json -Depth 4
[System.IO.File]::WriteAllText([System.IO.Path]::GetFullPath($Output), $json + "`n", [System.Text.UTF8Encoding]::new($false))
Write-Host "Fork upstream source target: $($tags[0]) ($upstreamCommit). Regenerate and review the matching ABI reference after source updates."
