# Dinero Service Installation Script - Windows
# Installs dinerod as a Windows service

param(
    [string]$BinaryPath = "",
    [string]$DataDir = ""
)

# Get script directory
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir

# Default paths
if ($BinaryPath -eq "") {
    $BinaryPath = Join-Path $ProjectRoot "build\bin\dinerod.exe"
}
if ($DataDir -eq "") {
    $DataDir = "$env:APPDATA\DineroCoin"
}

Write-Host "🪟 Installing Dinero daemon service for Windows..." -ForegroundColor Green

# Check if binary exists
if (-not (Test-Path $BinaryPath)) {
    Write-Host "❌ Dinero binary not found at: $BinaryPath" -ForegroundColor Red
    Write-Host "Please build the project first: cmake --build build" -ForegroundColor Yellow
    exit 1
}

# Create data directory
Write-Host "📁 Creating data directory..." -ForegroundColor Blue
New-Item -ItemType Directory -Force -Path $DataDir | Out-Null

# Create secure config
Write-Host "🔒 Creating secure configuration..." -ForegroundColor Blue
$ConfigContent = @"
# Dinero Core Configuration
# Generated with secure defaults

server=1
rpcbind=127.0.0.1
rpcallowip=127.0.0.1
rpcport=20998
port=20999
rpccookiefile=$DataDir\.cookie

# Security: localhost-only RPC binding
# Cookie authentication enabled
# No external network access by default
"@

$ConfigPath = Join-Path $DataDir "dinero.conf"
$ConfigContent | Out-File -FilePath $ConfigPath -Encoding UTF8

# Set secure permissions (owner-only)
icacls $ConfigPath /inheritance:r /grant:r "$env:USERNAME:(F)" | Out-Null

# Create service
Write-Host "🚀 Creating Windows service..." -ForegroundColor Blue
$ServiceName = "DineroD"
$ServiceDisplayName = "Dinero Core Daemon"
$ServiceDescription = "Dinero cryptocurrency daemon with canonical consensus"

# Remove existing service if it exists
if (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue) {
    Write-Host "⚠️  Removing existing service..." -ForegroundColor Yellow
    Stop-Service -Name $ServiceName -Force -ErrorAction SilentlyContinue
    sc.exe delete $ServiceName | Out-Null
    Start-Sleep -Seconds 2
}

# Create new service
$ServiceArgs = "`"$BinaryPath`" -datadir=`"$DataDir`""
sc.exe create $ServiceName binPath= $ServiceArgs DisplayName= $ServiceDisplayName start= auto | Out-Null

if ($LASTEXITCODE -ne 0) {
    Write-Host "❌ Failed to create service" -ForegroundColor Red
    exit 1
}

# Configure service failure actions
Write-Host "🔧 Configuring service recovery..." -ForegroundColor Blue
sc.exe failure $ServiceName reset= 0 actions= restart/1000/restart/2000/restart/5000 | Out-Null

# Set service description
sc.exe description $ServiceName $ServiceDescription | Out-Null

# Start the service
Write-Host "▶️  Starting Dinero daemon service..." -ForegroundColor Blue
Start-Service -Name $ServiceName

# Wait for startup
Start-Sleep -Seconds 3

# Check service status
$Service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($Service -and $Service.Status -eq "Running") {
    Write-Host "✅ Dinero daemon service installed and started successfully!" -ForegroundColor Green
    Write-Host ""
    Write-Host "📊 Service Status:" -ForegroundColor Cyan
    Get-Service -Name $ServiceName | Format-Table -AutoSize
    Write-Host ""
    Write-Host "🔧 Management Commands:" -ForegroundColor Cyan
    Write-Host "  Stop:     Stop-Service -Name $ServiceName" -ForegroundColor White
    Write-Host "  Start:    Start-Service -Name $ServiceName" -ForegroundColor White
    Write-Host "  Restart:  Restart-Service -Name $ServiceName" -ForegroundColor White
    Write-Host "  Status:   Get-Service -Name $ServiceName" -ForegroundColor White
    Write-Host "  Remove:   sc.exe delete $ServiceName" -ForegroundColor White
    Write-Host ""
    Write-Host "🌐 RPC Endpoint: http://127.0.0.1:20998" -ForegroundColor Cyan
    Write-Host "📁 Data Directory: $DataDir" -ForegroundColor Cyan
    Write-Host "📝 Config File: $ConfigPath" -ForegroundColor Cyan
} else {
    Write-Host "❌ Failed to start Dinero daemon service" -ForegroundColor Red
    Write-Host "Check Windows Event Log for details" -ForegroundColor Yellow
    exit 1
}
