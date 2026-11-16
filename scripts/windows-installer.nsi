; rega's Windows installer: a per-user install (no admin, no UAC prompt) of the packaged
; rega/ folder, the same one that goes into the zip. Built by `just package-gui` on Windows,
; or from any packaged folder with `just windows-installer <dir>` (makensis runs on Linux
; too). Adapted from quickrecord's.
;
; Besides the Start menu entry: "Open with rega" for its formats (never the default app),
; and "Open in rega" on folders.
;
; makensis runs from this directory, so relative paths start here.
; Defines (all required): VERSION (e.g. 0.2.0 or dev), VI_VERSION (four numbers, e.g.
; 0.2.0.0), SRC_DIR (absolute path of the packaged folder), OUT_FILE (the setup .exe).

Unicode true
SetCompressor /SOLID lzma
RequestExecutionLevel user

!include MUI2.nsh
!include FileFunc.nsh

!define APP "rega"
!define EXE "rega.exe"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP}"
!define PROGID "rega.file"
!define CLASSES "Software\Classes"

Name "${APP}"
OutFile "${OUT_FILE}"
InstallDir "$LOCALAPPDATA\Programs\${APP}"
InstallDirRegKey HKCU "${UNINSTALL_KEY}" "InstallLocation"
BrandingText "${APP} ${VERSION}"

VIProductVersion "${VI_VERSION}"
VIAddVersionKey "ProductName" "${APP}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "${APP} installer"
VIAddVersionKey "LegalCopyright" "MIT licence"

!define MUI_ICON "../assets/icons/rega.ico"
!define MUI_UNICON "../assets/icons/rega.ico"
!define MUI_ABORTWARNING
!define MUI_COMPONENTSPAGE_NODESC
!define MUI_FINISHPAGE_RUN "$INSTDIR\${EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Launch ${APP}"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE OwnFolder
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; Installing (and uninstalling) removes the install folder, so it must be one of our own: a
; folder picked as, say, Documents becomes Documents\rega.
Function OwnFolder
    StrCpy $0 $INSTDIR "" -4
    StrCmp $0 "${APP}" +2
    StrCpy $INSTDIR "$INSTDIR\${APP}"
FunctionEnd

; Waits until a running rega's exe can be replaced (up to 10 s): close it first.
!macro WAIT_FOR_EXE un
Function ${un}WaitForExe
    IfFileExists "$INSTDIR\${EXE}" 0 done
    StrCpy $1 0
    retry:
        ClearErrors
        Delete "$INSTDIR\${EXE}"
        IfErrors 0 done
        IntOp $1 $1 + 1
        IntCmp $1 40 0 0 failed
        Sleep 250
        Goto retry
    failed:
        MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "Close rega, then retry." IDRETRY retry
        Abort
    done:
FunctionEnd
!macroend
!insertmacro WAIT_FOR_EXE ""
!insertmacro WAIT_FOR_EXE "un."

; "Open with rega" for one extension: listed, not made the default.
!macro OPEN_WITH ext
    WriteRegStr HKCU "${CLASSES}\.${ext}\OpenWithProgids" "${PROGID}" ""
    WriteRegStr HKCU "${CLASSES}\Applications\${EXE}\SupportedTypes" ".${ext}" ""
!macroend
!macro NO_OPEN_WITH ext
    DeleteRegValue HKCU "${CLASSES}\.${ext}\OpenWithProgids" "${PROGID}"
!macroend

Section "-${APP}"
    Call OwnFolder
    Call WaitForExe
    ; An upgrade starts from an empty folder, so nothing of the previous version lingers.
    IfFileExists "$INSTDIR\Uninstall.exe" 0 +2
    RMDir /r "$INSTDIR"

    SetOutPath "$INSTDIR"
    ; A backslash: Windows makensis only splits the folder from the wildcard there (the Linux
    ; and macOS builds of makensis turn it into a slash).
    File /r "${SRC_DIR}\*"
    WriteUninstaller "$INSTDIR\Uninstall.exe"
    CreateShortcut "$SMPROGRAMS\${APP}.lnk" "$INSTDIR\${EXE}"

    ; What "Open with rega" runs.
    WriteRegStr HKCU "${CLASSES}\${PROGID}" "" "Image (rega)"
    WriteRegStr HKCU "${CLASSES}\${PROGID}\DefaultIcon" "" '"$INSTDIR\${EXE}",0'
    WriteRegStr HKCU "${CLASSES}\${PROGID}\shell\open\command" "" '"$INSTDIR\${EXE}" "%1"'
    WriteRegStr HKCU "${CLASSES}\Applications\${EXE}" "FriendlyAppName" "rega"
    WriteRegStr HKCU "${CLASSES}\Applications\${EXE}\shell\open\command" "" '"$INSTDIR\${EXE}" "%1"'
    !insertmacro OPEN_WITH "exr"
    !insertmacro OPEN_WITH "tif"
    !insertmacro OPEN_WITH "tiff"
    !insertmacro OPEN_WITH "png"
    !insertmacro OPEN_WITH "jpg"
    !insertmacro OPEN_WITH "jpeg"
    !insertmacro OPEN_WITH "mov"
    !insertmacro OPEN_WITH "mp4"

    WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "${APP}"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "Publisher" "${APP}"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" '"$INSTDIR\${EXE}"'
    WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegStr HKCU "${UNINSTALL_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
    WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
    WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
    ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
    IntFmt $0 "0x%08X" $0
    WriteRegDWORD HKCU "${UNINSTALL_KEY}" "EstimatedSize" $0
    ; Explorer picks up the new "Open with" entries.
    System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
SectionEnd

Section "Open in rega on folders"
    WriteRegStr HKCU "${CLASSES}\Directory\shell\rega" "" "Open in rega"
    WriteRegStr HKCU "${CLASSES}\Directory\shell\rega" "Icon" '"$INSTDIR\${EXE}",0'
    WriteRegStr HKCU "${CLASSES}\Directory\shell\rega\command" "" '"$INSTDIR\${EXE}" "%1"'
SectionEnd

Section /o "Desktop shortcut"
    CreateShortcut "$DESKTOP\${APP}.lnk" "$INSTDIR\${EXE}"
SectionEnd

Section "Uninstall"
    Call un.WaitForExe
    Delete "$SMPROGRAMS\${APP}.lnk"
    Delete "$DESKTOP\${APP}.lnk"
    !insertmacro NO_OPEN_WITH "exr"
    !insertmacro NO_OPEN_WITH "tif"
    !insertmacro NO_OPEN_WITH "tiff"
    !insertmacro NO_OPEN_WITH "png"
    !insertmacro NO_OPEN_WITH "jpg"
    !insertmacro NO_OPEN_WITH "jpeg"
    !insertmacro NO_OPEN_WITH "mov"
    !insertmacro NO_OPEN_WITH "mp4"
    DeleteRegKey HKCU "${CLASSES}\${PROGID}"
    DeleteRegKey HKCU "${CLASSES}\Applications\${EXE}"
    DeleteRegKey HKCU "${CLASSES}\Directory\shell\rega"
    DeleteRegKey HKCU "${UNINSTALL_KEY}"
    System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
    ; Only our own folder (see OwnFolder). Annotations and settings live in
    ; %LOCALAPPDATA%\rega and stay.
    StrCpy $0 $INSTDIR "" -4
    StrCmp $0 "${APP}" 0 +2
    RMDir /r "$INSTDIR"
SectionEnd
