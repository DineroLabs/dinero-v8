; Dinero Cryptocurrency Windows Installer
; Creates a complete Windows installer with all dependencies

!define PRODUCT_NAME "Dinero Cryptocurrency"
!define PRODUCT_VERSION "1.0.0"
!define PRODUCT_PUBLISHER "Dinero Project"
!define PRODUCT_WEB_SITE "https://github.com/your-org/DineroCoin"
!define PRODUCT_DIR_REGKEY "Software\Microsoft\Windows\CurrentVersion\App Paths\dinero-qt6.exe"
!define PRODUCT_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}"
!define PRODUCT_UNINST_ROOT_KEY "HKLM"

; Modern UI
!include "MUI2.nsh"

; General
Name "${PRODUCT_NAME} ${PRODUCT_VERSION}"
OutFile "DineroCoin-${PRODUCT_VERSION}-Windows-Setup.exe"
InstallDir "$PROGRAMFILES64\Dinero"
InstallDirRegKey HKLM "${PRODUCT_DIR_REGKEY}" ""
ShowInstDetails show
ShowUnInstDetails show

; Request admin privileges
RequestExecutionLevel admin

; Interface Settings
!define MUI_ABORTWARNING
!define MUI_ICON "..\icons\Dinero-Coin.ico"
!define MUI_UNICON "..\icons\Dinero-Coin.ico"

; Welcome page
!insertmacro MUI_PAGE_WELCOME

; License page
!insertmacro MUI_PAGE_LICENSE "..\LICENSE"

; Directory page
!insertmacro MUI_PAGE_DIRECTORY

; Components page
!insertmacro MUI_PAGE_COMPONENTS

; Instfiles page
!insertmacro MUI_PAGE_INSTFILES

; Finish page
!define MUI_FINISHPAGE_RUN "$INSTDIR\dinero-qt6.exe"
!define MUI_FINISHPAGE_SHOWREADME "$INSTDIR\README.txt"
!insertmacro MUI_PAGE_FINISH

; Uninstaller pages
!insertmacro MUI_UNPAGE_INSTFILES

; Language files
!insertmacro MUI_LANGUAGE "English"

; Reserve files
!insertmacro MUI_RESERVEFILE_LANGDLL

Section "Core Application" SEC01
  SectionIn RO
  
  ; Set output path to the installation directory
  SetOutPath "$INSTDIR"
  
  ; Core binaries
  File "..\build\bin\Release\dinero-qt6.exe"
  File "..\build\bin\Release\dinerod.exe"
  File "..\build\bin\Release\dinero-cli.exe"
  
  ; Qt6 DLLs (if not statically linked)
  File /nonfatal "..\build\bin\Release\Qt6Core.dll"
  File /nonfatal "..\build\bin\Release\Qt6Gui.dll"
  File /nonfatal "..\build\bin\Release\Qt6Widgets.dll"
  File /nonfatal "..\build\bin\Release\Qt6Network.dll"
  
  ; Visual C++ Redistributable (if needed)
  File /nonfatal "vcredist_x64.exe"
  
  ; Documentation
  File "..\README.md"
  File "..\LICENSE"
  
  ; Create data directory
  CreateDirectory "$APPDATA\DineroCoin"
  
  ; Create shortcuts
  CreateDirectory "$SMPROGRAMS\Dinero"
  CreateShortCut "$SMPROGRAMS\Dinero\Dinero Wallet.lnk" "$INSTDIR\dinero-qt6.exe"
  CreateShortCut "$SMPROGRAMS\Dinero\Uninstall.lnk" "$INSTDIR\uninst.exe"
  CreateShortCut "$DESKTOP\Dinero Wallet.lnk" "$INSTDIR\dinero-qt6.exe"
  
  ; Registry entries
  WriteRegStr HKLM "${PRODUCT_DIR_REGKEY}" "" "$INSTDIR\dinero-qt6.exe"
  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "DisplayName" "$(^Name)"
  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "UninstallString" "$INSTDIR\uninst.exe"
  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "DisplayIcon" "$INSTDIR\dinero-qt6.exe"
  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "DisplayVersion" "${PRODUCT_VERSION}"
  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "URLInfoAbout" "${PRODUCT_WEB_SITE}"
  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "Publisher" "${PRODUCT_PUBLISHER}"
  
  ; File associations
  WriteRegStr HKCR ".din" "" "DineroWallet"
  WriteRegStr HKCR "DineroWallet" "" "Dinero Wallet File"
  WriteRegStr HKCR "DineroWallet\DefaultIcon" "" "$INSTDIR\dinero-qt6.exe,0"
  WriteRegStr HKCR "DineroWallet\shell\open\command" "" '"$INSTDIR\dinero-qt6.exe" "%1"'
  
SectionEnd

Section "Desktop Integration" SEC02
  ; Additional desktop integration
  WriteRegStr HKLM "SOFTWARE\RegisteredApplications" "Dinero" "Software\Dinero\Capabilities"
  WriteRegStr HKLM "SOFTWARE\Dinero\Capabilities" "ApplicationName" "Dinero Cryptocurrency"
  WriteRegStr HKLM "SOFTWARE\Dinero\Capabilities" "ApplicationDescription" "Complete mining node and wallet for Dinero cryptocurrency"
SectionEnd

Section "Windows Service" SEC03
  ; Optional: Install daemon as Windows service
  DetailPrint "Installing Dinero daemon service..."
  ExecWait '"$INSTDIR\dinerod.exe" -install-service'
SectionEnd

; Section descriptions
!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC01} "Core Dinero application files (required)"
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC02} "Desktop integration and file associations"
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC03} "Install daemon as Windows service (optional)"
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section Uninstall
  ; Remove registry keys
  DeleteRegKey ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}"
  DeleteRegKey HKLM "${PRODUCT_DIR_REGKEY}"
  DeleteRegKey HKCR ".din"
  DeleteRegKey HKCR "DineroWallet"
  
  ; Remove files and uninstaller
  Delete "$INSTDIR\dinero-qt6.exe"
  Delete "$INSTDIR\dinerod.exe"
  Delete "$INSTDIR\dinero-cli.exe"
  Delete "$INSTDIR\*.dll"
  Delete "$INSTDIR\README.md"
  Delete "$INSTDIR\LICENSE"
  Delete "$INSTDIR\uninst.exe"
  
  ; Remove shortcuts
  Delete "$SMPROGRAMS\Dinero\*.*"
  Delete "$DESKTOP\Dinero Wallet.lnk"
  
  ; Remove directories
  RMDir "$SMPROGRAMS\Dinero"
  RMDir "$INSTDIR"
  
  ; Note: We don't remove $APPDATA\DineroCoin to preserve user data
  
  SetAutoClose true
SectionEnd
