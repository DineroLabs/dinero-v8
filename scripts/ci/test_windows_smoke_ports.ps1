$ErrorActionPreference = 'Stop'
$path = Join-Path $PSScriptRoot '../native-msvc-regtest-smoke.ps1'
$tokens = $null; $errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile((Resolve-Path $path), [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw "Smoke script parse failure: $errors" }
$function = $ast.Find({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Get-SmokePorts' }, $true)
if (-not $function) { throw 'Regression: numeric random ports are not checked by binding before use' }
. ([scriptblock]::Create($function.Extent.Text))
$occupied = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, 0)
$occupied.Server.ExclusiveAddressUse = $true
$occupied.Start()
try {
  $refused = $false
  try { Get-SmokePorts -CandidatePorts @($occupied.LocalEndpoint.Port, 0, 0) | Out-Null } catch { $refused = $true }
  if (-not $refused) { throw 'An occupied port was accepted' }
  for ($run = 0; $run -lt 30; $run++) {
    $ports = @(Get-SmokePorts)
    if ($ports.Count -ne 3 -or @($ports | Select-Object -Unique).Count -ne 3) { throw 'Need three distinct bound ports' }
    $listeners = @()
    try {
      foreach ($port in $ports) {
        if ($port -le 0 -or $port -gt 65535) { throw 'Invalid allocated port' }
        $listener = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, $port)
        $listener.Server.ExclusiveAddressUse = $true
        $listeners += $listener
        $listener.Start()
      }
    } finally { foreach ($listener in $listeners) { $listener.Stop() } }
  }
} finally { $occupied.Stop() }
Write-Host 'PASS: occupied port rejected; 30 sets of three distinct bindable ports allocated and released'
