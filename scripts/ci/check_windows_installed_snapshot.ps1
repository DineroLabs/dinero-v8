param(
    [Parameter(Mandatory = $true)] [string]$InstallDir,
    [Parameter(Mandatory = $true)] [string]$DataDir,
    [Parameter(Mandatory = $true)] [string]$Manifest,
    [Parameter(Mandatory = $true)] [System.Diagnostics.Process]$InitialQt
)

$ErrorActionPreference = 'Stop'
$cli = Join-Path $InstallDir 'dinero-cli.exe'
$qtExe = Join-Path $InstallDir 'dinero-qt.exe'
$daemonExe = Join-Path $InstallDir 'dinerod.exe'
$manifestData = Get-Content -Raw -LiteralPath $Manifest | ConvertFrom-Json
$base = $manifestData.snapshot
if ($manifestData.network -ne 'mainnet' -or $manifestData.format_version -ne 6 -or
    $manifestData.snapshot_profile -ne 'full' -or -not $base.height -or
    $base.block_hash -notmatch '^[0-9a-f]{64}$') {
    throw 'Installed desktop manifest is missing its full-v6 mainnet base'
}

function Get-NodeRpc {
    param([string]$Method, [string[]]$Arguments = @())
    $prior = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $value = & $cli "-datadir=$DataDir" '-rpcclienttimeout=8' $Method @Arguments 2>$null
        if ($LASTEXITCODE -ne 0 -or -not $value) { return $null }
        $text = $value -join [Environment]::NewLine
        try { return ($text | ConvertFrom-Json -ErrorAction Stop) }
        catch { return $text.Trim() }
    } finally {
        $ErrorActionPreference = $prior
    }
}

function Get-EmbeddedDaemon {
    param([System.Diagnostics.Process]$Qt)
    Get-CimInstance Win32_Process -Filter "ParentProcessId=$($Qt.Id) AND Name='dinerod.exe'" |
        Where-Object { $_.ExecutablePath -eq $daemonExe } |
        Select-Object -First 1
}

function Wait-SelectedTip {
    param([System.Diagnostics.Process]$Qt, [int]$MinHeight, [bool]$ProveImport, [int]$Minutes)
    $started = Get-Date
    $deadline = $started.AddMinutes($Minutes)
    $nextReport = $started
    $sawImport = $false
    $last = $null
    do {
        $Qt.Refresh()
        if ($Qt.HasExited) { throw "Installed Qt exited during catch-up: $($Qt.ExitCode)" }
        if (-not (Get-EmbeddedDaemon $Qt)) {
            Start-Sleep -Seconds 5
            continue
        }
        $status = (Get-NodeRpc 'getsnapshotbootstrapstatus').snapshot_bootstrap
        if ($status.fatal) { throw "Installed snapshot became fatal: $($status.fatal_reason)" }
        $sawImport = $sawImport -or (
            $status.assumeutxo_active -and $status.snapshot_base_height -eq $base.height -and
            $status.snapshot_base_block -eq $base.block_hash)
        $last = Get-NodeRpc 'getblockchaininfo'
        if ((Get-Date) -ge $nextReport) {
            $elapsed = [int]((Get-Date) - $started).TotalSeconds
            Write-Host "Installed GUI progress after ${elapsed}s (blocks=$($last.blocks), headers=$($last.headers), imported=$sawImport)"
            $nextReport = (Get-Date).AddMinutes(1)
        }
        if ($last.chain -eq 'mainnet' -and $last.blocks -ge $MinHeight -and
            $last.blocks -eq $last.headers -and $last.initialblockdownload -eq $false -and
            ($sawImport -or -not $ProveImport)) {
            $baseHash = Get-NodeRpc 'getblockhash' @([string]$base.height)
            $peers = Get-NodeRpc 'getconnectioncount'
            if ($baseHash -eq $base.block_hash -and [int]$peers -gt 0) {
                return [PSCustomObject]@{ blocks = $last.blocks; headers = $last.headers;
                    hash = $last.bestblockhash; peers = $peers; imported = $sawImport;
                    elapsed_seconds = [int]((Get-Date) - $started).TotalSeconds }
            }
        }
        Start-Sleep -Seconds 5
    } while ((Get-Date) -lt $deadline)
    throw "Installed GUI did not catch up from H$($base.height) to H$MinHeight (last blocks=$($last.blocks), headers=$($last.headers), imported=$sawImport)"
}

