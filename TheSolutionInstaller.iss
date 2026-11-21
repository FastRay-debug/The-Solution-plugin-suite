; The Solution Suite - Windows installer with checkboxes (Inno Setup 6.3+)
;
; Expects the built plugins gathered next to this file in "The Solution\":
;   The Solution\VST3\*.vst3   The Solution\CLAP\*.clap   The Solution\Standalone\*.exe
;   The Solution\Skins\*.jpg   The Solution\AAX\*.aaxplugin (optional)
; Install.bat (option E) and the GitHub Actions build both create that folder.
;
; Build:  ISCC.exe TheSolutionInstaller.iss            (optional: /DAppVersion=1.2.3)

#ifndef AppVersion
  #define AppVersion "0.2.0"
#endif
#define Src "The Solution"
#define HasAAX DirExists(AddBackslash(SourcePath) + Src + "\AAX")

[Setup]
AppId={{33435123-E4D3-4049-893B-DA50E43A3F7E}
AppName=The Solution Suite
AppVersion={#AppVersion}
AppVerName=The Solution Suite {#AppVersion}
AppPublisher=Fast Ray Media
DefaultDirName={commonpf64}\Fast Ray Media\The Solution
DefaultGroupName=The Solution
UninstallDisplayName=The Solution Suite
OutputDir=OutputInstaller
OutputBaseFilename=The_Solution_Suite_Setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
CloseApplications=yes
; Pages: checkboxes -> install -> done. Everything else is automatic.
DisableWelcomePage=yes
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=yes
DisableFinishedPage=no

[Types]
Name: "full";   Description: "Everything"
Name: "custom"; Description: "Choose what to install"; Flags: iscustom

[Components]
Name: "formats";            Description: "Plugin formats";                      Types: full custom
Name: "formats\vst3";       Description: "VST3  (most DAWs)";                   Types: full custom
Name: "formats\clap";       Description: "CLAP  (Bitwig, Reaper, FL Studio)";   Types: full
#if HasAAX
Name: "formats\aax";        Description: "AAX  (Pro Tools)";                    Types: full
#endif
Name: "formats\standalone"; Description: "Standalone apps + Start Menu shortcuts"; Types: full
Name: "utility";            Description: "Solution Utility (Fast Ray diagnostics: CPU cores, GPU, every Solution plugin)"; Types: full custom
Name: "skins";              Description: "Skin faceplates (shared by all modules)"; Types: full custom
Name: "modules";            Description: "Modules";                             Types: full custom
Name: "modules\bunker"; Description: "The Bunker"; Types: full custom
Name: "modules\stage"; Description: "The Stage"; Types: full custom
Name: "modules\land"; Description: "The Land"; Types: full custom
Name: "modules\loft"; Description: "The Loft"; Types: full custom
Name: "modules\orbit"; Description: "The Orbit"; Types: full custom
Name: "modules\bridge"; Description: "The Bridge"; Types: full custom
Name: "modules\void"; Description: "The Void"; Types: full custom
Name: "modules\anchor"; Description: "The Anchor"; Types: full custom
Name: "modules\aether"; Description: "The Aether"; Types: full custom
Name: "modules\mosaic"; Description: "The Mosaic"; Types: full custom
Name: "modules\innerspace"; Description: "Innerspace"; Types: full custom
Name: "modules\matrix"; Description: "The Matrix"; Types: full custom
Name: "modules\scope"; Description: "The Scope"; Types: full custom

[Files]
Source: "{#Src}\Utility\Solution Utility.exe"; DestDir: "{app}\Utility"; Components: utility; Flags: ignoreversion skipifsourcedoesntexist
; The Bunker
Source: "{#Src}\VST3\The Bunker.vst3\*"; DestDir: "{commoncf64}\VST3\The Solution\The Bunker.vst3"; Components: formats\vst3 and modules\bunker; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "{#Src}\CLAP\The Bunker.clap"; DestDir: "{commoncf64}\CLAP\The Solution"; Components: formats\clap and modules\bunker; Flags: ignoreversion skipifsourcedoesntexist
#if HasAAX
Source: "{#Src}\AAX\The Bunker.aaxplugin\*"; DestDir: "{commoncf64}\Avid\Audio\Plug-Ins\The Solution\The Bunker.aaxplugin"; Components: formats\aax and modules\bunker; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
#endif
Source: "{#Src}\Standalone\The Bunker.exe"; DestDir: "{app}\Standalone"; Components: formats\standalone and modules\bunker; Flags: ignoreversion skipifsourcedoesntexist
; The Stage
Source: "{#Src}\VST3\The Stage.vst3\*"; DestDir: "{commoncf64}\VST3\The Solution\The Stage.vst3"; Components: formats\vst3 and modules\stage; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "{#Src}\CLAP\The Stage.clap"; DestDir: "{commoncf64}\CLAP\The Solution"; Components: formats\clap and modules\stage; Flags: ignoreversion skipifsourcedoesntexist
#if HasAAX
Source: "{#Src}\AAX\The Stage.aaxplugin\*"; DestDir: "{commoncf64}\Avid\Audio\Plug-Ins\The Solution\The Stage.aaxplugin"; Components: formats\aax and modules\stage; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
#endif
Source: "{#Src}\Standalone\The Stage.exe"; DestDir: "{app}\Standalone"; Components: formats\standalone and modules\stage; Flags: ignoreversion skipifsourcedoesntexist
; The Land
Source: "{#Src}\VST3\The Land.vst3\*"; DestDir: "{commoncf64}\VST3\The Solution\The Land.vst3"; Components: formats\vst3 and modules\land; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "{#Src}\CLAP\The Land.clap"; DestDir: "{commoncf64}\CLAP\The Solution"; Components: formats\clap and modules\land; Flags: ignoreversion skipifsourcedoesntexist
#if HasAAX
Source: "{#Src}\AAX\The Land.aaxplugin\*"; DestDir: "{commoncf64}\Avid\Audio\Plug-Ins\The Solution\The Land.aaxplugin"; Components: formats\aax and modules\land; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
#endif
Source: "{#Src}\Standalone\The Land.exe"; DestDir: "{app}\Standalone"; Components: formats\standalone and modules\land; Flags: ignoreversion skipifsourcedoesntexist
; The Loft
Source: "{#Src}\VST3\The Loft.vst3\*"; DestDir: "{commoncf64}\VST3\The Solution\The Loft.vst3"; Components: formats\vst3 and modules\loft; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "{#Src}\CLAP\The Loft.clap"; DestDir: "{commoncf64}\CLAP\The Solution"; Components: formats\clap and modules\loft; Flags: ignoreversion skipifsourcedoesntexist
#if HasAAX
Source: "{#Src}\AAX\The Loft.aaxplugin\*"; DestDir: "{commoncf64}\Avid\Audio\Plug-Ins\The Solution\The Loft.aaxplugin"; Components: formats\aax and modules\loft; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
#endif
Source: "{#Src}\Standalone\The Loft.exe"; DestDir: "{app}\Standalone"; Components: formats\standalone and modules\loft; Flags: ignoreversion skipifsourcedoesntexist
; The Orbit
Source: "{#Src}\VST3\The Orbit.vst3\*"; DestDir: "{commoncf64}\VST3\The Solution\The Orbit.vst3"; Components: formats\vst3 and modules\orbit; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "{#Src}\CLAP\The Orbit.clap"; DestDir: "{commoncf64}\CLAP\The Solution"; Components: formats\clap and modules\orbit; Flags: ignoreversion skipifsourcedoesntexist
#if HasAAX
Source: "{#Src}\AAX\The Orbit.aaxplugin\*"; DestDir: "{commoncf64}\Avid\Audio\Plug-Ins\The Solution\The Orbit.aaxplugin"; Components: formats\aax and modules\orbit; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
#endif
Source: "{#Src}\Standalone\The Orbit.exe"; DestDir: "{app}\Standalone"; Components: formats\standalone and modules\orbit; Flags: ignoreversion skipifsourcedoesntexist
; The Bridge
Source: "{#Src}\VST3\The Bridge.vst3\*"; DestDir: "{commoncf64}\VST3\The Solution\The Bridge.vst3"; Components: formats\vst3 and modules\bridge; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "{#Src}\CLAP\The Bridge.clap"; DestDir: "{commoncf64}\CLAP\The Solution"; Components: formats\clap and modules\bridge; Flags: ignoreversion skipifsourcedoesntexist
#if HasAAX
Source: "{#Src}\AAX\The Bridge.aaxplugin\*"; DestDir: "{commoncf64}\Avid\Audio\Plug-Ins\The Solution\The Bridge.aaxplugin"; Components: formats\aax and modules\bridge; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
#endif
Source: "{#Src}\Standalone\The Bridge.exe"; DestDir: "{app}\Standalone"; Components: formats\standalone and modules\bridge; Flags: ignoreversion skipifsourcedoesntexist
; The Void
Source: "{#Src}\VST3\The Void.vst3\*"; DestDir: "{commoncf64}\VST3\The Solution\The Void.vst3"; Components: formats\vst3 and modules\void; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "{#Src}\CLAP\The Void.clap"; DestDir: "{commoncf64}\CLAP\The Solution"; Components: formats\clap and modules\void; Flags: ignoreversion skipifsourcedoesntexist
#if HasAAX
Source: "{#Src}\AAX\The Void.aaxplugin\*"; DestDir: "{commoncf64}\Avid\Audio\Plug-Ins\The Solution\The Void.aaxplugin"; Components: formats\aax and modules\void; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
#endif
Source: "{#Src}\Standalone\The Void.exe"; DestDir: "{app}\Standalone"; Components: formats\standalone and modules\void; Flags: ignoreversion skipifsourcedoesntexist
; The Anchor
Source: "{#Src}\VST3\The Anchor.vst3\*"; DestDir: "{commoncf64}\VST3\The Solution\The Anchor.vst3"; Components: formats\vst3 and modules\anchor; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "{#Src}\CLAP\The Anchor.clap"; DestDir: "{commoncf64}\CLAP\The Solution"; Components: formats\clap and modules\anchor; Flags: ignoreversion skipifsourcedoesntexist
#if HasAAX
Source: "{#Src}\AAX\The Anchor.aaxplugin\*"; DestDir: "{commoncf64}\Avid\Audio\Plug-Ins\The Solution\The Anchor.aaxplugin"; Components: formats\aax and modules\anchor; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
#endif
Source: "{#Src}\Standalone\The Anchor.exe"; DestDir: "{app}\Standalone"; Components: formats\standalone and modules\anchor; Flags: ignoreversion skipifsourcedoesntexist
; The Aether
Source: "{#Src}\VST3\The Aether.vst3\*"; DestDir: "{commoncf64}\VST3\The Solution\The Aether.vst3"; Components: formats\vst3 and modules\aether; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "{#Src}\CLAP\The Aether.clap"; DestDir: "{commoncf64}\CLAP\The Solution"; Components: formats\clap and modules\aether; Flags: ignoreversion skipifsourcedoesntexist
#if HasAAX
Source: "{#Src}\AAX\The Aether.aaxplugin\*"; DestDir: "{commoncf64}\Avid\Audio\Plug-Ins\The Solution\The Aether.aaxplugin"; Components: formats\aax and modules\aether; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
#endif
Source: "{#Src}\Standalone\The Aether.exe"; DestDir: "{app}\Standalone"; Components: formats\standalone and modules\aether; Flags: ignoreversion skipifsourcedoesntexist
; The Mosaic
Source: "{#Src}\VST3\The Mosaic.vst3\*"; DestDir: "{commoncf64}\VST3\The Solution\The Mosaic.vst3"; Components: formats\vst3 and modules\mosaic; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "{#Src}\CLAP\The Mosaic.clap"; DestDir: "{commoncf64}\CLAP\The Solution"; Components: formats\clap and modules\mosaic; Flags: ignoreversion skipifsourcedoesntexist
#if HasAAX
Source: "{#Src}\AAX\The Mosaic.aaxplugin\*"; DestDir: "{commoncf64}\Avid\Audio\Plug-Ins\The Solution\The Mosaic.aaxplugin"; Components: formats\aax and modules\mosaic; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
#endif
Source: "{#Src}\Standalone\The Mosaic.exe"; DestDir: "{app}\Standalone"; Components: formats\standalone and modules\mosaic; Flags: ignoreversion skipifsourcedoesntexist
; Innerspace
Source: "{#Src}\VST3\Innerspace.vst3\*"; DestDir: "{commoncf64}\VST3\The Solution\Innerspace.vst3"; Components: formats\vst3 and modules\innerspace; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "{#Src}\CLAP\Innerspace.clap"; DestDir: "{commoncf64}\CLAP\The Solution"; Components: formats\clap and modules\innerspace; Flags: ignoreversion skipifsourcedoesntexist
#if HasAAX
Source: "{#Src}\AAX\Innerspace.aaxplugin\*"; DestDir: "{commoncf64}\Avid\Audio\Plug-Ins\The Solution\Innerspace.aaxplugin"; Components: formats\aax and modules\innerspace; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
#endif
Source: "{#Src}\Standalone\Innerspace.exe"; DestDir: "{app}\Standalone"; Components: formats\standalone and modules\innerspace; Flags: ignoreversion skipifsourcedoesntexist
; The Matrix
Source: "{#Src}\VST3\The Matrix.vst3\*"; DestDir: "{commoncf64}\VST3\The Solution\The Matrix.vst3"; Components: formats\vst3 and modules\matrix; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "{#Src}\CLAP\The Matrix.clap"; DestDir: "{commoncf64}\CLAP\The Solution"; Components: formats\clap and modules\matrix; Flags: ignoreversion skipifsourcedoesntexist
#if HasAAX
Source: "{#Src}\AAX\The Matrix.aaxplugin\*"; DestDir: "{commoncf64}\Avid\Audio\Plug-Ins\The Solution\The Matrix.aaxplugin"; Components: formats\aax and modules\matrix; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
#endif
Source: "{#Src}\Standalone\The Matrix.exe"; DestDir: "{app}\Standalone"; Components: formats\standalone and modules\matrix; Flags: ignoreversion skipifsourcedoesntexist
; The Scope
Source: "{#Src}\VST3\The Scope.vst3\*"; DestDir: "{commoncf64}\VST3\The Solution\The Scope.vst3"; Components: formats\vst3 and modules\scope; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "{#Src}\CLAP\The Scope.clap"; DestDir: "{commoncf64}\CLAP\The Solution"; Components: formats\clap and modules\scope; Flags: ignoreversion skipifsourcedoesntexist
#if HasAAX
Source: "{#Src}\AAX\The Scope.aaxplugin\*"; DestDir: "{commoncf64}\Avid\Audio\Plug-Ins\The Solution\The Scope.aaxplugin"; Components: formats\aax and modules\scope; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
#endif
Source: "{#Src}\Standalone\The Scope.exe"; DestDir: "{app}\Standalone"; Components: formats\standalone and modules\scope; Flags: ignoreversion skipifsourcedoesntexist
; Shared skins
Source: "{#Src}\Skins\*.jpg"; DestDir: "{commonappdata}\Fast Ray Media\The Solution\Skins"; Components: skins; Flags: ignoreversion skipifsourcedoesntexist

[Icons]
Name: "{group}\Solution Utility"; Filename: "{app}\Utility\Solution Utility.exe"; Components: utility
Name: "{group}\The Bunker"; Filename: "{app}\Standalone\The Bunker.exe"; Components: formats\standalone and modules\bunker; Check: StandaloneExists('The Bunker')
Name: "{group}\The Stage"; Filename: "{app}\Standalone\The Stage.exe"; Components: formats\standalone and modules\stage; Check: StandaloneExists('The Stage')
Name: "{group}\The Land"; Filename: "{app}\Standalone\The Land.exe"; Components: formats\standalone and modules\land; Check: StandaloneExists('The Land')
Name: "{group}\The Loft"; Filename: "{app}\Standalone\The Loft.exe"; Components: formats\standalone and modules\loft; Check: StandaloneExists('The Loft')
Name: "{group}\The Orbit"; Filename: "{app}\Standalone\The Orbit.exe"; Components: formats\standalone and modules\orbit; Check: StandaloneExists('The Orbit')
Name: "{group}\The Bridge"; Filename: "{app}\Standalone\The Bridge.exe"; Components: formats\standalone and modules\bridge; Check: StandaloneExists('The Bridge')
Name: "{group}\The Void"; Filename: "{app}\Standalone\The Void.exe"; Components: formats\standalone and modules\void; Check: StandaloneExists('The Void')
Name: "{group}\The Anchor"; Filename: "{app}\Standalone\The Anchor.exe"; Components: formats\standalone and modules\anchor; Check: StandaloneExists('The Anchor')
Name: "{group}\The Aether"; Filename: "{app}\Standalone\The Aether.exe"; Components: formats\standalone and modules\aether; Check: StandaloneExists('The Aether')
Name: "{group}\The Mosaic"; Filename: "{app}\Standalone\The Mosaic.exe"; Components: formats\standalone and modules\mosaic; Check: StandaloneExists('The Mosaic')
Name: "{group}\Innerspace"; Filename: "{app}\Standalone\Innerspace.exe"; Components: formats\standalone and modules\innerspace; Check: StandaloneExists('Innerspace')
Name: "{group}\The Matrix"; Filename: "{app}\Standalone\The Matrix.exe"; Components: formats\standalone and modules\matrix; Check: StandaloneExists('The Matrix')
Name: "{group}\The Scope"; Filename: "{app}\Standalone\The Scope.exe"; Components: formats\standalone and modules\scope; Check: StandaloneExists('The Scope')
Name: "{group}\Uninstall The Solution Suite"; Filename: "{uninstallexe}"

[Code]
function StandaloneExists(Name: String): Boolean;
begin
  Result := FileExists(ExpandConstant('{app}\Standalone\') + Name + '.exe');
end;

[Messages]
SelectComponentsLabel2=Tick what you want installed, then click Install.
FinishedLabelNoIcons=The Solution Suite is installed.%n%nOpen your DAW and rescan plugins - you'll find the modules under Fast Ray Media / The Solution.
FinishedLabel=The Solution Suite is installed.%n%nOpen your DAW and rescan plugins - you'll find the modules under Fast Ray Media / The Solution.
