param(
    [Parameter(Mandatory=$true)][string]$ArtifactDir,
    [Parameter(Mandatory=$true)][ValidatePattern('^[0-9a-f]{40}$')][string]$ExpectedSource
)
$ErrorActionPreference = 'Stop'
$artifact = (Resolve-Path $ArtifactDir).Path
$version = '8.2.0'
$zipName = "dinero-v$version-windows-x86_64-msvc.zip"
$zip = Get-ChildItem $artifact -Recurse -File | Where-Object Name -eq $zipName
if (@($zip).Count -ne 1) { throw 'Expected exactly one portable operator package' }
$sums = Get-ChildItem $artifact -Recurse -File | Where-Object Name -eq "SHA256SUMS-windows-x86_64-$version"
if (@($sums).Count -ne 1) { throw 'Expected checksum inventory is missing' }
foreach ($line in Get-Content $sums.FullName) {
    if ($line -notmatch '^([0-9a-f]{64})\s+\*?(.+)$') { throw 'Malformed checksum row' }
    $wanted = $Matches[1]; $name = $Matches[2]
    $files = @(Get-ChildItem $artifact -Recurse -File | Where-Object Name -eq $name)
    if ($files.Count -ne 1 -or (Get-FileHash $files[0].FullName -Algorithm SHA256).Hash.ToLowerInvariant() -ne $wanted) { throw "Checksum mismatch: $name" }
}
$owned = Join-Path $env:RUNNER_TEMP 'v820-desktop-runtime'
if (Test-Path $owned) { throw 'Refusing an existing test directory' }
New-Item $owned -ItemType Directory | Out-Null
Expand-Archive $zip.FullName -DestinationPath (Join-Path $owned 'portable')
$operator = @(Get-ChildItem (Join-Path $owned 'portable') -Recurse -File -Filter dinerod.exe)
if ($operator.Count -ne 1) { throw 'Expected one daemon in portable operator package' }
$operatorBin = Split-Path $operator[0].FullName
# The portable ZIP intentionally ships operators, not Qt. Qualify the actual
# signed GUI installer, then compare its operator bytes with that ZIP.
$setup = @(Get-ChildItem $artifact -Recurse -File | Where-Object Name -eq "Dinero-$version-windows-x86_64-Setup.exe")
if ($setup.Count -ne 1) { throw 'Expected signed desktop installer' }
$setupSignature = Get-AuthenticodeSignature $setup[0].FullName
if ($setupSignature.Status -ne 'Valid' -or $setupSignature.SignerCertificate.Subject -notmatch 'CN=Mirsad Hajdarevic(?:,|$)' -or $null -eq $setupSignature.TimeStamperCertificate) { throw 'Untrusted desktop installer signature' }
$installed = Join-Path $owned 'installed'
$process = Start-Process $setup[0].FullName -ArgumentList '/S',"/D=$installed" -Wait -PassThru
if ($process.ExitCode -ne 0) { throw 'Silent desktop installation failed' }
$qt = @(Get-ChildItem $installed -Recurse -File -Filter dinero-qt.exe)
if ($qt.Count -ne 1) { throw 'Expected one Qt executable in installed desktop package' }
$bin = Split-Path $qt[0].FullName
foreach ($name in 'dinero-qt.exe','dinerod.exe','dinero-cli.exe') {
    $file = Join-Path $bin $name
    $signature = Get-AuthenticodeSignature $file
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'CN=Mirsad Hajdarevic(?:,|$)' -or $null -eq $signature.TimeStamperCertificate) { throw "Untrusted product signature: $name" }
}
$identity = (& (Join-Path $bin 'dinerod.exe') --version) -join "`n"
if ($LASTEXITCODE -ne 0 -or $identity -notmatch [regex]::Escape($ExpectedSource)) { throw 'Portable daemon source identity differs' }
if (@(Get-Process dinerod,dinero-qt -ErrorAction SilentlyContinue).Count -ne 0) { throw 'Refusing a runner with pre-existing Dinero processes' }
$report = [ordered]@{ source=$ExpectedSource; package_checksums='PASS'; product_signatures='Valid publisher and trusted timestamp'; nvidia_driver_present=(Test-Path "$env:WINDIR\System32\nvcuda.dll"); scope='Fresh hosted Windows VM; no product source build, signing, publication or production changes' }
$gui = $null; $daemonId = $null; $data = $null
try {
    $env:QT_QPA_PLATFORM = 'windows'
    $gui = Start-Process -FilePath $qt[0].FullName -WorkingDirectory $bin -PassThru -RedirectStandardOutput (Join-Path $owned 'qt.stdout.log') -RedirectStandardError (Join-Path $owned 'qt.stderr.log')
    $deadline = (Get-Date).AddSeconds(100); $child = $null
    do {
        $gui.Refresh()
        if ($gui.HasExited) { throw "Qt exited before starting its daemon: $($gui.ExitCode)" }
        $children = @(Get-CimInstance Win32_Process -Filter "ParentProcessId=$($gui.Id)" | Where-Object Name -eq 'dinerod.exe')
        if ($children.Count -gt 1) { throw 'Qt started duplicate daemon children' }
        if ($children.Count -eq 1) { $child=$children[0]; break }
        Start-Sleep -Milliseconds 500
    } while ((Get-Date) -lt $deadline)
    if ($null -eq $child) { throw 'Qt did not launch its bundled daemon' }
    $daemonId = [int]$child.ProcessId
    if ([IO.Path]::GetFullPath($child.ExecutablePath) -ne [IO.Path]::GetFullPath((Join-Path $bin 'dinerod.exe'))) { throw 'Qt launched a different daemon executable' }
    $match = [regex]::Match($child.CommandLine, '(?:--datadir|-datadir)(?:=|\s+)(?:"([^"]+)"|(\S+))')
    if (-not $match.Success) { throw 'Bundled daemon has no explicit datadir' }
    $data = if ($match.Groups[1].Success) { $match.Groups[1].Value } else { $match.Groups[2].Value }
    $deadline = (Get-Date).AddSeconds(90); $ready=$false
    do {
        try {
            $count = & (Join-Path $bin 'dinero-cli.exe') "-datadir=$data" getblockcount 2>$null
            if ($LASTEXITCODE -eq 0 -and ($count -join '').Trim() -match '^\d+$') { $ready=$true; break }
        } catch { # Native stderr while RPC is warming up is retriable in Windows PowerShell.
        }
        $gui.Refresh(); if ($gui.HasExited) { throw 'Qt exited while waiting for its daemon' }
        Start-Sleep -Seconds 1
    } while ((Get-Date) -lt $deadline)
    if (-not $ready) { throw 'Bundled daemon did not reach authenticated block-count RPC' }
    $report['qt_native_launch']='PASS'; $report['exactly_one_bundled_daemon']='PASS'; $report['authenticated_rpc']='PASS'
} finally {
    if ($null -ne $daemonId -and (Get-Process -Id $daemonId -ErrorAction SilentlyContinue)) {
        if ($null -ne $data) { try { & (Join-Path $bin 'dinero-cli.exe') "-datadir=$data" stop *> (Join-Path $owned 'stop.log') } catch {} }
        for ($i=0; $i -lt 40 -and (Get-Process -Id $daemonId -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 500 }
        if (Get-Process -Id $daemonId -ErrorAction SilentlyContinue) { Stop-Process -Id $daemonId -Force; $report['forced_owned_daemon_cleanup']=$true }
    }
    if ($null -ne $gui -and -not $gui.HasExited) { $gui.CloseMainWindow() | Out-Null; Start-Sleep -Seconds 2; $gui.Refresh(); if (-not $gui.HasExited) { Stop-Process -Id $gui.Id -Force; $report['forced_owned_gui_cleanup']=$true } }
    $report | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $owned 'runtime-result.json') -Encoding UTF8
}
foreach ($name in 'dinerod.exe','dinero-cli.exe','dinero-miner.exe','dinero-stratum-worker.exe','dinero-gpu-miner.exe','dinero-wallet-cli.exe','dinero-seeder.exe','dinero-sv2-miner.exe','dinero-sv2-gpu-miner.exe') {
    $left=Join-Path $operatorBin $name; $right=Join-Path $bin $name
    if (-not (Test-Path $right) -or (Get-FileHash $left -Algorithm SHA256).Hash -ne (Get-FileHash $right -Algorithm SHA256).Hash) { throw "Installed versus portable byte mismatch: $name" }
}
$report['installed_vs_portable_product_bytes']='PASS'
$report['result']='PASS: signed installed Qt startup, one bundled daemon, authenticated RPC and installed-versus-portable operator parity'
$report | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $owned 'runtime-result.json') -Encoding UTF8
$report | ConvertTo-Json -Depth 8
