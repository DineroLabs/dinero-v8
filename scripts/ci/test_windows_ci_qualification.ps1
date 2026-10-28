# Execute the installer entry point, refusing qualification outside CI versions.
$ErrorActionPreference = 'Stop'
$builder = Join-Path $PSScriptRoot '..\..\packaging\windows\build-server-installer.ps1'
$original = $env:GITHUB_ACTIONS
try {
    foreach ($case in @(
        @{ Environment = 'true'; Version = '8.2.0'; Name = 'normal release version' },
        @{ Environment = 'false'; Version = '8.2.0-ci'; Name = 'non-CI caller' }
    )) {
        $env:GITHUB_ACTIONS = $case.Environment
        $refused = $false
        try {
            & $builder -Version $case.Version -CiOrchardQualification -SkipBuild
        } catch {
            if ($_.Exception.Message -notlike '*restricted to GitHub CI smoke versions ending in -ci*') {
                throw
            }
            $refused = $true
        }
        if (-not $refused) { throw "Qualification accepted $($case.Name)" }
        Write-Host "PASS: qualification refuses $($case.Name)"
    }
} finally {
    $env:GITHUB_ACTIONS = $original
}
