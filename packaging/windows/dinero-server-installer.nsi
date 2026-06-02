; Dinero Windows Server installer (NSIS)
;
; Headless install for Windows server administrators. Installs the daemon stack
; to C:\Program Files\Dinero-Server\ and registers dinerod as a Windows Service.
; Parallel to dinero-installer.nsi, which is the Qt user lane.
;
; Build with:
;   makensis /DVERSION=8.0.0-rc27 dinero-server-installer.nsi
;
; Layout in server-installer-stage/:
;   dinerod.exe ... etc
;   vc_redist.x64.exe
;   optional runtime DLLs copied by build-server-installer.ps1
;   LICENSE
;
; Datadir convention: C:\ProgramData\Dinero. Created by the installer and
; writable by LocalSystem, which is the default service account.

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
!define APP_REGKEY     "Software\${APP_NAME}"
!define APP_UNINSTKEY  "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_DIRNAME}"

SetCompressor /SOLID lzma
SetCompressorDictSize 64

Name "${APP_NAME}"
OutFile "dist\Dinero-Server-${APP_VERSION}-windows-x86_64-Setup.exe"
InstallDir "$PROGRAMFILES64\${APP_DIRNAME}"
InstallDirRegKey HKLM "${APP_REGKEY}" "InstallDir"
RequestExecutionLevel admin
Unicode true

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
!include "LogicLib.nsh"
!insertmacro GetSize

Var AppDataDir

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

Function ResolveCommonDataDir
  ; With all-users shell context, $APPDATA resolves to the common appdata
  ; location (normally C:\ProgramData), not the installing user's profile.
  SetShellVarContext all
  StrCpy $AppDataDir "$APPDATA\Dinero"
FunctionEnd

Function IsVCRedistInstalled
  StrCpy $R0 "0"
  SetRegView 64
  ClearErrors
  ReadRegDWORD $0 HKLM "SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64" "Installed"
  ${IfNot} ${Errors}
  ${AndIf} $0 = 1
    StrCpy $R0 "1"
  ${EndIf}
FunctionEnd

Function EnsureVCRedist
  Call IsVCRedistInstalled
  ${If} $R0 = "1"
    DetailPrint "Microsoft Visual C++ runtime already installed."
    Return
  ${EndIf}

  IfFileExists "$INSTDIR\vc_redist.x64.exe" 0 missing_redist
    DetailPrint "Installing Microsoft Visual C++ 2015-2022 x64 Redistributable..."
    ExecWait '"$INSTDIR\vc_redist.x64.exe" /install /quiet /norestart' $0
    ${If} $0 = 0
    ${OrIf} $0 = 3010
    ${OrIf} $0 = 1638
      DetailPrint "Microsoft Visual C++ runtime installer completed with exit code $0."
      Return
    ${EndIf}

    MessageBox MB_ICONSTOP "Microsoft Visual C++ 2015-2022 x64 Redistributable installation failed with exit code $0. Dinero Server cannot start without the MSVC runtime."
    Abort

  missing_redist:
    MessageBox MB_ICONSTOP "Microsoft Visual C++ 2015-2022 x64 Redistributable is not installed, and vc_redist.x64.exe was not bundled in this installer. Install the official x64 redistributable from Microsoft, then rerun this installer."
    Abort
FunctionEnd

