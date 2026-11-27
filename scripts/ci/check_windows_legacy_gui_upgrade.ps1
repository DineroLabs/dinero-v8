param(
    [Parameter(Mandatory = $true)] [string]$InstallDir,
    [Parameter(Mandatory = $true)] [string]$Fixture
)

$ErrorActionPreference = 'Stop'
$root = Join-Path $env:RUNNER_TEMP 'dinero-legacy-gui-smoke'
$appData = Join-Path $root 'Roaming'
$oldDir = Join-Path $appData 'Dinero'
New-Item -ItemType Directory -Force -Path $oldDir | Out-Null

# Reproduce a no-wallet Windows installation that kept its H99,677 node data.
& $Fixture $oldDir 99677
if ($LASTEXITCODE -ne 0) { throw 'Could not prepare the old Windows data folder' }
$oldIndex = Join-Path $oldDir 'blockchain\utxo'
$before = (Get-FileHash -LiteralPath $oldIndex -Algorithm SHA256).Hash

$priorAppData = $env:APPDATA
$qt = $null
try {
    $env:APPDATA = $appData
    $qt = Start-Process -FilePath (Join-Path $InstallDir 'dinero-qt.exe') `
        -RedirectStandardOutput (Join-Path $root 'qt-stdout.log') `
        -RedirectStandardError (Join-Path $root 'qt-stderr.log') -PassThru

    Add-Type -AssemblyName UIAutomationClient
    Add-Type -AssemblyName UIAutomationTypes
    $processCondition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $qt.Id)
    $dialog = $null
    $deadline = (Get-Date).AddSeconds(90)
    do {
        $qt.Refresh()
        if ($qt.HasExited) { throw "Installed Qt exited before the old-data prompt (exit=$($qt.ExitCode))" }
        $windows = [System.Windows.Automation.AutomationElement]::RootElement.FindAll(
            [System.Windows.Automation.TreeScope]::Children, $processCondition)
        foreach ($window in $windows) {
            if ($window.Current.Name -eq 'Update Dinero wallet data') {
                $dialog = $window
                break
            }
        }
        if (-not $dialog) { Start-Sleep -Milliseconds 500 }
    } while (-not $dialog -and (Get-Date) -lt $deadline)
    if (-not $dialog) {
        @($windows | ForEach-Object { $_.Current.Name }) | Write-Host
        throw 'Installed Qt did not show the old-snapshot recovery prompt'
    }

    $button = $null
    $elements = $dialog.FindAll([System.Windows.Automation.TreeScope]::Descendants,
        [System.Windows.Automation.Condition]::TrueCondition)
    foreach ($element in $elements) {
        if ($element.Current.Name -eq 'Start with current data' -and
            $element.Current.ControlType -eq [System.Windows.Automation.ControlType]::Button) {
            $button = $element
            break
        }
    }
    if (-not $button) {
        @($elements | Select-Object -First 50 | ForEach-Object { $_.Current.Name }) | Write-Host
        throw 'Old-snapshot prompt did not offer a separate current-data folder'
    }
    $invoke = $null
    if (-not $button.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern,
        [ref]$invoke)) {
        throw 'Old-snapshot recovery button is not invokable'
    }
    $invoke.Invoke()

    $newDir = $null
    $child = $null
    $reportedDir = $null
    $rpcFailure = $null
    $deadline = (Get-Date).AddSeconds(120)
    do {
        $qt.Refresh()
        if ($qt.HasExited) { throw "Installed Qt exited after selecting current data (exit=$($qt.ExitCode))" }
        $newDirs = @(Get-ChildItem -LiteralPath $appData -Directory -Filter 'Dinero-current-*')
        if ($newDirs.Count -gt 1) { throw 'Recovery created multiple current-data folders' }
        if ($newDirs.Count -eq 1) { $newDir = $newDirs[0].FullName }
        $child = Get-CimInstance Win32_Process -Filter "ParentProcessId=$($qt.Id) AND Name='dinerod.exe'" |
            Select-Object -First 1
        # Qt may use forward slashes in its Windows command line. Ask the
        # authenticated daemon which data folder it actually opened.
        if ($newDir -and $child) {
            $cookiePath = Join-Path $newDir '.cookie'
            if (Test-Path -LiteralPath $cookiePath) {
                try {
                    $cookie = (Get-Content -Raw -LiteralPath $cookiePath).Trim()
                    $auth = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($cookie))
                    $reply = Invoke-RestMethod -Uri 'http://127.0.0.1:20998/' -Method Post `
                        -Headers @{ Authorization = "Basic $auth" } -ContentType 'application/json' `
                        -Body '{"jsonrpc":"2.0","id":"legacy-gui-gate","method":"rpc.getcontext","params":{}}' `
                        -TimeoutSec 5
                    if (-not $reply.error -and $reply.result.datadir) {
                        $reportedDir = [System.IO.Path]::GetFullPath([string]$reply.result.datadir)
                        if ($reportedDir -ieq [System.IO.Path]::GetFullPath($newDir)) { break }
                    }
                } catch {
                    $rpcFailure = $_.Exception.GetType().Name
                }
            }
        }
        Start-Sleep -Seconds 1
    } while ((Get-Date) -lt $deadline)
    $expectedDir = if ($newDir) { [System.IO.Path]::GetFullPath($newDir) } else { $null }
    if (-not $expectedDir -or -not $child -or $reportedDir -ine $expectedDir) {
        throw "Installed Qt did not use the separate folder (new=$newDir, rpc=$reportedDir, child=$($child.ProcessId), command=$($child.CommandLine), rpc_failure=$rpcFailure)"
    }
    if ((Get-FileHash -LiteralPath $oldIndex -Algorithm SHA256).Hash -ne $before) {
        throw 'Old snapshot index changed during GUI recovery'
    }
    Write-Host 'Installed Qt offered current data for empty H99677; daemon used the separate folder and old index stayed unchanged'
} finally {
    $env:APPDATA = $priorAppData
    if ($qt) {
        Get-CimInstance Win32_Process -Filter "ParentProcessId=$($qt.Id) AND Name='dinerod.exe'" |
            ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
        $qt.Refresh()
        if (-not $qt.HasExited) { Stop-Process -Id $qt.Id -Force -ErrorAction SilentlyContinue }
    }
}
