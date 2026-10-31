; LYNX SHARE per-user Windows installer (NSIS 3). Built by scripts/make-installer.sh; no admin rights needed.
; Installs to %LOCALAPPDATA%\Programs\LYNX, adds Start-menu (and optional desktop) shortcuts, registers .lynx
; projects and offers LYNX in Explorer's "Open with" for .flp without taking over FL Studio's default.
; The uninstaller removes only what this installer added (settings in %APPDATA%\LYNX stay).
;
; Defines passed by the script / release workflow: VERSION (e.g. 0.1.3), STAGE (folder with LYNX.exe, LICENSE,
; THIRD_PARTY.md, optional PRIVACY.md and Demo.lynx), OUTFILE, ICON.

Unicode true
SetCompressor /SOLID lzma
RequestExecutionLevel user
ManifestDPIAware true

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "Sections.nsh"

!define APP "LYNX SHARE" ; the product name shown (v0.1.20 rename); folders, keys and LYNX.exe keep "LYNX"
!define PROGID "LYNX.Project"
!define FLP_PROGID "LYNX.FLProject"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\LYNX"

Name "${APP} ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\Programs\LYNX"
InstallDirRegKey HKCU "Software\LYNX" "InstallDir"
BrandingText "LYNX SHARE ${VERSION}"
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "LYNX SHARE"
VIAddVersionKey "FileDescription" "LYNX SHARE installer"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "AGPL-3.0"

!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\LYNX.exe"
!define MUI_FINISHPAGE_RUN_TEXT "$(RunLynx)"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${STAGE}\LICENSE"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "PortugueseBR"

LangString RunLynx ${LANG_ENGLISH} "Start LYNX SHARE"
LangString RunLynx ${LANG_PORTUGUESEBR} "Abrir o LYNX SHARE"
LangString SecApp ${LANG_ENGLISH} "LYNX SHARE (required)"
LangString SecApp ${LANG_PORTUGUESEBR} "LYNX SHARE (obrigatório)"
LangString SecDesktop ${LANG_ENGLISH} "Desktop shortcut"
LangString SecDesktop ${LANG_PORTUGUESEBR} "Atalho na área de trabalho"
LangString SecFlp ${LANG_ENGLISH} "Offer LYNX SHARE in $\"Open with$\" for FL Studio projects (.flp)"
LangString SecFlp ${LANG_PORTUGUESEBR} "Oferecer o LYNX SHARE em $\"Abrir com$\" para projetos do FL Studio (.flp)"
LangString FlpType ${LANG_ENGLISH} "FL Studio project (LYNX SHARE)"
LangString FlpType ${LANG_PORTUGUESEBR} "Projeto do FL Studio (LYNX SHARE)"
LangString StillRunning ${LANG_ENGLISH} "LYNX SHARE is running. Close it and click Retry."
LangString StillRunning ${LANG_PORTUGUESEBR} "O LYNX SHARE está aberto. Feche-o e clique em Repetir."

Function .onInit
    ; the installer's language follows Windows
    System::Call 'kernel32::GetUserDefaultUILanguage() i .r0'
    ${If} $0 == 1046
        StrCpy $LANGUAGE ${LANG_PORTUGUESEBR}
    ${EndIf}
    Call SilentKeepsChoices
FunctionEnd

!macro CheckNotRunning
    ; overwriting a running exe fails: ask to close it first
    retry:
    ClearErrors
    ${If} ${FileExists} "$INSTDIR\LYNX.exe"
        FileOpen $0 "$INSTDIR\LYNX.exe" a
        ${If} ${Errors}
            MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "$(StillRunning)" IDRETRY retry
            Abort
        ${EndIf}
        FileClose $0
    ${EndIf}
!macroend

