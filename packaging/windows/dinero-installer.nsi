; Dinero Windows installer (NSIS)
;
; One-click installer that bundles the Qt6 GUI, all daemon binaries,
; and the Qt runtime DLLs into C:\Program Files\Dinero\.
;
; Build with:
;   makensis /DVERSION=8.0.0-rc2 dinero-installer.nsi
;
; Layout in installer-stage/:
;   dinero-qt.exe             main GUI
;   dinerod.exe ... etc       daemon binaries
;   Qt6Core.dll, Qt6Gui.dll, Qt6Widgets.dll, Qt6Network.dll, ...
;   platforms/qwindows.dll    Qt platform plugin (required at runtime)
;   imageformats/, styles/, tls/, networkinformation/
;   LICENSE

!ifndef VERSION
  !define VERSION "0.0.0-dev"
!endif

!ifndef VERSION_NUMERIC
  !define VERSION_NUMERIC "0.0.0.0"
!endif

!define APP_NAME       "Dinero"
!define APP_VERSION    "${VERSION}"
!define APP_PUBLISHER  "DineroLabs"
!define APP_URL        "https://github.com/DineroLabs/dinero-v8"
!define APP_EXE        "dinero-qt.exe"
!define APP_REGKEY     "Software\${APP_NAME}"
!define APP_UNINSTKEY  "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}"

; LZMA solid compression: same single-stream LZMA across all files,
; gives the highest ratio. Slow to build, fast to extract.
SetCompressor /SOLID lzma
SetCompressorDictSize 64

Name "${APP_NAME}"
OutFile "dist\Dinero-${APP_VERSION}-windows-x86_64-Setup.exe"
InstallDir "$PROGRAMFILES64\${APP_NAME}"
InstallDirRegKey HKLM "${APP_REGKEY}" "InstallDir"
RequestExecutionLevel admin
Unicode true

; Embedded file metadata — shows up in:
;   - right-click installer.exe -> Properties -> Details ("Company: DineroLabs")
;   - Process Explorer / Task Manager publisher column
;   - shdocvw / Get-Item PowerShell metadata
; Note: this does NOT change the UAC dialog's "Verified publisher" /
; "Unknown publisher" line. That line is gated by Authenticode code
; signing (signtool + a code-signing certificate from a CA). Without
; signing, UAC always shows "Unknown publisher" no matter what
; metadata is embedded.
;
; VIProductVersion requires X.Y.Z.W numeric format (no -rc suffix).
VIProductVersion "${VERSION_NUMERIC}"
VIFileVersion    "${VERSION_NUMERIC}"
VIAddVersionKey "ProductName"      "${APP_NAME}"
VIAddVersionKey "CompanyName"      "${APP_PUBLISHER}"
VIAddVersionKey "LegalCopyright"   "Copyright (C) 2026 ${APP_PUBLISHER}"
VIAddVersionKey "FileDescription"  "${APP_NAME} Installer"
VIAddVersionKey "FileVersion"      "${APP_VERSION}"
VIAddVersionKey "ProductVersion"   "${APP_VERSION}"
VIAddVersionKey "InternalName"     "Dinero-Setup"
VIAddVersionKey "OriginalFilename" "Dinero-${APP_VERSION}-windows-x86_64-Setup.exe"

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"
!insertmacro GetSize

; Ensure the Microsoft Visual C++ 2015-2022 x64 Redistributable is present.
; Qt6Core.dll and dinero-qt.exe depend on VCRUNTIME140/MSVCP140; on a clean
; machine without the redistributable they fail to load with a "Qt6Core.dll"
; error — which the finish-page "Launch Dinero" surfaces right after install.
; Mirrors dinero-server-installer.nsi.
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

    MessageBox MB_ICONSTOP "Microsoft Visual C++ 2015-2022 x64 Redistributable installation failed with exit code $0. Dinero cannot start without the MSVC runtime."
    Abort

  missing_redist:
    MessageBox MB_ICONSTOP "Microsoft Visual C++ 2015-2022 x64 Redistributable is not installed, and vc_redist.x64.exe was not bundled in this installer. Install the official x64 redistributable from Microsoft, then rerun this installer."
    Abort
FunctionEnd

!define MUI_ABORTWARNING

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "..\..\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Launch Dinero"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_WELCOME
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH

!insertmacro MUI_LANGUAGE "English"

