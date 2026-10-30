# Check the resolved cache, including when packaging an existing -SkipBuild tree.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [ValidateSet('Release', 'Qualification')][string]$Mode = 'Release'
)
$ErrorActionPreference = 'Stop'
$cache = Join-Path $BuildDir 'CMakeCache.txt'
if (-not (Test-Path $cache -PathType Leaf)) {
    throw "Missing Orchard build-mode cache: $cache"
}
$expected = [ordered]@{
    DINERO_BUILD_ORCHARD_BACKEND = 'ON'
    DINERO_ORCHARD_WINDOWS_RELEASE = $(if ($Mode -eq 'Release') { 'ON' } else { 'OFF' })
    DINERO_ORCHARD_WINDOWS_QUALIFICATION = $(if ($Mode -eq 'Qualification') { 'ON' } else { 'OFF' })
}
$lines = @(Get-Content $cache)
foreach ($entry in $expected.GetEnumerator()) {
    $prefix = $entry.Key + ':'
    $entries = @($lines | Where-Object { $_.StartsWith($prefix, [StringComparison]::Ordinal) })
    $required = $entry.Key + ':BOOL=' + $entry.Value
    if ($entries.Count -ne 1 -or $entries[0] -cne $required) {
        throw "Orchard $Mode build requires exactly $required in $cache"
    }
}
Write-Host "PASS: Orchard $Mode build mode ($cache)"
