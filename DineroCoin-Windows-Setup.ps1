# 🚀 Dinero Cryptocurrency - Windows Setup & Installer
# Self-contained PowerShell script that downloads, builds, and installs Dinero on Windows
# 
# Usage: Right-click → "Run with PowerShell" or run from PowerShell as Administrator

param(
    [string]$InstallPath = "$env:PROGRAMFILES\Dinero",
    [string]$DataPath = "$env:APPDATA\DineroCoin",
    [switch]$SkipBuild = $false,
    [switch]$Portable = $false
)

# Set execution policy for this session
Set-ExecutionPolicy -ExecutionPolicy Bypass -Scope Process -Force

Write-Host "🚀 Dinero Cryptocurrency - Windows Setup" -ForegroundColor Green
Write-Host "=======================================" -ForegroundColor Green
Write-Host ""

# Check if running as Administrator
$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole] "Administrator")

if (-not $isAdmin -and -not $Portable) {
    Write-Host "⚠️  Administrator privileges required for system installation" -ForegroundColor Yellow
    Write-Host "   Restarting as Administrator..." -ForegroundColor Yellow
    Start-Process PowerShell -Verb RunAs -ArgumentList "-File `"$PSCommandPath`" -InstallPath `"$InstallPath`" -DataPath `"$DataPath`""
    exit
}

Write-Host "✅ Running with appropriate privileges" -ForegroundColor Green

# Function to download file with progress
function Download-File {
    param([string]$Url, [string]$Path)
    try {
        Write-Host "📥 Downloading: $(Split-Path $Path -Leaf)" -ForegroundColor Blue
        $webClient = New-Object System.Net.WebClient
        $webClient.DownloadFile($Url, $Path)
        Write-Host "✅ Downloaded successfully" -ForegroundColor Green
        return $true
    } catch {
        Write-Host "❌ Download failed: $($_.Exception.Message)" -ForegroundColor Red
        return $false
    }
}

# Create temporary directory
$tempDir = Join-Path $env:TEMP "DineroSetup"
if (Test-Path $tempDir) { Remove-Item $tempDir -Recurse -Force }
New-Item -ItemType Directory -Path $tempDir -Force | Out-Null

Write-Host "📁 Working directory: $tempDir" -ForegroundColor Blue

# Download pre-built binaries (if available) or source code
$repoUrl = "https://github.com/your-org/DineroCoin"
$releaseUrl = "$repoUrl/releases/latest/download"

# Try to download pre-built Windows binaries first
$binaryUrls = @(
    "$releaseUrl/DineroCoin-Windows-x64.zip",
    "$releaseUrl/dinero-comprehensive.exe",
    "$releaseUrl/dinerod.exe"
)

$downloadSuccess = $false
foreach ($url in $binaryUrls) {
    $fileName = Split-Path $url -Leaf
    $filePath = Join-Path $tempDir $fileName
    if (Download-File $url $filePath) {
        $downloadSuccess = $true
        break
    }
}

