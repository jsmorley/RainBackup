Unicode True
!include "MUI2.nsh"
!include "x64.nsh"
!include "Sections.nsh"

!ifndef RAINBACKUP_EXE
!define RAINBACKUP_EXE "..\build\Release\RainBackup.exe"
!endif

Name "RainBackup"
VIProductVersion "2.4.0.0"
VIAddVersionKey /LANG=1033 "ProductName" "RainBackup"
VIAddVersionKey /LANG=1033 "ProductVersion" "2.4.0"
VIAddVersionKey /LANG=1033 "FileDescription" "RainBackup Setup"
VIAddVersionKey /LANG=1033 "FileVersion" "2.4.0"
VIAddVersionKey /LANG=1033 "LegalCopyright" "Copyright Jeffrey Morley"
OutFile "..\dist\RainBackup-Setup.exe"
InstallDir "$PROGRAMFILES64\RainBackup"
InstallDirRegKey HKCU "Software\RainBackup" "InstallDir"
RequestExecutionLevel admin

!define MUI_ICON "..\src\RainBackup.ico"
!define MUI_ABORTWARNING
!define MUI_COMPONENTSPAGE_NODESC
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE ConfirmInstall
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "Launch RainBackup"
!define MUI_FINISHPAGE_RUN_FUNCTION LaunchRainBackup
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Function LaunchRainBackup
    ; Start through the desktop shell so RainBackup uses the signed-in user's settings.
    ClearErrors
    Exec '"$WINDIR\explorer.exe" "$INSTDIR\RainBackup.exe"'
    IfErrors 0 done
    MessageBox MB_OK|MB_ICONEXCLAMATION "RainBackup was installed. You can open it from:$\r$\n$INSTDIR\RainBackup.exe"
done:
FunctionEnd

Function CloseRunningCopies
    InitPluginsDir
    SetOutPath "$PLUGINSDIR"
    ClearErrors
    File /oname=RainBackupSetupCheck.exe "${RAINBACKUP_EXE}"
    IfErrors stop
retry:
    ClearErrors
    ExecWait '"$PLUGINSDIR\RainBackupSetupCheck.exe" --close-for-setup' $0
    IfErrors blocked
    StrCmp $0 0 done
blocked:
    IfSilent stop
    MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "RainBackup is still running. Let any backup or restore finish, then click Retry. If an idle window is still open, close it first." IDRETRY retry
stop:
    SetErrorLevel 1
    Quit
done:
FunctionEnd

Section "RainBackup"
    SectionIn RO
    SetShellVarContext current
    Call CloseRunningCopies
    SetOutPath "$INSTDIR"
    ClearErrors
    File /oname=RainBackup.exe "${RAINBACKUP_EXE}"
    IfErrors 0 installed
    IfSilent installFailed
    MessageBox MB_OK|MB_ICONSTOP "Setup could not replace RainBackup.exe. Close any running copy and run Setup again."
installFailed:
    SetErrorLevel 1
    Quit
installed:
    File "..\README.md"
    File "..\manual\RainBackup-Manual.txt"
    File "..\third_party\zlib\LICENSE"
    File "..\third_party\zlib\minizip\LICENSE.Info-Zip"
    File /oname=Rainmeter-LICENSE.txt "..\third_party\Rainmeter-LICENSE.txt"
    WriteUninstaller "$INSTDIR\Uninstall.exe"
    WriteRegStr HKCU "Software\RainBackup" "InstallDir" "$INSTDIR"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\RainBackup" "DisplayName" "RainBackup"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\RainBackup" "UninstallString" '$\"$INSTDIR\Uninstall.exe$\"'
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\RainBackup" "DisplayIcon" "$INSTDIR\RainBackup.exe"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\RainBackup" "DisplayVersion" "2.4.0"
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\RainBackup" "NoModify" 1
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\RainBackup" "NoRepair" 1
SectionEnd

Section "Create Start menu shortcut" StartMenuShortcut
    CreateShortcut "$SMPROGRAMS\RainBackup.lnk" "$INSTDIR\RainBackup.exe"
SectionEnd

Function .onInit
    ${If} ${RunningX64}
        SetShellVarContext current
        ReadRegStr $0 HKCU "Software\RainBackup" "InstallDir"
        ${If} $0 != ""
            !insertmacro UnselectSection ${StartMenuShortcut}
        ${EndIf}
    ${Else}
        MessageBox MB_ICONSTOP "RainBackup requires 64-bit Windows."
        Abort
    ${EndIf}
FunctionEnd

Function ConfirmInstall
    ${If} ${SectionIsSelected} ${StartMenuShortcut}
        StrCpy $0 "$\r$\n$\r$\nA Start menu shortcut will be created for this Windows account."
    ${Else}
        StrCpy $0 ""
    ${EndIf}
    MessageBox MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2 "Install RainBackup to:$\r$\n$INSTDIR$0" IDYES proceed
    Abort
proceed:
FunctionEnd

Section "Uninstall"
    SetShellVarContext current
    ExecWait '"$INSTDIR\RainBackup.exe" --remove-schedule'
    Delete "$SMPROGRAMS\RainBackup.lnk"
    Delete "$INSTDIR\RainBackup.exe"
    Delete "$INSTDIR\README.md"
    Delete "$INSTDIR\RainBackup-Manual.txt"
    Delete "$INSTDIR\LICENSE"
    Delete "$INSTDIR\LICENSE.Info-Zip"
    Delete "$INSTDIR\Rainmeter-LICENSE.txt"
    Delete "$INSTDIR\Uninstall.exe"
    RMDir "$INSTDIR"
    DeleteRegKey HKCU "Software\RainBackup"
    DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\RainBackup"
SectionEnd
