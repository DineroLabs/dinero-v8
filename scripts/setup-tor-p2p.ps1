param(
  [string]$DataDir = "$env:APPDATA\Dinero"
)

$ErrorActionPreference = "Stop"
$conf = Join-Path $DataDir "dinero.conf"

New-Item -ItemType Directory -Force -Path $DataDir | Out-Null
if (!(Test-Path $conf)) {
  New-Item -ItemType File -Force -Path $conf | Out-Null
}

function Set-DineroConfig {
  param(
    [string]$Key,
    [string]$Value
  )

  $lines = Get-Content $conf
  $pattern = "^\s*$([regex]::Escape($Key))\s*="
  $updated = $false
  $next = foreach ($line in $lines) {
    if ($line -match $pattern) {
      $updated = $true
      "$Key=$Value"
    } else {
      $line
    }
  }

  if (!$updated) {
    $next += "$Key=$Value"
  }
  Set-Content -Path $conf -Value $next -Encoding ASCII
}

Set-DineroConfig -Key "listen" -Value "1"
Set-DineroConfig -Key "onion" -Value "auto"

Write-Host "Updated $conf"
Write-Host "  listen=1"
Write-Host "  onion=auto"
Write-Host ""

$systemTor = Test-NetConnection -ComputerName 127.0.0.1 -Port 9050 -InformationLevel Quiet
$browserTor = Test-NetConnection -ComputerName 127.0.0.1 -Port 9150 -InformationLevel Quiet

if ($systemTor) {
  Write-Host "Detected system Tor SOCKS5 on 127.0.0.1:9050."
} elseif ($browserTor) {
  Write-Host "Detected Tor Browser SOCKS5 on 127.0.0.1:9150."
} else {
  Write-Host "Tor SOCKS5 was not detected yet."
  Write-Host "Install/start Tor Browser, or run system Tor with SOCKS5 on 127.0.0.1:9050."
}
