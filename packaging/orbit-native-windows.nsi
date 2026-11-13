Unicode true
RequestExecutionLevel user
SetCompressor /SOLID lzma

!include "MUI2.nsh"
!include "WinVer.nsh"
!include "x64.nsh"

Name "Orbit Native"
OutFile "${ORBIT_OUTPUT}"
InstallDir "$LOCALAPPDATA\Programs\Orbit Native"
VIProductVersion "${ORBIT_VERSION}.0"
VIAddVersionKey "ProductName" "Orbit Native"
VIAddVersionKey "FileDescription" "Orbit Native Setup"
VIAddVersionKey "FileVersion" "${ORBIT_VERSION}"
VIAddVersionKey "LegalCopyright" "Orbit Native contributors"

!define MUI_FINISHPAGE_RUN "$INSTDIR\orbit-app\bin\OrbitShell.exe"
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "Russian"

LangString Unsupported ${LANG_ENGLISH} "Orbit Native requires 64-bit Windows 10 (1809 or later) or Windows 11."
LangString Unsupported ${LANG_RUSSIAN} "Для Orbit Native нужна 64-битная Windows 10 версии 1809 или новее либо Windows 11."
LangString WebViewFailed ${LANG_ENGLISH} "Connect to the internet and run the installer again to install Microsoft WebView2."
LangString WebViewFailed ${LANG_RUSSIAN} "Подключитесь к интернету и снова запустите установщик для установки Microsoft WebView2."
LangString WebViewInstalling ${LANG_ENGLISH} "Downloading and installing Microsoft WebView2. This may take a few minutes."
LangString WebViewInstalling ${LANG_RUSSIAN} "Загружаем и устанавливаем Microsoft WebView2. Это может занять несколько минут."
LangString ExistingFolder ${LANG_ENGLISH} "The installation folder already contains files from a different application."
LangString ExistingFolder ${LANG_RUSSIAN} "В папке установки уже есть файлы другого приложения."

Function .onInit
    System::Call 'kernel32::GetUserDefaultUILanguage() i .r0'
    IntOp $0 $0 & 0x3ff
    ${If} $0 = 25
        StrCpy $LANGUAGE ${LANG_RUSSIAN}
    ${Else}
        StrCpy $LANGUAGE ${LANG_ENGLISH}
    ${EndIf}
    SetRegView 64
    ReadRegStr $1 HKLM "SOFTWARE\Microsoft\Windows NT\CurrentVersion" "CurrentBuildNumber"
    ${IfNot} ${AtLeastWin10}
    ${OrIf} ${IsNativeIA32}
    ${OrIf} $1 < 17763
        MessageBox MB_ICONSTOP|MB_OK "$(Unsupported)" /SD IDOK
        SetErrorLevel 2
        Abort
    ${EndIf}
    SetShellVarContext current
    SetRegView 64
FunctionEnd

Function ReadWebViewVersion
    ReadRegStr $0 HKLM "SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}" "pv"
    ${If} $0 == ""
    ${OrIf} $0 == "0.0.0.0"
        ReadRegStr $0 HKCU "Software\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}" "pv"
    ${EndIf}
FunctionEnd

Function EnsureWebView
    SetRegView 32
    Call ReadWebViewVersion
    ${If} $0 == ""
    ${OrIf} $0 == "0.0.0.0"
        DetailPrint "$(WebViewInstalling)"
        InitPluginsDir
        SetOutPath "$PLUGINSDIR"
        File /oname=OrbitWebView2Setup.exe "${ORBIT_BOOTSTRAPPER}"
        ClearErrors
        ExecWait '"$PLUGINSDIR\OrbitWebView2Setup.exe" /silent /install' $1
        ${If} ${Errors}
            StrCpy $1 1
        ${EndIf}
        Delete "$PLUGINSDIR\OrbitWebView2Setup.exe"
        ${If} $1 == 0
            StrCpy $2 20
            ${Do}
                Call ReadWebViewVersion
                ${If} $0 != ""
                ${AndIf} $0 != "0.0.0.0"
                    ${ExitDo}
                ${EndIf}
                ${If} $2 == 0
                    ${ExitDo}
                ${EndIf}
                Sleep 250
                IntOp $2 $2 - 1
            ${Loop}
        ${EndIf}
        ${If} $1 != 0
        ${OrIf} $0 == ""
        ${OrIf} $0 == "0.0.0.0"
            MessageBox MB_ICONSTOP|MB_OK "$(WebViewFailed)" /SD IDOK
            SetErrorLevel 3
            Abort
        ${EndIf}
    ${EndIf}
    SetRegView 64
FunctionEnd

Section
    ${If} ${FileExists} "$INSTDIR\orbit-app\*.*"
    ${AndIfNot} ${FileExists} "$INSTDIR\orbit-app\orbit-install.marker"
        MessageBox MB_ICONSTOP|MB_OK "$(ExistingFolder)" /SD IDOK
        SetErrorLevel 4
        Abort
    ${EndIf}
    Call EnsureWebView
    SetOutPath "$INSTDIR\orbit-app"
    ${If} ${IsNativeARM64}
        File /r "${ORBIT_ARM64}\*"
    ${Else}
        File /r "${ORBIT_X64}\*"
    ${EndIf}
    FileOpen $0 "$INSTDIR\orbit-app\orbit-install.marker" w
    FileClose $0
    WriteUninstaller "$INSTDIR\Uninstall.exe"
    CreateShortcut "$SMPROGRAMS\Orbit Native.lnk" "$INSTDIR\orbit-app\bin\OrbitShell.exe"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\OrbitNative" "DisplayName" "Orbit Native"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\OrbitNative" "DisplayVersion" "${ORBIT_VERSION}"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\OrbitNative" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\OrbitNative" "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\OrbitNative" "NoModify" 1
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\OrbitNative" "NoRepair" 1
SectionEnd

Section "Uninstall"
    SetShellVarContext current
    SetRegView 64
    ${If} ${FileExists} "$INSTDIR\orbit-app\orbit-install.marker"
        RMDir /r "$INSTDIR\orbit-app"
    ${EndIf}
    Delete "$SMPROGRAMS\Orbit Native.lnk"
    DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "OrbitNative"
    DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\OrbitNative"
    Delete "$INSTDIR\Uninstall.exe"
    RMDir "$INSTDIR"
SectionEnd
