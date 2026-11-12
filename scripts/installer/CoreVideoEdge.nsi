; CoreVideo Edge Windows installer (unsigned beta).
; Built by scripts\package-release.ps1, which passes:
;   /DRELEASE_ID=<beta-YYYY-MM-DD-sha7>  /DAPP_VERSION=<x.y.z>
;   /DSTAGE_DIR=<staged CoreVideoEdge folder>  /DVCREDIST=<vc_redist.x64.exe>
;   /DOUTFILE=<Setup .exe path>  /DICON=<CoreVideoEdge.ico>
;
; Per-user install (no admin) into %LOCALAPPDATA%\Programs\CoreVideoEdge.
; The only step that can ask for elevation is the Microsoft VC++ runtime, and
; only when this machine does not already have it.

Unicode true
SetCompressor /SOLID lzma
RequestExecutionLevel user

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"

!define PRODUCT     "CoreVideo Edge"
!define UNINST_KEY  "Software\Microsoft\Windows\CurrentVersion\Uninstall\CoreVideoEdge"

Name "${PRODUCT} ${RELEASE_ID}"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\Programs\CoreVideoEdge"
InstallDirRegKey HKCU "${UNINST_KEY}" "InstallLocation"
BrandingText "${PRODUCT} ${RELEASE_ID} (unsigned beta)"

VIProductVersion "${APP_VERSION}.0"
VIAddVersionKey "ProductName" "${PRODUCT}"
VIAddVersionKey "ProductVersion" "${RELEASE_ID}"
VIAddVersionKey "FileVersion" "${APP_VERSION}"
VIAddVersionKey "FileDescription" "${PRODUCT} installer"
VIAddVersionKey "LegalCopyright" "iamfatness"

!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\CoreVideoEdge.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Start ${PRODUCT}"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "${PRODUCT} needs 64-bit Windows." /SD IDOK
    Abort
  ${EndIf}
FunctionEnd

Section "CoreVideo Edge" SecMain
  SectionIn RO

  ; Refuse to overwrite a running copy: its files are locked mid-show.
  FindWindow $0 "" "CoreVideo Edge"
  ${If} $0 <> 0
    MessageBox MB_ICONEXCLAMATION "Close ${PRODUCT} before installing." /SD IDOK
    SetErrorLevel 2
    Abort
  ${EndIf}

  ; A previous release's files are replaced wholesale (Zoom runtime included),
  ; so a DLL from an older SDK can never sit beside the new engine.
  ${If} ${FileExists} "$INSTDIR\CoreVideoEdge.exe"
    RMDir /r "$INSTDIR"
  ${EndIf}

  SetOutPath "$INSTDIR"
  File /r "${STAGE_DIR}\*.*"

  ; Microsoft VC++ 2015-2022 runtime (the node, engine and Zoom SDK need it).
  SetRegView 64
  ReadRegDWORD $1 HKLM "SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64" "Installed"
  SetRegView 32
  ${If} $1 <> 1
    DetailPrint "Installing the Microsoft Visual C++ runtime..."
    SetOutPath "$PLUGINSDIR"
    File "/oname=vc_redist.x64.exe" "${VCREDIST}"
    ExecWait '"$PLUGINSDIR\vc_redist.x64.exe" /install /quiet /norestart' $2
    ; 0 ok, 1638 newer already present, 3010 ok + reboot later.
    ${If} $2 <> 0
    ${AndIf} $2 <> 1638
    ${AndIf} $2 <> 3010
      MessageBox MB_ICONEXCLAMATION "The Microsoft Visual C++ runtime did not install (code $2). ${PRODUCT} may not start until it is installed." /SD IDOK
    ${EndIf}
    SetOutPath "$INSTDIR"
  ${EndIf}

  CreateDirectory "$SMPROGRAMS"
  CreateShortcut "$SMPROGRAMS\${PRODUCT}.lnk" "$INSTDIR\CoreVideoEdge.exe" "" "$INSTDIR\CoreVideoEdge.exe" 0

  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr   HKCU "${UNINST_KEY}" "DisplayName" "${PRODUCT}"
  WriteRegStr   HKCU "${UNINST_KEY}" "DisplayVersion" "${RELEASE_ID}"
  WriteRegStr   HKCU "${UNINST_KEY}" "Publisher" "iamfatness"
  WriteRegStr   HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr   HKCU "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\CoreVideoEdge.exe"
  WriteRegStr   HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr   HKCU "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  FindWindow $0 "" "CoreVideo Edge"
  ${If} $0 <> 0
    MessageBox MB_ICONEXCLAMATION "Close ${PRODUCT} before uninstalling." /SD IDOK
    SetErrorLevel 2
    Abort
  ${EndIf}

  Delete "$SMPROGRAMS\${PRODUCT}.lnk"

  ; corevideo:// is shared with CoreVideo Pro: remove it only while it still
  ; points at this install.
  ReadRegStr $1 HKCU "Software\Classes\corevideo\shell\open\command" ""
  StrLen $2 "$INSTDIR"
  StrCpy $3 $1 $2 1
  ${If} $3 == "$INSTDIR"
    DeleteRegKey HKCU "Software\Classes\corevideo"
  ${EndIf}

  DeleteRegKey HKCU "${UNINST_KEY}"
  ; Settings and sign-in (%LOCALAPPDATA%\CoreVideoEdge) and recordings stay.
  RMDir /r "$INSTDIR"
SectionEnd
