param(
  [string]$BuildDir = "build-msvc-native\Release",
  [string[]]$Executables = @("dinerod.exe", "dinero-cli.exe"),
  [string]$Dumpbin = "",
  [string]$StageDir = "",
  [switch]$CleanStage,
  [string]$ZipPath = ""
)

$ErrorActionPreference = "Stop"

function Resolve-RepoPath([string]$Path) {
  if ([System.IO.Path]::IsPathRooted($Path)) {
    return $Path
  }
  return (Join-Path (Get-Location) $Path)
}

function Find-Dumpbin {
  if ($Dumpbin) {
    if (-not (Test-Path $Dumpbin)) {
      throw "Requested dumpbin.exe not found: $Dumpbin"
    }
    return $Dumpbin
  }

  $cmd = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
  if ($cmd) {
    return $cmd.Source
  }

  $roots = @(
    "C:\Program Files\Microsoft Visual Studio\2022",
    "C:\Program Files (x86)\Microsoft Visual Studio\2022"
  )

  $candidates = foreach ($root in $roots) {
    if (Test-Path $root) {
      Get-ChildItem -Path $root -Filter dumpbin.exe -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -like "*\bin\Hostx64\x64\dumpbin.exe" }
    }
  }

  $pick = $candidates | Sort-Object FullName -Descending | Select-Object -First 1
  if (-not $pick) {
    throw "dumpbin.exe was not found. Run from a Developer PowerShell or pass -Dumpbin <path>."
  }
  return $pick.FullName
}

function Get-Dependents([string]$ExePath) {
  $lines = & $script:DumpbinPath /DEPENDENTS $ExePath 2>&1
  if ($LASTEXITCODE -ne 0) {
    throw "dumpbin failed for $ExePath`n$($lines | Out-String)"
  }

  $deps = New-Object System.Collections.Generic.List[string]
  $inside = $false
  foreach ($line in $lines) {
    if ($line -match "Image has the following dependencies:") {
      $inside = $true
      continue
    }
    if ($inside -and $line -match "^\s*([A-Za-z0-9_.-]+\.dll)\s*$") {
      $deps.Add($Matches[1])
      continue
    }
    if ($inside -and $line -match "^\s*Summary\s*$") {
      break
    }
  }
  return $deps
}

function Resolve-Dependency([string]$Name) {
  $searchDirs = @($script:BuildRoot)
  $pathDirs = foreach ($dir in ($env:PATH -split ";")) {
    if (-not $dir) {
      continue
    }
    try {
      if (Test-Path -LiteralPath $dir -ErrorAction Stop) {
        $dir
      }
    } catch {
      # Some developer-tool PATH entries deny directory probes. They cannot
      # help stage DLLs, so skip them quietly.
    }
  }
  $searchDirs += $pathDirs

  foreach ($dir in $searchDirs) {
    $candidate = Join-Path $dir $Name
    if (Test-Path $candidate) {
      return (Resolve-Path $candidate).Path
    }
  }
  return ""
}

$systemDlls = @(
  "advapi32.dll", "bcrypt.dll", "crypt32.dll", "dbghelp.dll", "gdi32.dll",
  "iphlpapi.dll", "kernel32.dll", "mswsock.dll", "ncrypt.dll", "ntdll.dll",
  "ole32.dll", "rpcrt4.dll", "secur32.dll", "shell32.dll", "shlwapi.dll",
  "user32.dll", "userenv.dll", "version.dll", "winhttp.dll", "winmm.dll",
  "ws2_32.dll", "wtsapi32.dll"
)

$runtimeDlls = @(
  "concrt140.dll", "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll",
  "ucrtbase.dll", "vcruntime140.dll", "vcruntime140_1.dll"
)

$script:BuildRoot = Resolve-RepoPath $BuildDir
$script:DumpbinPath = Find-Dumpbin

if (-not (Test-Path $script:BuildRoot)) {
  throw "Build directory not found: $script:BuildRoot"
}

Write-Host "Native MSVC dependency scan"
Write-Host "  build  : $script:BuildRoot"
Write-Host "  dumpbin: $script:DumpbinPath"

