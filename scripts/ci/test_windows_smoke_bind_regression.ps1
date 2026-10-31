param([Parameter(Mandatory=$true)][string]$BuildDir)
$ErrorActionPreference = 'Stop'
$path = Join-Path $PSScriptRoot '../native-msvc-regtest-smoke.ps1'
$fixed = Get-Content $path -Raw
$prior = git show b10b2a5f4fd1dcdf95e843ea629f4b78581194c8:scripts/native-msvc-regtest-smoke.ps1 | Out-String
if ($LASTEXITCODE -ne 0 -or $prior -notmatch 'Get-Random -Minimum 30000') { throw 'Prior harness identity mismatch' }
# Bound, non-listening exclusive socket: reproduces an unavailable numeric
# port while CLI connection attempts fail promptly instead of reaching a fake
# HTTP listener. Both complete smoke harnesses see the same forced random port.
$occupied = New-Object System.Net.Sockets.Socket([System.Net.Sockets.AddressFamily]::InterNetwork, [System.Net.Sockets.SocketType]::Stream, [System.Net.Sockets.ProtocolType]::Tcp)
$occupied.ExclusiveAddressUse = $true
$occupied.Bind((New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Loopback, 0)))
$global:OccupiedSmokeRegressionPort = $occupied.LocalEndPoint.Port
function global:Get-Random { param($Minimum, $Maximum) return $global:OccupiedSmokeRegressionPort }
try {
  Set-Content $path -Value $prior -Encoding utf8
  $failedOnBind = $false
  try { & $path -BuildDir $BuildDir -TimeoutSeconds 90 } catch {
    $failedOnBind = $_.ToString().Contains("Failed to bind socket to 127.0.0.1:$global:OccupiedSmokeRegressionPort")
    if (-not $failedOnBind) { throw 'Prior harness failed for an unexpected reason; inspect owned test data' }
  } finally { Set-Content $path -Value $fixed -Encoding utf8 }
  if (-not $failedOnBind) { throw 'Prior native smoke unexpectedly accepted the occupied random port' }
  Write-Host 'PASS negative control: actual prior harness/native daemon fails at the deliberately unavailable RPC port'
  & $path -BuildDir $BuildDir -TimeoutSeconds 90
  Write-Host 'PASS fixed control: the complete native smoke ignores the unavailable random port and passes mining plus cold restart'
} finally {
  Set-Content $path -Value $fixed -Encoding utf8
  Remove-Item Function:\Get-Random -ErrorAction SilentlyContinue
  Remove-Variable OccupiedSmokeRegressionPort -Scope Global -ErrorAction SilentlyContinue
  $occupied.Dispose()
}
