param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [string]$ExpectedVersion = '3.5.7'
)

$ErrorActionPreference = 'Stop'

function Fail([string]$Message) {
    throw "OPENSSL VERSION ASSERTION FAILED: $Message"
}

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$build = if ([IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir
} else {
    Join-Path $repo $BuildDir
}
$opensslRoot = Join-Path $repo "third_party\openssl-$ExpectedVersion"
$prebuilt = Join-Path $opensslRoot 'prebuilt\windows-x86_64-msvc'
$header = Join-Path $prebuilt 'include\openssl\opensslv.h'
$metadata = Join-Path $prebuilt '.dinero-build-meta'
$cache = Join-Path $build 'CMakeCache.txt'
$libcrypto = Join-Path $prebuilt 'libcrypto.lib'
$dinerod = Join-Path $build 'Release\dinerod.exe'

foreach ($required in @($header, $metadata, $cache, $libcrypto, $dinerod)) {
    if (-not (Test-Path -LiteralPath $required)) {
        Fail "required file is missing: $required"
    }
}

$headerText = Get-Content -LiteralPath $header -Raw
if ($headerText -notmatch "OPENSSL_FULL_VERSION_STR\s+`"$([regex]::Escape($ExpectedVersion))`"") {
    Fail "vendored header does not declare exactly $ExpectedVersion ($header)"
}

$metadataText = Get-Content -LiteralPath $metadata -Raw
if ($metadataText -notmatch "(?m)^OPENSSL_VERSION=$([regex]::Escape($ExpectedVersion))\s*$") {
    Fail "vendored metadata does not declare exactly $ExpectedVersion ($metadata)"
}

$cacheText = Get-Content -LiteralPath $cache -Raw
if ($cacheText -notmatch "(?m)^DINERO_VENDORED_OPENSSL_VERSION:[^=]*=$([regex]::Escape($ExpectedVersion))\s*$") {
    Fail "CMake did not resolve the policy pin $ExpectedVersion ($cache)"
}
if ($cacheText -notmatch "(?m)^USE_SYSTEM_OPENSSL:BOOL=OFF\s*$") {
    Fail "Windows release verification unexpectedly used system OpenSSL"
}

# The "one crypto baseline" policy governs *every* binary the release ships,
# not just dinerod. Auxiliary tools can drag in a different OpenSSL through a
# dynamic dependency — e.g. dinero-wallet-cli -> libcurl.dll -> a vcpkg
# libcrypto-3-x64.dll built against a different OpenSSL. The header/metadata/
# cache/dinerod checks above cannot see that second copy, so scan the actual
# bundled DLLs and EXEs too. Scan the installer stages (the definitive shipped
# payload) and the build Release dir.
$scanRoots = New-Object System.Collections.Generic.List[string]
$stage = Join-Path $repo 'packaging\windows\dist\server-installer-stage'
if (Test-Path -LiteralPath $stage) { $scanRoots.Add((Resolve-Path -LiteralPath $stage).Path) }
$userStage = Join-Path $repo 'packaging\windows\dist\installer-stage'
if (Test-Path -LiteralPath $userStage) { $scanRoots.Add((Resolve-Path -LiteralPath $userStage).Path) }
$releaseDir = Join-Path $build 'Release'
if (Test-Path -LiteralPath $releaseDir) { $scanRoots.Add((Resolve-Path -LiteralPath $releaseDir).Path) }
if ($scanRoots.Count -eq 0) {
    Fail "no bundled payload found to scan (looked for $stage, $userStage and $releaseDir)"
}

# Search raw bytes rather than relying on a platform-specific strings utility.
# The archive must contain exactly the pinned OpenSSL version. The final
# executable is decisive when a version object survives selective static
# archive extraction; absence there is permitted, matching the POSIX checker.
# For bundled DLLs/EXEs the same rule applies: exactly the baseline or none;
# any *other* OpenSSL version is a hard failure.
$python = @'
import pathlib, re, sys

expected = sys.argv[1]
expected_str = f"OpenSSL {expected}"
libcrypto = pathlib.Path(sys.argv[2]).read_bytes()
dinerod = pathlib.Path(sys.argv[3]).read_bytes()
scan_roots = sys.argv[4:]
pat = re.compile(rb"OpenSSL 3\.[0-9]+\.[0-9]+")

def versions(data):
    return sorted(set(x.decode() for x in pat.findall(data)))

lib_versions = versions(libcrypto)
if lib_versions != [expected_str]:
    raise SystemExit(f"libcrypto embeds {lib_versions or 'no version'}, expected exactly {expected_str}")

bin_versions = versions(dinerod)
if bin_versions and bin_versions != [expected_str]:
    raise SystemExit(f"dinerod embeds {bin_versions}, expected exactly {expected_str}")

print(f"libcrypto embeds exactly {expected_str}")
if bin_versions:
    print(f"dinerod embeds exactly {expected_str}")
else:
    print("dinerod embeds no OpenSSL version string; header, metadata, cache, and archive checks carry the guarantee")

seen = set()
offenders = []
scanned = 0
for root in scan_roots:
    for path in sorted(pathlib.Path(root).rglob("*")):
        if path.suffix.lower() not in (".dll", ".exe"):
            continue
        rp = path.resolve()
        if rp in seen:
            continue
        seen.add(rp)
        vers = versions(path.read_bytes())
        if not vers:
            continue
        scanned += 1
        if any(v != expected_str for v in vers):
            offenders.append(f"{path.name}: {', '.join(vers)}")

if offenders:
    raise SystemExit(
        f"bundled binaries embed a non-baseline OpenSSL (expected exactly {expected_str}):\n  "
        + "\n  ".join(sorted(set(offenders)))
    )
print(f"scanned bundled DLLs/EXEs across {len(scan_roots)} payload root(s); "
      f"{scanned} embed an OpenSSL version, all exactly {expected_str}")
'@

$python | python - $ExpectedVersion $libcrypto $dinerod @scanRoots
if ($LASTEXITCODE -ne 0) {
    Fail "raw archive/binary version inspection failed"
}

Write-Host "openssl version assertion OK: Windows build consumed exactly OpenSSL $ExpectedVersion (daemon + bundled payload)" -ForegroundColor Green