$allRows = @()
foreach ($exe in $Executables) {
  $exePath = Join-Path $script:BuildRoot $exe
  if (-not (Test-Path $exePath)) {
    throw "Executable not found: $exePath"
  }

  $deps = Get-Dependents $exePath | Sort-Object -Unique
  foreach ($dep in $deps) {
    $lower = $dep.ToLowerInvariant()
    $kind = if ($systemDlls -contains $lower) {
      "Windows"
    } elseif (($runtimeDlls -contains $lower) -or ($lower -like "api-ms-win-crt-*.dll")) {
      "MSVC runtime"
    } else {
      "App/dependency"
    }

    $resolved = Resolve-Dependency $dep
    $allRows += [PSCustomObject]@{
      Executable = $exe
      Dependency = $dep
      Kind = $kind
      ResolvedPath = $resolved
    }
  }
}

$allRows | Sort-Object Executable, Dependency | Format-Table -AutoSize

$missing = $allRows | Where-Object { $_.Kind -eq "App/dependency" -and -not $_.ResolvedPath }
if ($missing) {
  Write-Host ""
  Write-Host "Missing app/dependency DLLs:"
  $missing | Format-Table Executable, Dependency -AutoSize
  exit 2
}

if ($StageDir) {
  $stageRoot = Resolve-RepoPath $StageDir
  if ($CleanStage -and (Test-Path $stageRoot)) {
    Remove-Item -LiteralPath $stageRoot -Recurse -Force
  }
  New-Item -ItemType Directory -Force -Path $stageRoot | Out-Null

  foreach ($exe in $Executables) {
    Copy-Item -LiteralPath (Join-Path $script:BuildRoot $exe) -Destination $stageRoot -Force
  }

  $dllsToStage = $allRows |
    Where-Object { $_.Kind -eq "App/dependency" -and $_.ResolvedPath } |
    Select-Object -ExpandProperty ResolvedPath -Unique

  foreach ($dll in $dllsToStage) {
    Copy-Item -LiteralPath $dll -Destination $stageRoot -Force
  }

  $manifestPath = Join-Path $stageRoot "native-msvc-manifest.txt"
  $nowUtc = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
  $commit = if (Get-Command git -ErrorAction SilentlyContinue) {
    git rev-parse --short HEAD 2>$null
  } else {
    "unknown"
  }
  if (-not $commit) {
    $commit = "unknown"
  }

  $manifestLines = New-Object System.Collections.Generic.List[string]
  $manifestLines.Add("Dinero native MSVC staging manifest")
  $manifestLines.Add("generated_utc=$nowUtc")
  $manifestLines.Add("source_commit=$commit")
  $manifestLines.Add("source_build_dir=$script:BuildRoot")
  $manifestLines.Add("")
  $manifestLines.Add("Files:")

  Get-ChildItem -LiteralPath $stageRoot -File | Sort-Object Name | ForEach-Object {
    $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    $manifestLines.Add("$hash  $($_.Name)  $($_.Length) bytes")
  }

  $manifestLines | Set-Content -LiteralPath $manifestPath -Encoding ASCII

  Write-Host ""
  Write-Host "Staged executables and app/dependency DLLs to: $stageRoot"
  Write-Host "Wrote manifest: $manifestPath"

  if ($ZipPath) {
    $zipFullPath = Resolve-RepoPath $ZipPath
    $zipParent = Split-Path -Parent $zipFullPath
    if ($zipParent) {
      New-Item -ItemType Directory -Force -Path $zipParent | Out-Null
    }
    if (Test-Path $zipFullPath) {
      Remove-Item -LiteralPath $zipFullPath -Force
    }

    Compress-Archive -Path (Join-Path $stageRoot "*") -DestinationPath $zipFullPath -Force
    $zipHash = (Get-FileHash -LiteralPath $zipFullPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $zipName = Split-Path -Leaf $zipFullPath
    "$zipHash  $zipName" | Set-Content -LiteralPath "$zipFullPath.sha256" -Encoding ASCII

    Write-Host "Wrote ZIP: $zipFullPath"
    Write-Host "Wrote ZIP checksum: $zipFullPath.sha256"
  }
}

Write-Host ""
Write-Host "PASS: dependency scan completed; all non-system dependencies resolved."
