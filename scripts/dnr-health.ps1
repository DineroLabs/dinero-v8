# Dinero Health Monitoring Script (PowerShell)
# Cross-platform health dashboard with standardized paths and auth

param(
    [string]$Rpc = $env:DINERO_RPC_URL ?? "http://127.0.0.1:20998",
    [string]$DataDir = $env:DINERO_DATADIR ?? "$env:APPDATA\DineroCoin",
    [string]$CookieFile = $env:DINERO_COOKIE_FILE ?? "$env:APPDATA\DineroCoin\.cookie",
    [string]$Network = $env:DINERO_NETWORK ?? "mainnet",
    [switch]$Json,
    [switch]$Help
)

# Exit codes
$EXIT_HEALTHY = 0
$EXIT_UNHEALTHY = 1
$EXIT_DAEMON_DOWN = 2

# Colors for output
$Colors = @{
    Red = "Red"
    Green = "Green"
    Yellow = "Yellow"
    Blue = "Blue"
    White = "White"
}

function Write-ColorOutput {
    param(
        [string]$Message,
        [string]$Color = "White"
    )
    Write-Host $Message -ForegroundColor $Color
}

function Show-Help {
    Write-ColorOutput "Dinero Health Monitoring Script (PowerShell)" $Colors.Blue
    Write-Host ""
    Write-Host "Usage: .\din-health.ps1 [options]"
    Write-Host ""
    Write-Host "Options:"
    Write-Host "  -Rpc <url>        RPC endpoint URL"
    Write-Host "  -DataDir <path>   Data directory path"
    Write-Host "  -CookieFile <path> Cookie file path"
    Write-Host "  -Network <name>   Network (mainnet, testnet, regtest)"
    Write-Host "  -Json             Output health info as JSON"
    Write-Host "  -Help             Show this help message"
    Write-Host ""
    Write-Host "Environment Variables:"
    Write-Host "  DINERO_RPC_URL      RPC endpoint URL"
    Write-Host "  DINERO_DATADIR      Data directory path"
    Write-Host "  DINERO_COOKIE_FILE  Cookie file path"
    Write-Host "  DINERO_NETWORK      Network name"
    Write-Host ""
    Write-Host "Exit Codes:"
    Write-Host "  0  Healthy"
    Write-Host "  1  Unhealthy"
    Write-Host "  2  Daemon down"
}

function Get-AuthHeader {
    if (Test-Path $CookieFile) {
        try {
            $cookie = (Get-Content -Raw -Path $CookieFile).Trim()
            if ($cookie -notmatch ":") {
                $cookie = "__cookie__:$cookie"
            }
            $bytes = [Text.Encoding]::UTF8.GetBytes($cookie)
            $b64 = [Convert]::ToBase64String($bytes)
            return @{ Authorization = "Basic $b64"; "Content-Type" = "application/json" }
        }
        catch {
            Write-ColorOutput "Warning: Failed to read cookie file: $_" $Colors.Yellow
        }
    }
    return @{ "Content-Type" = "application/json" }
}

function Invoke-RpcCall {
    param(
        [string]$Method,
        [array]$Params = @()
    )
    
    $body = @{
        jsonrpc = "2.0"
        id = 1
        method = $Method
        params = $Params
    } | ConvertTo-Json -Compress
    
    $headers = Get-AuthHeader
    
    try {
        $response = Invoke-RestMethod -Uri $Rpc -Method Post -Headers $headers -Body $body -ErrorAction Stop
        return $response.result
    }
    catch {
        Write-ColorOutput "RPC call failed: $_" $Colors.Red
        return $null
    }
}

function Test-DaemonHealth {
    $response = Invoke-RpcCall "gethealth"
    return $response -ne $null
}

function Get-HealthInfo {
    return Invoke-RpcCall "gethealth"
}

function Get-MiningInfo {
    return Invoke-RpcCall "getmininginfo"
}

function Get-BlockchainInfo {
    return Invoke-RpcCall "getblockchaininfo"
}

function Get-NetworkInfo {
    return Invoke-RpcCall "getnetworkinfo"
}