function Stop-InstalledQt {
    param([System.Diagnostics.Process]$Qt)
    $Qt.Refresh()
    if ($Qt.HasExited) { throw "Installed Qt exited before it could close (exit=$($Qt.ExitCode))" }
    $child = Get-EmbeddedDaemon $Qt
    if (-not $child) { throw 'Installed Qt has no bundled daemon to close' }
    $daemon = [System.Diagnostics.Process]::GetProcessById($child.ProcessId)
    # Retain both process handles so Windows still exposes their exit codes.
    try {
        $null = $Qt.Handle
        $null = $daemon.Handle
        $windowDeadline = (Get-Date).AddSeconds(30)
        do {
            $Qt.Refresh()
            if ($Qt.HasExited) { throw "Installed Qt exited before close (exit=$($Qt.ExitCode))" }
            $closeRequested = $Qt.CloseMainWindow()
            if (-not $closeRequested) { Start-Sleep -Milliseconds 500 }
        } while (-not $closeRequested -and (Get-Date) -lt $windowDeadline)
        if (-not $closeRequested) {
            throw "Could not close the installed Qt window (title=$($Qt.MainWindowTitle), hwnd=$($Qt.MainWindowHandle))"
        }
        if (-not $Qt.WaitForExit(150000)) { throw 'Installed Qt did not close within 150 seconds' }
        if ($Qt.ExitCode -ne 0) { throw "Installed Qt did not exit cleanly (exit=$($Qt.ExitCode))" }
        if (-not $daemon.WaitForExit(5000)) { throw 'Installed Qt left its bundled daemon running' }
        if ($daemon.ExitCode -ne 0) { throw "Installed daemon stopped uncleanly (exit=$($daemon.ExitCode))" }
        $daemonLog = Join-Path $DataDir 'embedded-daemon.log'
        if (-not (Select-String -LiteralPath $daemonLog -SimpleMatch 'phase=shutdown_complete' -Quiet)) {
            throw 'Installed daemon did not record completed shutdown'
        }
        Write-Host 'Installed Qt and its bundled daemon exited cleanly'
    } finally {
        $daemon.Dispose()
    }
}

$qt = $InitialQt
try {
    # The first hosted run was still validating forward blocks at its original
    # 25-minute deadline. Budget for cold header indexing, import and catch-up.
    $fresh = Wait-SelectedTip -Qt $qt -MinHeight ([int]$base.height + 288) -ProveImport $true -Minutes 60
    Write-Host "Installed Qt reached H$($fresh.blocks) with $($fresh.peers) peers"
    Stop-InstalledQt $qt
    $launch = @{
        FilePath = $qtExe
        ArgumentList = ('"-datadir=' + $DataDir + '"')
        RedirectStandardOutput = "$DataDir\qt-restart-stdout.log"
        RedirectStandardError = "$DataDir\qt-restart-stderr.log"
        PassThru = $true
    }
    $qt = Start-Process @launch
    $restart = Wait-SelectedTip -Qt $qt -MinHeight $fresh.blocks -ProveImport $false -Minutes 20
    Write-Host "Restarted Qt selected H$($restart.blocks) with $($restart.peers) peers"
    Stop-InstalledQt $qt
    [ordered]@{ primary = $base.height; base_hash = $base.block_hash;
        fresh = $fresh; cold_restart = $restart } |
        ConvertTo-Json -Depth 4 | Tee-Object -FilePath 'installed-windows-snapshot-result.json'
} finally {
    if ($qt -and -not $qt.HasExited) {
        $child = Get-EmbeddedDaemon $qt
        $null = Get-NodeRpc 'stop'
        if (-not $qt.WaitForExit(15000)) { Stop-Process -Id $qt.Id -Force }
        if ($child) { Stop-Process -Id $child.ProcessId -Force -ErrorAction SilentlyContinue }
    }
}