Section "!$(SecApp)" SecMain
    SectionIn RO
    SetShellVarContext current
    !insertmacro CheckNotRunning
    SetOutPath "$INSTDIR"
    File "${STAGE}\LYNX.exe"
    File "${STAGE}\LICENSE"
    File "${STAGE}\THIRD_PARTY.md"
    File /nonfatal "${STAGE}\PRIVACY.md"
    File /nonfatal "${STAGE}\Demo.lynx"
    ; LYNX ASIO (REA round 97): LYNX loads it directly; the .bat files register it for other programs (administrator)
    File /nonfatal "${STAGE}\lynx-asio.dll"
    File /nonfatal "${STAGE}\lynx-asio32.dll" ; for 32-bit programs (REA round 106)
    File /nonfatal "${STAGE}\Register LYNX ASIO.bat"
    File /nonfatal "${STAGE}\Unregister LYNX ASIO.bat"
    WriteUninstaller "$INSTDIR\Uninstall.exe"

    Delete "$SMPROGRAMS\LYNX.lnk" ; the shortcut before the v0.1.20 rename
    CreateShortCut "$SMPROGRAMS\LYNX SHARE.lnk" "$INSTDIR\LYNX.exe" "" "$INSTDIR\LYNX.exe" 0

    ; .lynx projects (LYNX itself keeps this up to date at every start, too)
    WriteRegStr HKCU "Software\Classes\.lynx" "" "${PROGID}"
    WriteRegStr HKCU "Software\Classes\.lynx" "Content Type" "application/x-lynx-project"
    WriteRegStr HKCU "Software\Classes\${PROGID}" "" "LYNX SHARE Project"
    WriteRegStr HKCU "Software\Classes\${PROGID}\DefaultIcon" "" '"$INSTDIR\LYNX.exe",-2'
    WriteRegStr HKCU "Software\Classes\${PROGID}\shell\open\command" "" '"$INSTDIR\LYNX.exe" "%1"'
    ; "Open with" list entry for the app
    WriteRegStr HKCU "Software\Classes\Applications\LYNX.exe" "FriendlyAppName" "LYNX SHARE"
    WriteRegStr HKCU "Software\Classes\Applications\LYNX.exe\shell\open\command" "" '"$INSTDIR\LYNX.exe" "%1"'
    WriteRegStr HKCU "Software\Classes\Applications\LYNX.exe\SupportedTypes" ".lynx" ""

    WriteRegStr HKCU "Software\LYNX" "InstallDir" "$INSTDIR"
    WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "LYNX SHARE"
    WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
    WriteRegStr HKCU "${UNINST_KEY}" "Publisher" "LYNX SHARE"
    WriteRegStr HKCU "${UNINST_KEY}" "URLInfoAbout" "https://github.com/Ka1-S/LYNX-SHARE"
    WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon" '"$INSTDIR\LYNX.exe",0'
    WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegStr HKCU "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
    WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
    WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
    SectionGetSize ${SecMain} $0
    WriteRegDWORD HKCU "${UNINST_KEY}" "EstimatedSize" $0
SectionEnd

Section "$(SecDesktop)" SecDesktop
    SetShellVarContext current
    Delete "$DESKTOP\LYNX.lnk"
    CreateShortCut "$DESKTOP\LYNX SHARE.lnk" "$INSTDIR\LYNX.exe" "" "$INSTDIR\LYNX.exe" 0
SectionEnd

Section "$(SecFlp)" SecFlp
    ; adds LYNX to .flp's "Open with" list only: FL Studio stays the default program for .flp
    WriteRegStr HKCU "Software\Classes\${FLP_PROGID}" "" "$(FlpType)"
    WriteRegStr HKCU "Software\Classes\${FLP_PROGID}\DefaultIcon" "" '"$INSTDIR\LYNX.exe",0'
    WriteRegStr HKCU "Software\Classes\${FLP_PROGID}\shell\open\command" "" '"$INSTDIR\LYNX.exe" "%1"'
    WriteRegStr HKCU "Software\Classes\.flp\OpenWithProgids" "${FLP_PROGID}" ""
    WriteRegStr HKCU "Software\Classes\Applications\LYNX.exe\SupportedTypes" ".flp" ""
