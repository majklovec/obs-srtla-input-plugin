; NSIS installer for the OBS SRTLA Source plugin (Windows x64).
;
; Built by the CMake `installer` target (cmake/windows/installer.cmake), which
; stages the plugin in an OBS-like layout and passes these defines:
;   PLUGIN_NAME          module name, e.g. obs-srtla-source
;   PLUGIN_DISPLAY_NAME  human-readable name
;   PLUGIN_VERSION       X.Y.Z
;   PLUGIN_AUTHOR        publisher shown in Apps & features
;   PLUGIN_WEBSITE       optional URL
;   STAGING_DIR          directory with obs-plugins\ and data\ to package
;   OUTPUT_FILE          path of the installer to write
;
; The installer copies the module into an existing OBS Studio installation:
;   <OBS>\obs-plugins\64bit\<PLUGIN_NAME>.dll
;   <OBS>\data\obs-plugins\<PLUGIN_NAME>\...

Unicode true
ManifestDPIAware true
SetCompressor /SOLID lzma

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "StrFunc.nsh"
!include "x64.nsh"

!ifndef PLUGIN_NAME
  !error "PLUGIN_NAME is not defined; build through the CMake installer target"
!endif
!ifndef PLUGIN_DISPLAY_NAME
  !define PLUGIN_DISPLAY_NAME "${PLUGIN_NAME}"
!endif
!ifndef PLUGIN_VERSION
  !define PLUGIN_VERSION "0.0.0"
!endif
!ifndef PLUGIN_AUTHOR
  !define PLUGIN_AUTHOR ""
!endif
!ifndef PLUGIN_WEBSITE
  !define PLUGIN_WEBSITE ""
!endif
!ifndef STAGING_DIR
  !error "STAGING_DIR is not defined"
!endif
!ifndef OUTPUT_FILE
  !define OUTPUT_FILE "${PLUGIN_NAME}-${PLUGIN_VERSION}-windows-x64-installer.exe"
!endif

!define PLUGIN_DLL "${PLUGIN_NAME}.dll"
!define PLUGIN_DATA_SUBDIR "data\obs-plugins\${PLUGIN_NAME}"
!define UNINSTALLER "Uninstall-${PLUGIN_NAME}.exe"
!define PLUGIN_REG_KEY "Software\${PLUGIN_NAME}"
!define UNINST_REG_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PLUGIN_NAME}"
!define OBS_REG_KEY "Software\OBS Studio"

${StrStr}
${UnStrStr}

Name "${PLUGIN_DISPLAY_NAME}"
OutFile "${OUTPUT_FILE}"
InstallDir "$PROGRAMFILES64\obs-studio"
RequestExecutionLevel admin
ShowInstDetails show
ShowUninstDetails show

VIProductVersion "${PLUGIN_VERSION}.0"
VIAddVersionKey "ProductName" "${PLUGIN_DISPLAY_NAME}"
VIAddVersionKey "ProductVersion" "${PLUGIN_VERSION}"
VIAddVersionKey "FileVersion" "${PLUGIN_VERSION}"
VIAddVersionKey "FileDescription" "${PLUGIN_DISPLAY_NAME} installer"
VIAddVersionKey "CompanyName" "${PLUGIN_AUTHOR}"
VIAddVersionKey "LegalCopyright" "${PLUGIN_AUTHOR}"

; ---------------------------------------------------------------------------
; Pages
; ---------------------------------------------------------------------------
!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "${PLUGIN_DISPLAY_NAME} ${PLUGIN_VERSION}"
!define MUI_WELCOMEPAGE_TEXT "This wizard installs the ${PLUGIN_DISPLAY_NAME} plugin into an existing OBS Studio (64-bit) installation.$\r$\n$\r$\nClose OBS Studio before continuing."
!define MUI_DIRECTORYPAGE_TEXT_TOP "Select the OBS Studio installation folder (the one containing bin\64bit\obs64.exe). The plugin is copied into its obs-plugins and data folders."
!define MUI_DIRECTORYPAGE_TEXT_DESTINATION "OBS Studio folder"
!define MUI_FINISHPAGE_TITLE "Installation complete"
!define MUI_FINISHPAGE_TEXT "Start OBS Studio and add an $\"SRTLA Source$\" from the Sources dock.$\r$\n$\r$\nThe combined plugin is licensed under the GNU AGPL-3.0; the license texts are in ${PLUGIN_DATA_SUBDIR}\licenses."

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${STAGING_DIR}\${PLUGIN_DATA_SUBDIR}\licenses\LICENSE-srtla.txt"
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE DirectoryPageLeave
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; ---------------------------------------------------------------------------
; Helpers
; ---------------------------------------------------------------------------

; Blocks while obs64.exe is running; the module cannot be replaced or removed
; while OBS has it loaded.
!macro ENSURE_OBS_NOT_RUNNING UN
  Function ${UN}EnsureObsNotRunning
    Push $0
    Push $1
    Push $2
    retry:
      nsExec::ExecToStack 'tasklist /FI "IMAGENAME eq obs64.exe" /NH'
      Pop $0
      Pop $1
      ${If} $0 == "error"
        Goto done
      ${EndIf}
      !if "${UN}" == "un."
        ${UnStrStr} $2 $1 "obs64.exe"
      !else
        ${StrStr} $2 $1 "obs64.exe"
      !endif
      ${If} $2 != ""
        MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION \
          "OBS Studio is running.$\r$\n$\r$\nClose OBS Studio, then click Retry." \
          /SD IDCANCEL IDRETRY retry
        Abort "OBS Studio is running."
      ${EndIf}
    done:
    Pop $2
    Pop $1
    Pop $0
  FunctionEnd
