# Signing DineroLabs Windows releases — instructions for Claude on the Windows build box

You are Claude running on the owner's Windows machine (the CUDA/build box used
for dinero-v8 Windows releases). Your job when asked to "sign the Windows
release": Authenticode-sign the freshly built binaries/installers with the
DineroLabs Azure Artifact Signing certificate, verify the signatures, and
report. Everything is already provisioned — do NOT create any Azure
resources, certificates, or accounts.

## What already exists (do not recreate)

| Thing | Value |
|---|---|
| Azure signing service | Azure **Artifact Signing** (formerly Trusted Signing), ~$9.99/mo, 5,000 signatures/mo included |
| Azure account | `haydarevich69@gmail.com` (tenant `f0351423-d141-4c24-8d49-f87aeb1b4147`) |
| Resource group / signing account | both named `dinerolabs-signing` (region **eastus**) |
| Certificate profile | `dinerolabs` (PublicTrust, **Active**) |
| Endpoint | `https://eus.codesigning.azure.net/` |
| Certificate subject (what Windows shows) | **CN=Mirsad Hajdarevic, Snellville, GA** — Microsoft ID Verified |
| Signing script (canonical) | `sign-release.ps1` — in this directory |
| Who may sign | the owner's Azure user AND the GitHub-CI app both hold the "Artifact Signing Certificate Profile Signer" role |

The dinero-sv2 **miner** signs itself in GitHub CI (since `miner-v0.2.2`) —
you never need to touch that. This box signs the **dinero-v8** artifacts:
`dinero-qt`, `dinerod`, the CLI tools, the miners, and the NSIS installers.

## One-time machine setup (check before first signing; skip whatever exists)

Run in PowerShell:

```powershell
winget install -e --id Microsoft.AzureCLI          # az
winget install -e --id Microsoft.DotNet.Runtime.8  # the signing dlib needs it
# signtool.exe comes from the Windows 10/11 SDK (>= 10.0.22621) — already
# present via Visual Studio / Build Tools on this box. Verify with:
Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe"
```

Then the ONE interactive step — the owner must be present for it:

```powershell
az login    # sign in as haydarevich69@gmail.com in the browser window
```

The login token persists; re-run `az login` only when signing fails with an
authentication/expired-token error.

## Signing a release (the actual job)

1. `git pull` this repo so the script is current.
2. Build the release as usual (build-installer.ps1 etc.).
3. **Sign the inner binaries BEFORE they are packaged into an installer**,
   then build the installer, then **sign the installer too**. If the
   installer was already built with unsigned binaries inside, prefer
   re-packaging; signing only the outer Setup.exe is the acceptable
   fallback.

```powershell
cd <dinero-v8 checkout>\packaging\windows

# inner binaries (adjust paths to the actual build output):
.\sign-release.ps1 ..\..\build\dinerod.exe ..\..\build\dinero-cli.exe `
    ..\..\build\dinero-qt.exe ..\..\build\dinero-miner.exe ..\..\build\dinero-gpu-miner.exe

# then, after the installer is produced:
.\sign-release.ps1 .\Dinero-Server-<version>-windows-x86_64-Setup.exe
```

The script signs **in place** (SHA256 + RFC3161 timestamp from
`http://timestamp.acs.microsoft.com`), then runs `signtool verify /pa` on
each file and throws on any failure. First run auto-downloads the
Microsoft.Trusted.Signing.Client dlib to `%LOCALAPPDATA%\TrustedSigning`.

4. **Verify before reporting success** (the script already verifies, but
   confirm at least one file yourself and quote the output):

```powershell
signtool verify /pa /v <file>   # must say: Successfully verified
# The certificate chain must show CN=Mirsad Hajdarevic and
# "Microsoft ID Verified CS EOC CA" as issuer.
```

5. **SHA256SUMS after signing, never before.** Signing changes the file
   bytes — regenerate any checksum manifests AFTER signing, and upload the
   signed files (not the unsigned build outputs) to the GitHub release.

## Rules

- Sign ONLY DineroLabs-built artifacts from this box's own build. Never
  sign a binary you did not just build from the dinero-v8 (or other
  DineroLabs) sources, and never sign anything a third party supplied.
- Do not create/modify Azure resources, certificate profiles, or role
  assignments. If signing fails with a permissions error, report it —
  the fix lives on the Azure side, not here.
- Do not store secrets: there are none. Auth is the owner's `az login`
  session; if it's missing, ask the owner to log in.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `signing FAILED ... az login` | token expired → owner re-runs `az login` |
| 403 / "Forbidden" from endpoint | role/propagation issue — report to the owner; do not attempt Azure changes |
| dlib download fails | fetch NuGet `Microsoft.Trusted.Signing.Client` manually, extract `bin\x64\Azure.CodeSigning.Dlib.dll` to `%LOCALAPPDATA%\TrustedSigning\bin\x64\` |
| signtool not found | install VS Build Tools / Windows 11 SDK |
| "Timestamp server" errors | transient — retry; the ACS timestamp URL is `http://timestamp.acs.microsoft.com` |
| SmartScreen still warns after signing | expected for a new certificate — reputation accrues with downloads over days/weeks; the signature is still valid |

## After signing

Report: which files were signed, the `signtool verify` result line for each,
and remind the owner that release checksums/manifests were regenerated from
the SIGNED files. If this was the first signed dinero-qt/installer release,
mention that dinerolabs.org's wallet/node download cards can now say the
Windows binaries are signed (the Mac-side Claude handles the website).