function Show-Dashboard {
    Write-ColorOutput "=== Dinero Health Dashboard ===" $Colors.Blue
    Write-ColorOutput "Network: $Network" $Colors.Yellow
    Write-ColorOutput "RPC URL: $Rpc" $Colors.Yellow
    Write-ColorOutput "Data Dir: $DataDir" $Colors.Yellow
    Write-Host ""
    
    # Get all information
    $healthInfo = Get-HealthInfo
    $miningInfo = Get-MiningInfo
    $blockchainInfo = Get-BlockchainInfo
    $networkInfo = Get-NetworkInfo
    
    if ($healthInfo -eq $null) {
        Write-ColorOutput "❌ Failed to get health information" $Colors.Red
        return $EXIT_UNHEALTHY
    }
    
    # Parse and display health info
    $status = $healthInfo.status ?? "unknown"
    $chain = $healthInfo.chain ?? "unknown"
    $height = $healthInfo.height ?? 0
    $hashrate = $healthInfo.hashrate ?? 0
    $miningEnabled = $healthInfo.mining_enabled ?? $false
    $mempoolSize = $healthInfo.mempool_size ?? 0
    $connections = $healthInfo.connections ?? 0
    $uptime = $healthInfo.uptime ?? 0
    
    # Status indicator
    if ($status -eq "healthy") {
        Write-ColorOutput "Status: ✅ $status" $Colors.Green
    } else {
        Write-ColorOutput "Status: ❌ $status" $Colors.Red
    }
    
    Write-ColorOutput "Chain: $chain" $Colors.Yellow
    Write-ColorOutput "Height: $height" $Colors.Yellow
    Write-ColorOutput "Hashrate: $([math]::Round($hashrate, 2)) H/s" $Colors.Yellow
    
    # Mining status
    if ($miningEnabled) {
        Write-ColorOutput "Mining: ✅ Enabled" $Colors.Green
    } else {
        Write-ColorOutput "Mining: ⏸️  Disabled" $Colors.Yellow
    }
    
    Write-ColorOutput "Mempool: $mempoolSize transactions" $Colors.Yellow
    Write-ColorOutput "Connections: $connections peers" $Colors.Yellow
    
    if ($uptime -gt 0) {
        $uptimeHours = [math]::Floor($uptime / 3600)
        $uptimeMins = [math]::Floor(($uptime % 3600) / 60)
        Write-ColorOutput "Uptime: ${uptimeHours}h ${uptimeMins}m" $Colors.Yellow
    }
    
    Write-Host ""
    
    # Additional mining details if available
    if ($miningInfo -ne $null) {
        $difficulty = $miningInfo.difficulty ?? 0
        $networkHashrate = $miningInfo.networkhashps ?? 0
        $miningAddress = $miningInfo.mining_address ?? "none"
        
        Write-ColorOutput "=== Mining Details ===" $Colors.Blue
        Write-ColorOutput "Difficulty: $([math]::Round($difficulty, 2))" $Colors.Yellow
        Write-ColorOutput "Network Hashrate: $([math]::Round($networkHashrate, 2)) H/s" $Colors.Yellow
        Write-ColorOutput "Mining Address: $miningAddress" $Colors.Yellow
        Write-Host ""
    }
    
    # Network details if available
    if ($networkInfo -ne $null) {
        $version = $networkInfo.version ?? "unknown"
        $subversion = $networkInfo.subversion ?? "unknown"
        
        Write-ColorOutput "=== Network Details ===" $Colors.Blue
        Write-ColorOutput "Version: $version" $Colors.Yellow
        Write-ColorOutput "Subversion: $subversion" $Colors.Yellow
        Write-Host ""
    }
    
    # Return appropriate exit code
    if ($status -eq "healthy") {
        return $EXIT_HEALTHY
    } else {
        return $EXIT_UNHEALTHY
    }
}

function Main {
    # Set network-specific defaults
    switch ($Network) {
        "testnet" {
            $script:Rpc = $env:DINERO_RPC_URL ?? "http://127.0.0.1:20998"
            $script:DataDir = $env:DINERO_DATADIR ?? "$env:APPDATA\DineroCoin\testnet"
        }
        "regtest" {
            $script:Rpc = $env:DINERO_RPC_URL ?? "http://127.0.0.1:20996"
            $script:DataDir = $env:DINERO_DATADIR ?? "$env:APPDATA\DineroCoin\regtest"
        }
        "mainnet" {
            $script:Rpc = $env:DINERO_RPC_URL ?? "http://127.0.0.1:20998"
            $script:DataDir = $env:DINERO_DATADIR ?? "$env:APPDATA\DineroCoin"
        }
        default {
            Write-ColorOutput "Error: Invalid network '$Network'" $Colors.Red
            Write-ColorOutput "Valid networks: mainnet, testnet, regtest" $Colors.Yellow
            exit 1
        }
    }
    
    $script:CookieFile = $env:DINERO_COOKIE_FILE ?? "$DataDir\.cookie"
    
    # Check if daemon is responding
    if (-not (Test-DaemonHealth)) {
        Write-ColorOutput "❌ Daemon is not responding" $Colors.Red
        Write-ColorOutput "RPC URL: $Rpc" $Colors.Yellow
        Write-ColorOutput "Data Dir: $DataDir" $Colors.Yellow
        Write-ColorOutput "Cookie File: $CookieFile" $Colors.Yellow
        Write-Host ""
        Write-ColorOutput "Troubleshooting:" $Colors.White
        Write-ColorOutput "  1. Check if dinerod is running" $Colors.White
        Write-ColorOutput "  2. Verify RPC URL and port" $Colors.White
        Write-ColorOutput "  3. Check cookie file exists and is readable" $Colors.White
        Write-ColorOutput "  4. Check firewall settings" $Colors.White
        exit $EXIT_DAEMON_DOWN
    }
    
    # Display dashboard or JSON output
    if ($Json) {
        $healthInfo = Get-HealthInfo
        if ($healthInfo -ne $null) {
            $healthInfo | ConvertTo-Json -Depth 10
            exit $EXIT_HEALTHY
        } else {
            exit $EXIT_UNHEALTHY
        }
    } else {
        Show-Dashboard
    }
}

# Handle command line arguments
if ($Help) {
    Show-Help
    exit 0
}

# Run main function
Main
