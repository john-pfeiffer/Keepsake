; Keepsake Windows installer (spec §7: Inno Setup; signing optional for v1 -
; SmartScreen click-through is acceptable early).
;
; Build:  iscc /DVersion=1.0.0 /DArtefactsDir=..\..\build\Keepsake_artefacts\Release keepsake.iss
;
; VST3 goes to the standard system folder ({commoncf64}\VST3) as the folder-style
; bundle JUCE builds; the standalone goes to Program Files with a Start-menu
; entry. The uninstaller removes both.

#ifndef Version
  #define Version "0.0.0"
#endif
#ifndef ArtefactsDir
  #define ArtefactsDir "..\..\build\Keepsake_artefacts\Release"
#endif

[Setup]
AppId={{7B1E6C1A-4D5B-45E3-9C93-2E6D3E6B2A51}
AppName=Keepsake
AppVersion={#Version}
AppPublisher=Elan Vital Studios
DefaultDirName={autopf}\Keepsake
OutputBaseFilename=Keepsake-{#Version}-Windows-Setup
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
Compression=lzma2
SolidCompression=yes
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\Keepsake.exe
WizardStyle=modern

[Files]
Source: "{#ArtefactsDir}\VST3\Keepsake.vst3\*"; DestDir: "{commoncf64}\VST3\Keepsake.vst3"; Flags: recursesubdirs ignoreversion
Source: "{#ArtefactsDir}\Standalone\Keepsake.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Keepsake"; Filename: "{app}\Keepsake.exe"

[UninstallDelete]
Type: filesandordirs; Name: "{commoncf64}\VST3\Keepsake.vst3"
