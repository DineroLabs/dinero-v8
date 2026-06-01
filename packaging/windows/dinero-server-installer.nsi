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
  ExecWait '"$SYSDIR\sc.exe" stop ${SVC_NAME}'
  ExecWait '"$SYSDIR\sc.exe" delete ${SVC_NAME}'

  SetOutPath "$INSTDIR"
  File /r "dist\server-installer-stage\*.*"

  Call EnsureVCRedist

  CreateDirectory "$AppDataDir"

  ; Register dinerod as a real Windows Service. The --service flag makes
  ; dinerod enter StartServiceCtrlDispatcher(), while --datadir points the
  ; LocalSystem service at the shared ProgramData location.
  ExecWait '"$SYSDIR\sc.exe" create ${SVC_NAME} binPath= "$\"$INSTDIR\dinerod.exe$\" --service --datadir=$\"$AppDataDir$\"" start= auto DisplayName= "${SVC_DISPLAY}"'
  ExecWait '"$SYSDIR\sc.exe" description ${SVC_NAME} "${SVC_DESC}"'
  ExecWait '"$SYSDIR\sc.exe" failure ${SVC_NAME} reset= 86400 actions= restart/5000/restart/5000/restart/30000'
  ExecWait '"$SYSDIR\sc.exe" start ${SVC_NAME}'

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

  ExecWait '"$SYSDIR\sc.exe" stop ${SVC_NAME}'
  ExecWait '"$SYSDIR\sc.exe" delete ${SVC_NAME}'

  Delete "$INSTDIR\Uninstall.exe"
  RMDir /r "$INSTDIR"

  DeleteRegKey HKLM "${APP_REGKEY}"
  DeleteRegKey HKLM "${APP_UNINSTKEY}"

  ; Wallet and chain state under C:\ProgramData\Dinero are intentionally
  ; preserved across uninstall/reinstall.
SectionEnd
