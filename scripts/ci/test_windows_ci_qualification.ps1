# Exercise the real installer entrypoint without building or staging artifacts.
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
    # Capture the actual configure invocation before CMake or any packaging runs.
    # A cache containing qualification=ON must be explicitly cleared by release.
    $captured = New-Object 'System.Collections.Generic.List[object]'
    function Capture-OrchardConfigure {
        $captured.Add([string[]]$args)
        throw 'ORCHARD_CONFIGURE_CAPTURED'
    }
    foreach ($case in @(
        @{ Qualification = $false; Version = '8.2.0'; Release = 'ON'; Ci = 'OFF' },
        @{ Qualification = $true; Version = '8.2.0-ci'; Release = 'OFF'; Ci = 'ON' }
    )) {
        $env:GITHUB_ACTIONS = 'true'
        $captured.Clear()
        try {
            & $builder -Version $case.Version -CiOrchardQualification:$case.Qualification `
                -CMake 'Capture-OrchardConfigure' -BuildDir 'build-mode-contract-unused'
            throw 'Installer did not attempt configure'
        } catch {
            if ($_.Exception.Message -ne 'ORCHARD_CONFIGURE_CAPTURED') { throw }
        }
        if ($captured.Count -ne 1) { throw 'Expected exactly one configure invocation' }
        foreach ($required in @(
            '-DDINERO_BUILD_ORCHARD_BACKEND=ON',
            "-DDINERO_ORCHARD_WINDOWS_RELEASE=$($case.Release)",
            "-DDINERO_ORCHARD_WINDOWS_QUALIFICATION=$($case.Ci)"
        )) {
            if (@($captured[0] | Where-Object { $_ -ceq $required }).Count -ne 1) {
                throw "Configure did not explicitly select exactly $required"
            }
        }
        Write-Host "PASS: actual installer configure separates release=$($case.Release) qualification=$($case.Ci)"
    }

    $modeCheck = Join-Path $PSScriptRoot '..\..\packaging\windows\assert-orchard-build-mode.ps1'
    $temp = Join-Path ([IO.Path]::GetTempPath()) ('orchard-mode-contract-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $temp | Out-Null
    try {
        $cache = Join-Path $temp 'CMakeCache.txt'
        $releaseLines = @('DINERO_BUILD_ORCHARD_BACKEND:BOOL=ON',
            'DINERO_ORCHARD_WINDOWS_RELEASE:BOOL=ON', 'DINERO_ORCHARD_WINDOWS_QUALIFICATION:BOOL=OFF')
        Set-Content -Path $cache -Value $releaseLines
        & $modeCheck -BuildDir $temp
        $ciLines = @('DINERO_BUILD_ORCHARD_BACKEND:BOOL=ON',
            'DINERO_ORCHARD_WINDOWS_RELEASE:BOOL=OFF', 'DINERO_ORCHARD_WINDOWS_QUALIFICATION:BOOL=ON')
        Set-Content -Path $cache -Value $ciLines
        & $modeCheck -BuildDir $temp -Mode Qualification
        $refused = $false
        try { & $builder -Version '8.2.0' -BuildDir $temp -SkipBuild } catch {
            if ($_.Exception.Message -notlike 'Orchard Release build requires exactly*') { throw }
            $refused = $true
        }
        if (-not $refused) { throw 'SkipBuild promoted a CI-only tree into a production package' }
        Write-Host 'PASS: actual SkipBuild entrypoint refuses qualification-only artifacts'
        foreach ($lines in @(
            ,$ciLines,
            ,@('DINERO_BUILD_ORCHARD_BACKEND:BOOL=ON', 'DINERO_ORCHARD_WINDOWS_QUALIFICATION:BOOL=OFF'),
            ,($releaseLines + 'DINERO_ORCHARD_WINDOWS_RELEASE:BOOL=ON'),
            ,@('DINERO_BUILD_ORCHARD_BACKEND:BOOL=OFF', 'DINERO_ORCHARD_WINDOWS_RELEASE:BOOL=ON', 'DINERO_ORCHARD_WINDOWS_QUALIFICATION:BOOL=OFF')
        )) {
            Set-Content -Path $cache -Value $lines
            $refused = $false
            try { & $modeCheck -BuildDir $temp } catch { $refused = $true }
            if (-not $refused) { throw 'Accepted a missing, duplicate, disabled, or qualification-only production cache' }
        }
        Remove-Item $cache
        $refused = $false
        try { & $modeCheck -BuildDir $temp } catch { $refused = $true }
        if (-not $refused) { throw 'Accepted missing production cache' }
        Write-Host 'PASS: production staging refuses stale, missing, duplicate, or disabled mode receipts'
    } finally {
        Remove-Item $temp -Recurse -Force
    }
} finally {
    $env:GITHUB_ACTIONS = $original
}