Section "Dinero (required)" SecCore
  SectionIn RO

  ; All-users install: write to HKLM + the system-wide Start Menu so
  ; every user on the box sees the shortcut + Add/Remove entry. Without
  ; this, NSIS defaults to the elevating user's per-user context
  ; (HKCU + $APPDATA\Microsoft\Windows\Start Menu) which is wrong for
  ; an installer that already requested admin via RequestExecutionLevel.
  SetShellVarContext all

  ; 64-bit registry view: NSIS itself runs as a 32-bit process so
  ; HKLM writes are redirected to HKLM\Software\WOW6432Node\... by
  ; default. Our payload is 64-bit (dinerod/dinero-qt are x86_64),
  ; so the Add/Remove entry belongs in the 64-bit hive where 64-bit
  ; readers expect it. Add/Remove Programs reads both views and
  ; aggregates, but only the 64-bit hive is the canonical home.
  SetRegView 64

  SetOutPath "$INSTDIR"
  File /r "dist\installer-stage\*.*"

  ; Install the MSVC runtime before any launch, or Qt6Core.dll won't load
  ; on a clean machine. vc_redist.x64.exe is bundled into $INSTDIR above.
  Call EnsureVCRedist

  ; Start Menu
  CreateDirectory "$SMPROGRAMS\${APP_NAME}"
  CreateShortcut "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${APP_EXE}" 0
  CreateShortcut "$SMPROGRAMS\${APP_NAME}\Uninstall ${APP_NAME}.lnk" "$INSTDIR\Uninstall.exe"

  ; Registry: own key + Add/Remove Programs entry
  WriteRegStr HKLM "${APP_REGKEY}" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "${APP_REGKEY}" "Version"    "${APP_VERSION}"

  WriteRegStr   HKLM "${APP_UNINSTKEY}" "DisplayName"     "${APP_NAME}"
  WriteRegStr   HKLM "${APP_UNINSTKEY}" "DisplayVersion"  "${APP_VERSION}"
  WriteRegStr   HKLM "${APP_UNINSTKEY}" "DisplayIcon"     "$INSTDIR\${APP_EXE}"
  WriteRegStr   HKLM "${APP_UNINSTKEY}" "Publisher"       "${APP_PUBLISHER}"
  WriteRegStr   HKLM "${APP_UNINSTKEY}" "URLInfoAbout"    "${APP_URL}"
  WriteRegStr   HKLM "${APP_UNINSTKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr   HKLM "${APP_UNINSTKEY}" "UninstallString" "$INSTDIR\Uninstall.exe"
  WriteRegStr   HKLM "${APP_UNINSTKEY}" "QuietUninstallString" "$\"$INSTDIR\Uninstall.exe$\" /S"
  WriteRegDWORD HKLM "${APP_UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${APP_UNINSTKEY}" "NoRepair" 1

  ; Estimated install size, for Add/Remove Programs display
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKLM "${APP_UNINSTKEY}" "EstimatedSize" "$0"

  WriteUninstaller "$INSTDIR\Uninstall.exe"
SectionEnd

Section "Desktop shortcut" SecDesktop
  SetShellVarContext all
  CreateShortcut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${APP_EXE}" 0
SectionEnd

LangString DESC_SecCore    ${LANG_ENGLISH} "Core install: GUI, daemon, miners, wallet CLI, Qt runtime."
LangString DESC_SecDesktop ${LANG_ENGLISH} "Add a shortcut to the desktop."

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecCore}    $(DESC_SecCore)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} $(DESC_SecDesktop)
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; ====================================================================
; Uninstall section
; ====================================================================
;
; Removes:
;   - $INSTDIR (the program files we installed)
;   - Start Menu group
;   - Desktop shortcut (if it exists)
;   - Registry keys (own + Add/Remove)
;
; Preserves:
;   - %APPDATA%\Dinero (wallet datadir) — never touched by uninstall
;     so users don't lose their wallets when reinstalling/upgrading.

Section "Uninstall"
  ; Match the install-side context so HKLM/All Users paths are used.
  SetShellVarContext all
  SetRegView 64

  Delete "$INSTDIR\Uninstall.exe"

  ; windeployqt drops a half-dozen Qt plugin subdirs (platforms,
  ; imageformats, styles, tls, networkinformation, iconengines,
  ; generic, plus possibly more depending on Qt version). Rather
  ; than enumerate them all and risk leaving one behind, blow the
  ; whole InstallDir away. Standard pattern used by Bitcoin Core
  ; and every Windows app installer.
  ;
  ; Safety: $INSTDIR is set by the install registry write at install
  ; time and read back via InstallDirRegKey, so it points only at our
  ; own dedicated dir. We never installed to a shared path.
  RMDir /r "$INSTDIR"

  ; Start Menu
  Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
  Delete "$SMPROGRAMS\${APP_NAME}\Uninstall ${APP_NAME}.lnk"
  RMDir "$SMPROGRAMS\${APP_NAME}"

  ; Desktop (may not exist if the user unchecked the option)
  Delete "$DESKTOP\${APP_NAME}.lnk"

  ; Registry
  DeleteRegKey HKLM "${APP_REGKEY}"
  DeleteRegKey HKLM "${APP_UNINSTKEY}"

  ; NOTE: We intentionally do NOT remove %APPDATA%\${APP_NAME} —
  ; the wallet datadir survives uninstall so users can reinstall or
  ; upgrade without losing wallets.
SectionEnd
