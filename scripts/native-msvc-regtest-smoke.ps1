param(
  [string]$BuildDir = "build-msvc-native\Release",
  [string]$DataDir = "",
  [int]$TimeoutSeconds = 60,
  [switch]$KeepDataDir
)

$ErrorActionPreference = "Stop"

function Resolve-RepoPath([string]$Path) {
  if ([System.IO.Path]::IsPathRooted($Path)) {
    return $Path
  }
  return (Join-Path (Get-Location) $Path)
}

function Invoke-DineroCli {
  param(
    [Parameter(Mandatory = $true)][string[]]$RpcArgs,
    [switch]$AllowFail
  )

  $args = @(
    "-datadir=$script:SmokeDataDir",
    "-rpcport=$script:RpcPort"
  ) + $RpcArgs

  $oldErrorActionPreference = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  try {
    $output = & $script:Cli @args 2>&1
    $exit = $LASTEXITCODE
  } finally {
    $ErrorActionPreference = $oldErrorActionPreference
  }
  $text = ($output | Out-String).Trim()
  if ($exit -ne 0 -and -not $AllowFail) {
    throw "dinero-cli $($RpcArgs -join ' ') failed with exit $exit`n$text"
  }
  return $text
}

function Wait-ForRpc {
  param([string]$Phase)

  $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
  do {
    if ($script:NodeProcess.HasExited) {
      $stdout = if (Test-Path $script:StdoutLog) { Get-Content $script:StdoutLog -Raw } else { "" }
      $stderr = if (Test-Path $script:StderrLog) { Get-Content $script:StderrLog -Raw } else { "" }
      throw "dinerod exited while waiting for RPC during $Phase.`nSTDOUT:`n$stdout`nSTDERR:`n$stderr"
    }

    $count = Invoke-DineroCli -RpcArgs @("getblockcount") -AllowFail
    if ($LASTEXITCODE -eq 0 -and $count -match "^\d+$") {
      return [int]$count
    }
    Start-Sleep -Milliseconds 500
  } while ((Get-Date) -lt $deadline)

  throw "Timed out after $TimeoutSeconds seconds waiting for RPC during $Phase"
}

function Try-GetBlockCount {
  $heightText = Invoke-DineroCli -RpcArgs @("getblockcount") -AllowFail
  if ($LASTEXITCODE -eq 0 -and $heightText -match "^\d+$") {
    return [int]$heightText
  }
  return $null
}

function Wait-ForHeight {
  param(
    [int]$ExpectedHeight,
    [int]$Seconds = 20
  )

  $deadline = (Get-Date).AddSeconds($Seconds)
  do {
    if ($script:NodeProcess.HasExited) {
      return $null
    }
    $height = Try-GetBlockCount
    if ($null -ne $height -and $height -ge $ExpectedHeight) {
      return $height
    }
    Start-Sleep -Milliseconds 500
  } while ((Get-Date) -lt $deadline)

  return Try-GetBlockCount
}

function Start-DineroNode {
  param([string]$Phase)

  $script:StdoutLog = Join-Path $script:SmokeDataDir "dinerod-$Phase.out.log"
  $script:StderrLog = Join-Path $script:SmokeDataDir "dinerod-$Phase.err.log"

  $args = @(
    "--regtest",
    "--datadir=$script:SmokeDataDir",
    "--rpcport=$script:RpcPort",
    "--p2pport=$script:P2pPort",
    "--wallet-socket-port=$script:WalletPort",
    "--p2p.offline=1"
  )

  $script:NodeProcess = Start-Process `
    -FilePath $script:Dinerod `
    -ArgumentList $args `
    -PassThru `
    -WindowStyle Hidden `
    -RedirectStandardOutput $script:StdoutLog `
    -RedirectStandardError $script:StderrLog

  return Wait-ForRpc -Phase $Phase
}

function Stop-DineroNode {
  if (-not $script:NodeProcess) {
    return
  }

  if (-not $script:NodeProcess.HasExited) {
    Invoke-DineroCli -RpcArgs @("stop") -AllowFail | Out-Null
    if (-not $script:NodeProcess.WaitForExit(15000)) {
      Stop-Process -Id $script:NodeProcess.Id -Force
      $script:NodeProcess.WaitForExit(5000) | Out-Null
    }
  }
}