Section "Dinero Server (required)" SecCore
  SectionIn RO

  SetShellVarContext all
  SetRegView 64
  Call ResolveCommonDataDir

  ; Stop and remove any existing Dinerod service before overwriting binaries.
  ; Best-effort -- legitimate non-zero exits when the service is absent.
  nsExec::ExecToLog '"$SYSDIR\sc.exe" stop ${SVC_NAME}'
  Pop $0
  nsExec::ExecToLog '"$SYSDIR\sc.exe" delete ${SVC_NAME}'
  Pop $0

  SetOutPath "$INSTDIR"
  File /r "dist\server-installer-stage\*.*"

  Call EnsureVCRedist

  CreateDirectory "$AppDataDir"

  ; Register dinerod as a real Windows Service. The --service flag makes
  ; dinerod enter StartServiceCtrlDispatcher(), while --datadir points the
  ; LocalSystem service at the shared ProgramData location.
  ;
  ; sc create's binPath= value has nested quoting: a path-with-spaces wrapped
  ; in literal quotes ("C:\Program Files\...\dinerod.exe"), embedded inside
  ; the binPath= value which is itself an argv element wrapped in outer
  ; quotes. The embedded quotes MUST be backslash-escaped (\") so
  ; CommandLineToArgvW preserves them while still treating the outer pair as
  ; an argv delimiter -- without the backslash the parser sees consecutive
  ; "" and splits the binPath value across multiple argv elements, leaving
  ; sc.exe with an unparseable command. The original NSIS used $\" which
  ; emits a bare quote; this version uses \$\" to emit \" and wraps via
  ; cmd.exe /c for defense in depth (proven on the validation VPS). We also
  ; capture the exit code and Abort on failure so the silent-fail mode that
  ; bit the first-pass fix can't recur.
  DetailPrint "Registering ${SVC_NAME} Windows service..."
  nsExec::ExecToLog '"$SYSDIR\cmd.exe" /c sc create ${SVC_NAME} binPath= "\$\"$INSTDIR\dinerod.exe\$\" --service --datadir=\$\"$AppDataDir\$\"" start= auto DisplayName= "${SVC_DISPLAY}"'
  Pop $0
  ${If} $0 <> 0
    DetailPrint "sc create failed with exit code $0"
    MessageBox MB_ICONSTOP "Could not register the Dinerod Windows service (sc create exit code $0). The installer will abort.$\r$\n$\r$\nCommon causes:$\r$\n  - not running with administrator privileges$\r$\n  - antivirus blocking service registration$\r$\n  - leftover Dinerod service from a failed prior install (run 'sc.exe delete Dinerod' from an elevated prompt, then rerun this installer)"
    Abort
  ${EndIf}

  nsExec::ExecToLog '"$SYSDIR\sc.exe" description ${SVC_NAME} "${SVC_DESC}"'
  Pop $0
  nsExec::ExecToLog '"$SYSDIR\sc.exe" failure ${SVC_NAME} reset= 86400 actions= restart/5000/restart/5000/restart/30000'
  Pop $0

  DetailPrint "Starting ${SVC_NAME} service..."
  nsExec::ExecToLog '"$SYSDIR\sc.exe" start ${SVC_NAME}'
  Pop $0
  ${If} $0 <> 0
    DetailPrint "sc start returned exit code $0 (service is registered; start can be retried via 'sc start ${SVC_NAME}' or services.msc)"
  ${EndIf}

  WriteRegStr HKLM "${APP_REGKEY}" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "${APP_REGKEY}" "Version"    "${APP_VERSION}"
  WriteRegStr HKLM "${APP_REGKEY}" "Datadir"    "$AppDataDir"
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

Section "Uninstall"
  SetShellVarContext all
  SetRegView 64

  ; Stop the service and wait for STOPPED before delete + RMDir, otherwise
  ; dinerod.exe file handles can still be open when RMDir tries to clear the
  ; install dir and the dir is left behind. (Observed in rc27-fix testing:
  ; service in STOP_PENDING raced with the uninstaller's RMDir.)
  nsExec::ExecToLog '"$SYSDIR\sc.exe" stop ${SVC_NAME}'
  Pop $0
  Sleep 5000
  nsExec::ExecToLog '"$SYSDIR\sc.exe" delete ${SVC_NAME}'
  Pop $0
  Sleep 1000

  Delete "$INSTDIR\Uninstall.exe"
  RMDir /r "$INSTDIR"

  DeleteRegKey HKLM "${APP_REGKEY}"
  DeleteRegKey HKLM "${APP_UNINSTKEY}"

  ; Wallet and chain state under C:\ProgramData\Dinero are intentionally
  ; preserved across uninstall/reinstall.
SectionEnd
