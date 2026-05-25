; Dinero Windows Server installer (NSIS)
;
; Headless install for Windows server administrators. Installs only the
; daemon stack (no Qt GUI, no dinero-solo-miner) to C:\Program Files\
; Dinero-Server\ and registers dinerod as a Windows Service set to
; auto-start. Parallel to dinero-installer.nsi (which is the user lane).
;
; Build with:
;   makensis /DVERSION=8.0.0-rc3 dinero-server-installer.nsi
;
; Layout in server-installer-stage/:
;   dinerod.exe ... etc       7 daemon/operator binaries
;   libcurl.dll, libcrypto-3-x64.dll, libssl-3-x64.dll, z.dll  (vcpkg runtime)
;   LICENSE
;
; Datadir convention: C:\ProgramData\Dinero. Created by the installer
; (writable by LocalSystem under which the service runs).

!ifndef VERSION
  !define VERSION "0.0.0-dev"
!endif

!define APP_NAME       "Dinero Server"
!define APP_DIRNAME    "Dinero-Server"
!define APP_VERSION    "${VERSION}"
!define APP_PUBLISHER  "DineroLabs"
!define APP_URL        "https://github.com/DineroLabs/dinero-v8"
!define SVC_NAME       "Dinerod"
!define SVC_DISPLAY    "Dinero Full Node"
!define SVC_DESC       "Dinero blockchain full node daemon. Runs the dinerod RPC + peer-to-peer node as a Windows service."
!define APP_DATADIR    "$COMMONAPPDATA\Dinero"
!define APP_REGKEY     "Software\${APP_NAME}"
!define APP_UNINSTKEY  "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_DIRNAME}"

; LZMA solid compression: same single-stream LZMA across all files,
; gives the highest ratio. Slow to build, fast to extract.
SetCompressor /SOLID lzma
SetCompressorDictSize 64

Name "${APP_NAME}"
OutFile "dist\Dinero-Server-${APP_VERSION}-windows-x86_64-Setup.exe"
InstallDir "$PROGRAMFILES64\${APP_DIRNAME}"
InstallDirRegKey HKLM "${APP_REGKEY}" "InstallDir"
RequestExecutionLevel admin
Unicode true

; Embedded file metadata — shows up in right-click installer.exe ->
; Properties -> Details. Authenticode signing (not yet wired up) is what
; controls the UAC dialog's "Verified publisher" line — VIAddVersionKey
; below does not.
;
; VIProductVersion requires X.Y.Z.W numeric format (no -rc suffix).
VIProductVersion "8.0.0.0"
VIFileVersion    "8.0.0.0"
VIAddVersionKey "ProductName"      "${APP_NAME}"
VIAddVersionKey "CompanyName"      "${APP_PUBLISHER}"
VIAddVersionKey "LegalCopyright"   "Copyright (C) 2026 ${APP_PUBLISHER}"
VIAddVersionKey "FileDescription"  "${APP_NAME} Installer"
VIAddVersionKey "FileVersion"      "${APP_VERSION}"
VIAddVersionKey "ProductVersion"   "${APP_VERSION}"
VIAddVersionKey "InternalName"     "Dinero-Server-Setup"
VIAddVersionKey "OriginalFilename" "Dinero-Server-${APP_VERSION}-windows-x86_64-Setup.exe"

!include "MUI2.nsh"
!include "FileFunc.nsh"
!insertmacro GetSize

!define MUI_ABORTWARNING

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "..\..\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_WELCOME
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH

!insertmacro MUI_LANGUAGE "English"