if (-not $downloadSuccess) {
    Write-Host "📦 Pre-built binaries not available, will build from source" -ForegroundColor Yellow
    
    # Check build prerequisites
    Write-Host "🔍 Checking build prerequisites..." -ForegroundColor Blue
    
    $prerequisites = @()
    
    # Check Git
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        $prerequisites += "Git for Windows (https://git-scm.com/download/win)"
    }
    
    # Check Visual Studio or Build Tools
    $vsInstalled = $false
    $vsPaths = @(
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat",
        "${env:ProgramFiles}\Microsoft Visual Studio\2019\Community\Common7\Tools\VsDevCmd.bat",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2019\BuildTools\Common7\Tools\VsDevCmd.bat"
    )
    
    foreach ($path in $vsPaths) {
        if (Test-Path $path) {
            $vsInstalled = $true
            $vsDevCmd = $path
            break
        }
    }
    
    if (-not $vsInstalled) {
        $prerequisites += "Visual Studio 2019/2022 with C++ workload or Build Tools for Visual Studio"
    }
    
    # Check CMake
    if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
        $prerequisites += "CMake (https://cmake.org/download/ or winget install Kitware.CMake)"
    }
    
    # Check Qt6 (optional)
    $qtPaths = @(
        "C:\Qt\6.9.1\msvc2022_64",
        "C:\Qt\6.7.2\msvc2022_64",
        "C:\Qt\6.9.1\msvc2019_64"
    )
    
    $qtInstalled = $false
    foreach ($path in $qtPaths) {
        if (Test-Path $path) {
            $qtInstalled = $true
            $qtPath = $path
            break
        }
    }
    
    if ($prerequisites.Count -gt 0) {
        Write-Host "❌ Missing prerequisites:" -ForegroundColor Red
        foreach ($prereq in $prerequisites) {
            Write-Host "   • $prereq" -ForegroundColor Red
        }
        Write-Host ""
        Write-Host "📋 Installation options:" -ForegroundColor Yellow
        Write-Host "   1. Install prerequisites manually" -ForegroundColor Yellow
        Write-Host "   2. Use Chocolatey: choco install git cmake visualstudio2022buildtools" -ForegroundColor Yellow
        Write-Host "   3. Use winget: winget install Git.Git Kitware.CMake Microsoft.VisualStudio.2022.BuildTools" -ForegroundColor Yellow
        Write-Host ""
        
        $choice = Read-Host "Install prerequisites automatically? (y/N)"
        if ($choice -eq 'y' -or $choice -eq 'Y') {
            Write-Host "🔧 Installing prerequisites..." -ForegroundColor Blue
            
            # Install Chocolatey if not present
            if (-not (Get-Command choco -ErrorAction SilentlyContinue)) {
                Write-Host "📦 Installing Chocolatey..." -ForegroundColor Blue
                Set-ExecutionPolicy Bypass -Scope Process -Force
                [System.Net.ServicePointManager]::SecurityProtocol = [System.Net.ServicePointManager]::SecurityProtocol -bor 3072
                iex ((New-Object System.Net.WebClient).DownloadString('https://community.chocolatey.org/install.ps1'))
            }
            
            # Install prerequisites
            choco install git cmake visualstudio2022buildtools --yes
            
            Write-Host "✅ Prerequisites installed. Please restart this script." -ForegroundColor Green
            Read-Host "Press Enter to exit"
            exit
        } else {
            Write-Host "❌ Cannot proceed without prerequisites" -ForegroundColor Red
            Read-Host "Press Enter to exit"
            exit 1
        }
    }
    
    # Clone repository and build
    Write-Host "📥 Cloning Dinero repository..." -ForegroundColor Blue
    $repoDir = Join-Path $tempDir "DineroCoin"
    git clone $repoUrl $repoDir
    
    if (-not (Test-Path $repoDir)) {
        Write-Host "❌ Failed to clone repository" -ForegroundColor Red
        Read-Host "Press Enter to exit"
        exit 1
    }
    
    Set-Location $repoDir
    
    # Build the project
    Write-Host "🔨 Building Dinero..." -ForegroundColor Blue
    
    # Setup Visual Studio environment
    cmd /c "`"$vsDevCmd`" && cmake -S . -B build -G `"Visual Studio 17 2022`" -A x64 -DCMAKE_BUILD_TYPE=Release $(if ($qtInstalled) { "-DCMAKE_PREFIX_PATH=`"$qtPath`" -DBUILD_GUI=ON" } else { "-DBUILD_GUI=OFF" }) && cmake --build build --config Release"
    
    if (-not (Test-Path "build\bin\Release\dinerod.exe")) {
        Write-Host "❌ Build failed" -ForegroundColor Red
        Read-Host "Press Enter to exit"
        exit 1
    }
    
    Write-Host "✅ Build completed successfully!" -ForegroundColor Green
    $binariesPath = "build\bin\Release"
} else {
    # Extract downloaded binaries
    if ($downloadSuccess -and (Test-Path (Join-Path $tempDir "DineroCoin-Windows-x64.zip"))) {
        Write-Host "📦 Extracting binaries..." -ForegroundColor Blue
        Expand-Archive -Path (Join-Path $tempDir "DineroCoin-Windows-x64.zip") -DestinationPath $tempDir -Force
        $binariesPath = $tempDir
    }
}

# Installation
if ($Portable) {
    $InstallPath = Join-Path $env:USERPROFILE "DineroCoin"
    Write-Host "📦 Creating portable installation in: $InstallPath" -ForegroundColor Blue
} else {
    Write-Host "🏗️  Installing to: $InstallPath" -ForegroundColor Blue
}

# Create installation directory
if (Test-Path $InstallPath) { Remove-Item $InstallPath -Recurse -Force }
New-Item -ItemType Directory -Path $InstallPath -Force | Out-Null

# Copy binaries
$binaries = @("dinerod.exe", "dinero-cli.exe")
if ($qtInstalled) { $binaries += "dinero-comprehensive.exe" }

foreach ($binary in $binaries) {
    $sourcePath = Join-Path $binariesPath $binary
    if (Test-Path $sourcePath) {
        Copy-Item $sourcePath $InstallPath -Force
        Write-Host "✅ Installed: $binary" -ForegroundColor Green
    }
}

# Copy Qt6 DLLs if GUI was built
if ($qtInstalled -and (Test-Path (Join-Path $InstallPath "dinero-comprehensive.exe"))) {
    Write-Host "📦 Deploying Qt6 dependencies..." -ForegroundColor Blue
    & "$qtPath\bin\windeployqt.exe" (Join-Path $InstallPath "dinero-comprehensive.exe") --release --no-translations
}

# Create data directory
Write-Host "📁 Creating data directory: $DataPath" -ForegroundColor Blue
New-Item -ItemType Directory -Path $DataPath -Force | Out-Null

# Create configuration file
$configContent = @"
# Dinero Core Configuration
server=1
rpcbind=127.0.0.1
rpcallowip=127.0.0.1
rpcport=20998
port=20999
datadir=$($DataPath.Replace('\', '/'))
"@

$configPath = Join-Path $DataPath "dinero.conf"
$configContent | Out-File -FilePath $configPath -Encoding UTF8
Write-Host "✅ Created configuration: $configPath" -ForegroundColor Green

# Create shortcuts
if (-not $Portable) {
    Write-Host "🔗 Creating shortcuts..." -ForegroundColor Blue
    
    # Start Menu shortcut
    $startMenuPath = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs"
    $shortcutPath = Join-Path $startMenuPath "Dinero Wallet.lnk"
    
    $WshShell = New-Object -comObject WScript.Shell
    $Shortcut = $WshShell.CreateShortcut($shortcutPath)
    if (Test-Path (Join-Path $InstallPath "dinero-comprehensive.exe")) {
        $Shortcut.TargetPath = Join-Path $InstallPath "dinero-comprehensive.exe"
        $Shortcut.Description = "Dinero Cryptocurrency Wallet & Mining Node"
    } else {
        $Shortcut.TargetPath = Join-Path $InstallPath "dinerod.exe"
        $Shortcut.Arguments = "-daemon=0 -server=1"
        $Shortcut.Description = "Dinero Cryptocurrency Daemon"
    }
    $Shortcut.WorkingDirectory = $InstallPath
    $Shortcut.Save()
    
    # Desktop shortcut (optional)
    $choice = Read-Host "Create desktop shortcut? (Y/n)"
    if ($choice -ne 'n' -and $choice -ne 'N') {
        $desktopPath = [Environment]::GetFolderPath("Desktop")
        $desktopShortcut = Join-Path $desktopPath "Dinero Wallet.lnk"
        Copy-Item $shortcutPath $desktopShortcut -Force
        Write-Host "✅ Created desktop shortcut" -ForegroundColor Green
    }
}

# Register uninstaller
if (-not $Portable) {
    Write-Host "📝 Registering uninstaller..." -ForegroundColor Blue
    $uninstallKey = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\DineroCoin"
    New-Item -Path $uninstallKey -Force | Out-Null
    Set-ItemProperty -Path $uninstallKey -Name "DisplayName" -Value "Dinero Cryptocurrency"
    Set-ItemProperty -Path $uninstallKey -Name "DisplayVersion" -Value "1.0.0"
    Set-ItemProperty -Path $uninstallKey -Name "Publisher" -Value "Dinero Project"
    Set-ItemProperty -Path $uninstallKey -Name "InstallLocation" -Value $InstallPath
    Set-ItemProperty -Path $uninstallKey -Name "UninstallString" -Value "powershell.exe -File `"$PSCommandPath`" -Uninstall"
}

# Cleanup
Write-Host "🧹 Cleaning up..." -ForegroundColor Blue
Set-Location $env:TEMP
Remove-Item $tempDir -Recurse -Force -ErrorAction SilentlyContinue

# Success message
Write-Host ""
Write-Host "🎉 Dinero installation completed successfully!" -ForegroundColor Green
Write-Host "=======================================" -ForegroundColor Green
Write-Host ""
Write-Host "📁 Installation path: $InstallPath" -ForegroundColor Blue
Write-Host "📁 Data directory: $DataPath" -ForegroundColor Blue
Write-Host ""

if (Test-Path (Join-Path $InstallPath "dinero-comprehensive.exe")) {
    Write-Host "🚀 Launch Dinero Wallet from Start Menu or run:" -ForegroundColor Yellow
    Write-Host "   $InstallPath\dinero-comprehensive.exe" -ForegroundColor White
} else {
    Write-Host "🚀 Start Dinero daemon with:" -ForegroundColor Yellow
    Write-Host "   $InstallPath\dinerod.exe -daemon=0 -server=1" -ForegroundColor White
}

Write-Host ""
Write-Host "📋 Next steps:" -ForegroundColor Yellow
Write-Host "   1. Launch the application" -ForegroundColor White
Write-Host "   2. Complete the first-run setup" -ForegroundColor White
Write-Host "   3. Start mining Dinero coins!" -ForegroundColor White
Write-Host ""

Read-Host "Press Enter to exit"
