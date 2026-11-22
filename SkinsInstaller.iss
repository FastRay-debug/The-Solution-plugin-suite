; The Solution - Skins & Skin Editor (separate installer, Inno Setup 6.3+)
;
; Expects, next to this file:  The Solution\Skins\*.jpg + skins.json   The Solution\SkinEditor\Skin Editor.exe
; Every skin is its own checkbox. Skins go to the shared folder every Solution plugin reads; the Skin
; Editor goes where the plugins' "Skin Editor" button looks for it.
; Build:  ISCC.exe SkinsInstaller.iss   (optional: /DAppVersion=1.2.3)

#ifndef AppVersion
  #define AppVersion "0.2.0"
#endif
#define Src "The Solution"

[Setup]
AppId={{8C1F3B52-6A2E-4D7B-9E1C-3F4A5B6C7D80}
AppName=The Solution - Skins & Skin Editor
AppVersion={#AppVersion}
AppPublisher=Billet Audio
DefaultDirName={commonpf64}\Billet Audio\The Solution
UninstallDisplayName=The Solution - Skins & Skin Editor
OutputDir=OutputInstaller
OutputBaseFilename=The_Solution_Skins_Setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
DisableWelcomePage=yes
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=yes

[Types]
Name: "full";   Description: "Everything"
Name: "custom"; Description: "Choose skins"; Flags: iscustom

[Components]
Name: "editor"; Description: "Skin Editor (make skins from your own photos)"; Types: full custom
Name: "cars";   Description: "Cars"; Types: full custom
Name: "cars\lincoln69"; Description: "Lincoln Continental '69"; Types: full custom
Name: "cars\camaro71"; Description: "Camaro '71"; Types: full custom
Name: "cars\mach1"; Description: "Mustang Mach 1"; Types: full custom
Name: "cars\eviltwin"; Description: "Evil Twin"; Types: full custom
Name: "cars\eldorado59"; Description: "Eldorado '59"; Types: full custom
Name: "cars\cuda70"; Description: "'Cuda '70"; Types: full custom
Name: "cars\gto67"; Description: "GTO '67"; Types: full custom
Name: "places"; Description: "Places"; Types: full custom
Name: "places\rome"; Description: "Rome"; Types: full custom
Name: "places\giza"; Description: "Giza"; Types: full custom
Name: "places\aztec"; Description: "Aztec"; Types: full custom
Name: "places\serengeti"; Description: "Serengeti"; Types: full custom
Name: "places\museum"; Description: "Museum"; Types: full custom
Name: "places\piano"; Description: "Grand Piano"; Types: full custom

[Files]
Source: "{#Src}\SkinEditor\Skin Editor.exe"; DestDir: "{app}\Skin Editor"; Components: editor; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#Src}\Skins\skins.json"; DestDir: "{commonappdata}\Billet Audio\The Solution\Skins"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#Src}\Skins\lincoln69.jpg"; DestDir: "{commonappdata}\Billet Audio\The Solution\Skins"; Components: cars\lincoln69; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#Src}\Skins\camaro71.jpg"; DestDir: "{commonappdata}\Billet Audio\The Solution\Skins"; Components: cars\camaro71; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#Src}\Skins\mach1.jpg"; DestDir: "{commonappdata}\Billet Audio\The Solution\Skins"; Components: cars\mach1; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#Src}\Skins\eviltwin.jpg"; DestDir: "{commonappdata}\Billet Audio\The Solution\Skins"; Components: cars\eviltwin; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#Src}\Skins\eldorado59.jpg"; DestDir: "{commonappdata}\Billet Audio\The Solution\Skins"; Components: cars\eldorado59; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#Src}\Skins\cuda70.jpg"; DestDir: "{commonappdata}\Billet Audio\The Solution\Skins"; Components: cars\cuda70; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#Src}\Skins\gto67.jpg"; DestDir: "{commonappdata}\Billet Audio\The Solution\Skins"; Components: cars\gto67; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#Src}\Skins\rome.jpg"; DestDir: "{commonappdata}\Billet Audio\The Solution\Skins"; Components: places\rome; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#Src}\Skins\giza.jpg"; DestDir: "{commonappdata}\Billet Audio\The Solution\Skins"; Components: places\giza; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#Src}\Skins\aztec.jpg"; DestDir: "{commonappdata}\Billet Audio\The Solution\Skins"; Components: places\aztec; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#Src}\Skins\serengeti.jpg"; DestDir: "{commonappdata}\Billet Audio\The Solution\Skins"; Components: places\serengeti; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#Src}\Skins\museum.jpg"; DestDir: "{commonappdata}\Billet Audio\The Solution\Skins"; Components: places\museum; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#Src}\Skins\piano.jpg"; DestDir: "{commonappdata}\Billet Audio\The Solution\Skins"; Components: places\piano; Flags: ignoreversion skipifsourcedoesntexist

[Icons]
Name: "{commonprograms}\The Solution\Skin Editor"; Filename: "{app}\Skin Editor\Skin Editor.exe"; Components: editor
Name: "{commondesktop}\Skin Editor"; Filename: "{app}\Skin Editor\Skin Editor.exe"; Components: editor; Tasks: desktop

[Tasks]
Name: "desktop"; Description: "Skin Editor shortcut on the desktop"; Components: editor

[Run]
Filename: "{app}\Skin Editor\Skin Editor.exe"; Description: "Open the Skin Editor"; Flags: nowait postinstall skipifsilent; Components: editor
