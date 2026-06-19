; OrbitCab — IR Cabinet Loader (Felitronics by Darwin's Cat). Windows installer (Inno Setup 6).
;
; UNSIGNED for now: SmartScreen will warn ("Windows protected your PC" → More info →
; Run anyway) until code signing is added.
;
; Build (on Windows, after the CMake Release build):
;   ISCC.exe /DAppVersion=1.2.3 installer\orbitcab.iss
; Overridable defines: AppVersion, BuildDir (the CMake Release artefacts dir).

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef BuildDir
  #define BuildDir "..\build\OrbitCab_artefacts\Release"
#endif

#define AppName      "OrbitCab"
#define AppPublisher "Darwin's Cat"
#define AppURL       "https://darwinscat.com/orbitcab"

[Setup]
; Stable AppId (do NOT change post-release — it identifies the product for upgrades).
; This GUID is OrbitCab's own — keep it stable.
AppId={{C3A7F1E2-9B4D-4A6C-8E2F-0D5B7A1C3E94}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
DefaultDirName={autopf}\{#AppName}
DisableProgramGroupPage=yes
DisableDirPage=yes
OutputDir=output
OutputBaseFilename=OrbitCab-{#AppVersion}-Windows-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Plugins live under Program Files\Common Files → needs admin.
PrivilegesRequired=admin

[Components]
Name: "vst3";       Description: "VST3 plugin";   Types: full custom; Flags: fixed
Name: "standalone"; Description: "Standalone app"; Types: full custom

[Files]
; VST3 is a bundle (folder) — copy its contents into the shared VST3 location.
Source: "{#BuildDir}\VST3\OrbitCab.vst3\*"; DestDir: "{commoncf64}\VST3\OrbitCab.vst3"; \
    Flags: recursesubdirs createallsubdirs ignoreversion; Components: vst3
Source: "{#BuildDir}\Standalone\OrbitCab.exe"; DestDir: "{app}"; Flags: ignoreversion; Components: standalone

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\OrbitCab.exe"; Components: standalone

[UninstallDelete]
Type: filesandordirs; Name: "{commoncf64}\VST3\OrbitCab.vst3"