!macroend

!insertmacro ENSURE_OBS_NOT_RUNNING ""
!insertmacro ENSURE_OBS_NOT_RUNNING "un."

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP "${PLUGIN_DISPLAY_NAME} requires 64-bit Windows and OBS Studio 64-bit." /SD IDOK
    Abort
  ${EndIf}
  SetRegView 64

  ; Prefer the folder used by a previous install, then OBS Studio's own
  ; registry entry, then the default under Program Files.
  ReadRegStr $0 HKLM "${PLUGIN_REG_KEY}" "InstallDir"
  ${If} $0 == ""
    ReadRegStr $0 HKLM "${OBS_REG_KEY}" ""
  ${EndIf}
  ${If} $0 == ""
    ReadRegStr $0 HKCU "${OBS_REG_KEY}" ""
  ${EndIf}
  ${If} $0 != ""
    StrCpy $INSTDIR $0
  ${EndIf}

  Call EnsureObsNotRunning
FunctionEnd

Function DirectoryPageLeave
  ${IfNot} ${FileExists} "$INSTDIR\bin\64bit\obs64.exe"
    MessageBox MB_YESNO|MB_ICONQUESTION \
      "bin\64bit\obs64.exe was not found in:$\r$\n$INSTDIR$\r$\n$\r$\nThis does not look like an OBS Studio installation. Install here anyway?" \
      /SD IDNO IDYES +2
    Abort
  ${EndIf}
FunctionEnd

; ---------------------------------------------------------------------------
; Install
; ---------------------------------------------------------------------------
Section "-Plugin"
  SetRegView 64
  Call EnsureObsNotRunning

  SetOutPath "$INSTDIR\obs-plugins\64bit"
  File "${STAGING_DIR}\obs-plugins\64bit\${PLUGIN_DLL}"

  SetOutPath "$INSTDIR\${PLUGIN_DATA_SUBDIR}"
  File /r "${STAGING_DIR}\${PLUGIN_DATA_SUBDIR}\*.*"

  WriteUninstaller "$INSTDIR\${PLUGIN_DATA_SUBDIR}\${UNINSTALLER}"

  WriteRegStr HKLM "${PLUGIN_REG_KEY}" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "${PLUGIN_REG_KEY}" "Version" "${PLUGIN_VERSION}"

  WriteRegStr HKLM "${UNINST_REG_KEY}" "DisplayName" "${PLUGIN_DISPLAY_NAME} (OBS Studio plugin)"
  WriteRegStr HKLM "${UNINST_REG_KEY}" "DisplayVersion" "${PLUGIN_VERSION}"
  WriteRegStr HKLM "${UNINST_REG_KEY}" "Publisher" "${PLUGIN_AUTHOR}"
  WriteRegStr HKLM "${UNINST_REG_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_REG_KEY}" "DisplayIcon" "$INSTDIR\bin\64bit\obs64.exe"
  WriteRegStr HKLM "${UNINST_REG_KEY}" "UninstallString" '"$INSTDIR\${PLUGIN_DATA_SUBDIR}\${UNINSTALLER}"'
  WriteRegStr HKLM "${UNINST_REG_KEY}" "QuietUninstallString" '"$INSTDIR\${PLUGIN_DATA_SUBDIR}\${UNINSTALLER}" /S'
  !if "${PLUGIN_WEBSITE}" != ""
    WriteRegStr HKLM "${UNINST_REG_KEY}" "URLInfoAbout" "${PLUGIN_WEBSITE}"
  !endif
  WriteRegDWORD HKLM "${UNINST_REG_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_REG_KEY}" "NoRepair" 1

  ${GetSize} "$INSTDIR\${PLUGIN_DATA_SUBDIR}" "/S=0K" $0 $1 $2
  ${GetSize} "$INSTDIR\obs-plugins\64bit" "/M=${PLUGIN_DLL} /S=0K" $3 $1 $2
  IntOp $0 $0 + $3
  WriteRegDWORD HKLM "${UNINST_REG_KEY}" "EstimatedSize" $0
SectionEnd

; ---------------------------------------------------------------------------
; Uninstall
; ---------------------------------------------------------------------------
Function un.onInit
  SetRegView 64
  ; The uninstaller lives inside the plugin data folder, so $INSTDIR must be
  ; recovered from the registry rather than from the executable's location.
  ReadRegStr $0 HKLM "${PLUGIN_REG_KEY}" "InstallDir"
  ${If} $0 == ""
    MessageBox MB_OK|MB_ICONSTOP "The ${PLUGIN_DISPLAY_NAME} installation folder was not found in the registry." /SD IDOK
    Abort
  ${EndIf}
  StrCpy $INSTDIR $0
  Call un.EnsureObsNotRunning
FunctionEnd

Section "Uninstall"
  SetRegView 64

  Delete "$INSTDIR\obs-plugins\64bit\${PLUGIN_DLL}"
  RMDir /r "$INSTDIR\${PLUGIN_DATA_SUBDIR}"

  DeleteRegKey HKLM "${UNINST_REG_KEY}"
  DeleteRegKey HKLM "${PLUGIN_REG_KEY}"
SectionEnd