$script:BuildRoot = Resolve-RepoPath $BuildDir
$script:Dinerod = Join-Path $script:BuildRoot "dinerod.exe"
$script:Cli = Join-Path $script:BuildRoot "dinero-cli.exe"

if (-not (Test-Path $script:Dinerod)) {
  throw "Missing dinerod.exe at $script:Dinerod"
}
if (-not (Test-Path $script:Cli)) {
  throw "Missing dinero-cli.exe at $script:Cli"
}

$createdTempDataDir = -not [bool]$DataDir
if ($DataDir) {
  $script:SmokeDataDir = Resolve-RepoPath $DataDir
} else {
  $script:SmokeDataDir = Join-Path $env:TEMP ("dinero-msvc-regtest-smoke-" + [Guid]::NewGuid().ToString("N"))
}

New-Item -ItemType Directory -Force -Path $script:SmokeDataDir | Out-Null

$basePort = Get-Random -Minimum 30000 -Maximum 56000
$script:RpcPort = $basePort
$script:P2pPort = $basePort + 1
$script:WalletPort = $basePort + 2
$script:NodeProcess = $null

Write-Host "Native MSVC regtest smoke"
Write-Host "  build : $script:BuildRoot"
Write-Host "  data  : $script:SmokeDataDir"
Write-Host "  ports : rpc=$script:RpcPort p2p=$script:P2pPort wallet=$script:WalletPort"

try {
  $initialHeight = Start-DineroNode -Phase "first-start"
  $initialHash = Invoke-DineroCli -RpcArgs @("getbestblockhash")
  Write-Host "  first start: height=$initialHeight hash=$initialHash"

  $mineOutput = Invoke-DineroCli -RpcArgs @("generate", "1")
  $afterMineHeight = $null
  $afterMineHash = ""

  if (-not $script:NodeProcess.HasExited) {
    $afterMineHeight = Wait-ForHeight -ExpectedHeight ($initialHeight + 1)
    if ($null -ne $afterMineHeight -and -not $script:NodeProcess.HasExited) {
      $afterMineHash = Invoke-DineroCli -RpcArgs @("getbestblockhash")
    }
  }

  if ($null -eq $afterMineHeight -or $afterMineHeight -lt ($initialHeight + 1)) {
    throw "Expected height $($initialHeight + 1) after mining, got $afterMineHeight`nMining response:`n$mineOutput"
  }

  if ($afterMineHeight -ne ($initialHeight + 1)) {
    throw "Expected height $($initialHeight + 1) after mining, got $afterMineHeight`nMining response:`n$mineOutput"
  }
  if ($afterMineHash -eq $initialHash) {
    throw "Best block hash did not change after mining"
  }

  Write-Host "  mined     : height=$afterMineHeight hash=$afterMineHash"

  Stop-DineroNode

  $restartHeight = Start-DineroNode -Phase "restart"
  $restartHash = Invoke-DineroCli -RpcArgs @("getbestblockhash")

  if ($restartHeight -ne $afterMineHeight) {
    throw "Restart height mismatch: expected $afterMineHeight, got $restartHeight"
  }
  if ($restartHash -ne $afterMineHash) {
    throw "Restart hash mismatch: expected $afterMineHash, got $restartHash"
  }

  $chainInfo = Invoke-DineroCli -RpcArgs @("getblockchaininfo")
  Write-Host "  restart  : height=$restartHeight hash=$restartHash"
  try {
    $chainJson = $chainInfo | ConvertFrom-Json
    $reportedBlocks = if ($null -ne $chainJson.blocks) { $chainJson.blocks } else { "n/a" }
    $reportedBest = if ($chainJson.bestblockhash) { $chainJson.bestblockhash } else { "n/a" }
    Write-Host "  chaininfo: blocks=$reportedBlocks best=$reportedBest"
  } catch {
    Write-Host "  chaininfo: RPC returned non-JSON text"
  }
  Write-Host "PASS: native MSVC dinerod starts, mines one regtest block, stops, restarts, and reloads the same tip."
} finally {
  Stop-DineroNode
  if (-not $KeepDataDir -and $createdTempDataDir) {
    Remove-Item -LiteralPath $script:SmokeDataDir -Recurse -Force -ErrorAction SilentlyContinue
  } elseif ($KeepDataDir) {
    Write-Host "Kept data dir: $script:SmokeDataDir"
  }
}