SectionEnd

Section "-notify"
    System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
SectionEnd

Section "Uninstall"
    SetShellVarContext current
    Delete "$INSTDIR\LYNX.exe"
    Delete "$INSTDIR\LYNX.pdb"
    Delete "$INSTDIR\LICENSE"
    Delete "$INSTDIR\THIRD_PARTY.md"
    Delete "$INSTDIR\PRIVACY.md"
    Delete "$INSTDIR\Demo.lynx"
    ; LYNX ASIO registered for other programs: unregister it first (Windows asks for administrator rights).
    ; The 64-bit registration lives in the 64-bit registry view (this uninstaller is a 32-bit program).
    SetRegView 64
    ReadRegStr $0 HKLM "SOFTWARE\ASIO\LYNX ASIO" "CLSID"
    SetRegView 32
    ${If} $0 != ""
    ${AndIf} ${FileExists} "$INSTDIR\lynx-asio.dll"
        ExecShellWait "runas" "$WINDIR\Sysnative\regsvr32.exe" '/u /s "$INSTDIR\lynx-asio.dll"'
    ${EndIf}
    ; the 32-bit registration lives in the 32-bit registry view
    ReadRegStr $0 HKLM "SOFTWARE\ASIO\LYNX ASIO" "CLSID"
    ${If} $0 != ""
    ${AndIf} ${FileExists} "$INSTDIR\lynx-asio32.dll"
        ExecShellWait "runas" "$WINDIR\SysWOW64\regsvr32.exe" '/u /s "$INSTDIR\lynx-asio32.dll"'
    ${EndIf}
    Delete "$INSTDIR\lynx-asio.dll"
    Delete "$INSTDIR\lynx-asio32.dll"
    Delete "$INSTDIR\Register LYNX ASIO.bat"
    Delete "$INSTDIR\Unregister LYNX ASIO.bat"
    Delete "$INSTDIR\Uninstall.exe"
    RMDir "$INSTDIR"
    Delete "$SMPROGRAMS\LYNX.lnk"
    Delete "$DESKTOP\LYNX.lnk"
    Delete "$SMPROGRAMS\LYNX SHARE.lnk"
    Delete "$DESKTOP\LYNX SHARE.lnk"

    ; only our own keys and values; .lynx is removed only while it still points at LYNX
    ReadRegStr $0 HKCU "Software\Classes\.lynx" ""
    ${If} $0 == "${PROGID}"
        DeleteRegKey HKCU "Software\Classes\.lynx"
    ${EndIf}
    DeleteRegKey HKCU "Software\Classes\${PROGID}"
    DeleteRegKey HKCU "Software\Classes\${FLP_PROGID}"
    DeleteRegValue HKCU "Software\Classes\.flp\OpenWithProgids" "${FLP_PROGID}"
    ; (the .flp key itself is FL Studio's: left alone)
    DeleteRegKey HKCU "Software\Classes\Applications\LYNX.exe"
    DeleteRegKey HKCU "Software\LYNX"
    DeleteRegKey HKCU "${UNINST_KEY}"
    System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
SectionEnd

; A silent install (/S) is LYNX SHARE's in-app update (ui/Updater.h, round 166b): it keeps the choices of the install it
; updates instead of the defaults, so an update never adds a desktop shortcut or the .flp "Open with" entry the user
; left out. (Defined after the sections: it names their indices.)
Function SilentKeepsChoices
    ${If} ${Silent}
        SetShellVarContext current
        ${IfNot} ${FileExists} "$DESKTOP\LYNX SHARE.lnk"
            !insertmacro UnselectSection ${SecDesktop}
        ${EndIf}
        ReadRegStr $0 HKCU "Software\Classes\${FLP_PROGID}" ""
        ${If} $0 == ""
            !insertmacro UnselectSection ${SecFlp}
        ${EndIf}
    ${EndIf}
FunctionEnd
