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

# Search raw bytes rather than relying on a platform-specific strings utility.
# The archive must contain exactly the pinned OpenSSL version. The final
# executable is decisive when a version object survives selective static
# archive extraction; absence there is permitted, matching the POSIX checker.
$python = @'
import pathlib, re, sys

expected = sys.argv[1].encode()
libcrypto = pathlib.Path(sys.argv[2]).read_bytes()
dinerod = pathlib.Path(sys.argv[3]).read_bytes()
pat = re.compile(rb"OpenSSL 3\.[0-9]+\.[0-9]+")

lib_versions = sorted(set(x.decode() for x in pat.findall(libcrypto)))
if lib_versions != [f"OpenSSL {expected.decode()}"]:
    raise SystemExit(
        f"libcrypto embeds {lib_versions or 'no version'}, expected exactly OpenSSL {expected.decode()}"
    )

bin_versions = sorted(set(x.decode() for x in pat.findall(dinerod)))
if bin_versions and bin_versions != [f"OpenSSL {expected.decode()}"]:
    raise SystemExit(
        f"dinerod embeds {bin_versions}, expected exactly OpenSSL {expected.decode()}"
    )
print(f"libcrypto embeds exactly OpenSSL {expected.decode()}")
if bin_versions:
    print(f"dinerod embeds exactly OpenSSL {expected.decode()}")
else:
    print("dinerod embeds no OpenSSL version string; header, metadata, cache, and archive checks carry the guarantee")
'@

$python | python - $ExpectedVersion $libcrypto $dinerod
if ($LASTEXITCODE -ne 0) {
    Fail "raw archive/binary version inspection failed"
}

Write-Host "openssl version assertion OK: Windows build consumed exactly OpenSSL $ExpectedVersion" -ForegroundColor Green
