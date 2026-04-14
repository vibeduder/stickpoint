; StickPoint — Inno Setup installer script
; Requires Inno Setup 6.x  https://jrsoftware.org/isinfo.php
;
; Build the Release executable first:
;   cmake -B build -G "Visual Studio 17 2022" -A x64
;   cmake --build build --config Release
;
; Then compile this script with Inno Setup Compiler (ISCC.exe):
;   ISCC.exe installer.iss
;
; Output: Output\StickPoint-Setup.exe

; ---- application metadata -------------------------------------------------

#define AppName    "StickPoint"
#define AppVersion "1.1"
#define AppExe     "StickPoint.exe"
#define AppBin     "build\Release\" + AppExe

[Setup]
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=StickPoint

; Install to "C:\Program Files\StickPoint" by default.
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}

; Windows 10+ (build 10240) minimum.
MinVersion=10.0.10240

; Install in 64-bit mode when running on a 64-bit OS.
ArchitecturesInstallIn64BitMode=x64compatible

; Require admin so we can write to Program Files.
PrivilegesRequired=admin

; Installer output
OutputDir=Output
OutputBaseFilename=StickPoint-Setup
SetupIconFile=assets\icon.ico

Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern

; Prevent running multiple instances of the installer.
AppMutex=StickPointInstallerMutex

; ---- languages ------------------------------------------------------------

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

; ---- optional tasks shown to the user ------------------------------------

[Tasks]
; "Run at startup" is checked by default — sensible for a tray utility.
Name: "startup"; \
    Description: "Start {#AppName} automatically when Windows starts"; \
    GroupDescription: "Additional tasks:"

; ---- files ----------------------------------------------------------------

[Files]
; The compiled executable.  Adjust the path if your CMake build directory
; or configuration name differs.
Source: "{#AppBin}"; DestDir: "{app}"; Flags: ignoreversion
; Runtime assets must be installed next to the executable so build_image_path()
; can resolve assets\controllers\... from the application directory.
Source: "assets\*"; DestDir: "{app}\assets"; Flags: ignoreversion recursesubdirs createallsubdirs

; ---- Start Menu shortcuts -------------------------------------------------

[Icons]
Name: "{group}\{#AppName}";           Filename: "{app}\{#AppExe}"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"

; ---- registry (startup task) ---------------------------------------------

[Registry]
; HKCU Run key — added only when the user selects the "startup" task above,
; removed automatically on uninstall.
Root: HKCU; \
    Subkey:    "Software\Microsoft\Windows\CurrentVersion\Run"; \
    ValueType: string; \
    ValueName: "{#AppName}"; \
    ValueData: """{app}\{#AppExe}"""; \
    Flags:     uninsdeletevalue; \
    Tasks:     startup

; ---- post-install launch --------------------------------------------------

[Run]
; Offer to launch StickPoint immediately after installation completes.
Filename: "{app}\{#AppExe}"; \
    Description: "Launch {#AppName} now"; \
    Flags: nowait postinstall skipifsilent