Section "Dinero Server (required)" SecCore
  SectionIn RO

  SetShellVarContext all
  SetRegView 64

  ; Stop and remove any existing Dinerod service from a prior install so
  ; we can overwrite the binaries without a "file in use" error.
  ExecWait '"$SYSDIR\sc.exe" stop ${SVC_NAME}'
  ExecWait '"$SYSDIR\sc.exe" delete ${SVC_NAME}'

  SetOutPath "$INSTDIR"
  File /r "dist\server-installer-stage\*.*"

  ; Datadir under ProgramData. LocalSystem (the service account) has
  ; full access by default. Wallets and blockchain data live here so
  ; uninstall preserves it the same way the user installer preserves
  ; %APPDATA%\Dinero.
  CreateDirectory "${APP_DATADIR}"

  ; Register dinerod as a Windows Service:
  ;   - auto-start (start= auto)
  ;   - LocalSystem account (default, no obj= flag)
  ;   - binPath includes the -datadir flag so the service knows where
  ;     to read/write chain + wallet state (LocalSystem's %APPDATA% is
  ;     under \System32\config\systemprofile which is confusing; the
  ;     ProgramData path is the canonical Windows-Service convention).
  ExecWait '"$SYSDIR\sc.exe" create ${SVC_NAME} binPath= "\"$INSTDIR\dinerod.exe\" -datadir=\"${APP_DATADIR}\"" start= auto DisplayName= "${SVC_DISPLAY}"'
  ExecWait '"$SYSDIR\sc.exe" description ${SVC_NAME} "${SVC_DESC}"'
  ; Configure service failure recovery: restart on first three failures,
  ; reset failure counter after 1 day.
  ExecWait '"$SYSDIR\sc.exe" failure ${SVC_NAME} reset= 86400 actions= restart/5000/restart/5000/restart/30000'
  ; Start it now.
  ExecWait '"$SYSDIR\sc.exe" start ${SVC_NAME}'

  ; Registry: own key + Add/Remove Programs entry
  WriteRegStr HKLM "${APP_REGKEY}" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "${APP_REGKEY}" "Version"    "${APP_VERSION}"
  WriteRegStr HKLM "${APP_REGKEY}" "Datadir"    "${APP_DATADIR}"
  WriteRegStr HKLM "${APP_REGKEY}" "ServiceName" "${SVC_NAME}"

  WriteRegStr   HKLM "${APP_UNINSTKEY}" "DisplayName"     "${APP_NAME}"
  WriteRegStr   HKLM "${APP_UNINSTKEY}" "DisplayVersion"  "${APP_VERSION}"
  WriteRegStr   HKLM "${APP_UNINSTKEY}" "DisplayIcon"     "$INSTDIR\dinerod.exe"
  WriteRegStr   HKLM "${APP_UNINSTKEY}" "Publisher"       "${APP_PUBLISHER}"
  WriteRegStr   HKLM "${APP_UNINSTKEY}" "URLInfoAbout"    "${APP_URL}"
  WriteRegStr   HKLM "${APP_UNINSTKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr   HKLM "${APP_UNINSTKEY}" "UninstallString" "$INSTDIR\Uninstall.exe"
  WriteRegStr   HKLM "${APP_UNINSTKEY}" "QuietUninstallString" "$\"$INSTDIR\Uninstall.exe$\" /S"
  WriteRegDWORD HKLM "${APP_UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${APP_UNINSTKEY}" "NoRepair" 1

  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKLM "${APP_UNINSTKEY}" "EstimatedSize" "$0"

  WriteUninstaller "$INSTDIR\Uninstall.exe"
SectionEnd

LangString DESC_SecCore ${LANG_ENGLISH} "Daemon stack (dinerod + dinero-cli + miners + wallet-cli + seeder). Registers dinerod as a Windows Service (auto-start)."

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecCore} $(DESC_SecCore)
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; ====================================================================
; Uninstall section
; ====================================================================
;
; Removes:
;   - The Dinerod Windows Service (stop + delete)
;   - $INSTDIR (the program files we installed)
;   - Registry keys (own + Add/Remove)
;
; Preserves:
;   - ${APP_DATADIR} (C:\ProgramData\Dinero) — never touched by
;     uninstall so wallets + blockchain data survive reinstalls.

Section "Uninstall"
  SetShellVarContext all
  SetRegView 64

  ; Stop + delete the service before removing the binary, otherwise
  ; dinerod.exe is locked by the SCM and RMDir /r fails.
  ExecWait '"$SYSDIR\sc.exe" stop ${SVC_NAME}'
  ExecWait '"$SYSDIR\sc.exe" delete ${SVC_NAME}'

  Delete "$INSTDIR\Uninstall.exe"

  ; Blow away the install dir. ${APP_DATADIR} lives under ProgramData
  ; (not under $INSTDIR) so wallet + chain data survive.
  RMDir /r "$INSTDIR"

  ; Registry
  DeleteRegKey HKLM "${APP_REGKEY}"
  DeleteRegKey HKLM "${APP_UNINSTKEY}"

  ; NOTE: We intentionally do NOT remove ${APP_DATADIR} —
  ; wallet + chain state survives uninstall so reinstall/upgrade
  ; doesn't lose data. Operators who want a full purge can delete
  ; C:\ProgramData\Dinero manually.
SectionEnd
