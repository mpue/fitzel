; Fitzel -- installer (Inno Setup 7)
;
; Built by build-installer.bat, which passes the version (from CMakeLists.txt,
; the one place it is written down) and the folder of the MSVC runtime to ship.
; The executables come from build\release\bin (run build-release.bat first),
; everything else straight from the repository and content\.
;
; What is NOT in here, on purpose:
;  - the check harnesses, their pictures and logs, and the editor's own state
;    (editor.json, graphics.json, imgui.ini, recovery\ ...) that sit in bin\;
;  - ffmpeg.exe: a GPLv3 build. Video import finds ffmpeg on the PATH instead;
;  - every piece of content that is not CC0 (Textures.com, bought asset packs,
;    the car). See CONTENT-LICENSES.txt for what is shipped and where it is from.

#ifndef AppVersion
  #error Build this with build-installer.bat -- it passes AppVersion and CrtDir.
#endif

#define Bin      "..\build\release\bin"
#define Content  "..\content"

[Setup]
AppId={{64FDFD62-3A71-48D1-9D21-80675503A4BD}
AppName=Fitzel
AppVersion={#AppVersion}
AppVerName=Fitzel {#AppVersion}
AppPublisher=mpue
AppPublisherURL=https://github.com/mpue/fitzel
AppSupportURL=https://github.com/mpue/fitzel/issues
VersionInfoVersion={#AppVersion}
; Per user, and only per user. The editor keeps editor.json, graphics.json,
; imgui.ini, its crash recovery and -- unless told otherwise -- the projects it
; creates next to its own exe. Under Program Files none of that could be
; written, so there is no "for all users" option to pick by mistake.
PrivilegesRequired=lowest
DefaultDirName={autopf}\Fitzel
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir=..\build\installer
OutputBaseFilename=Fitzel-{#AppVersion}-Setup
; Most of the payload is JPG/PNG, which does not shrink: plain LZMA2 per file
; is as small as solid compression and far quicker to build.
Compression=lzma2
SolidCompression=no
WizardStyle=modern
UninstallDisplayName=Fitzel {#AppVersion}
; sandbox.exe carries no icon resource of its own, so the setup, the shortcuts
; and the entry under Apps all take theirs from this file.
SetupIconFile=..\images\fitzel.ico
UninstallDisplayIcon={app}\fitzel.ico

[Languages]
Name: "de"; MessagesFile: "compiler:Languages\German.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
de.TypeFull=Vollständig
de.TypeCompact=Nur Engine
de.TypeCustom=Benutzerdefiniert
de.CompEngine=Fitzel Editor und Player
de.CompContent=Freie Inhalte (CC0)
de.CompBase=Standard-Texturen für Terrain und Straße (ca. 280 MB)
de.CompMore=Weitere Texturen, eine HDRI und zwei Modelle (ca. 1 GB)
en.TypeFull=Full
en.TypeCompact=Engine only
en.TypeCustom=Custom
en.CompEngine=Fitzel editor and player
en.CompContent=Free content (CC0)
en.CompBase=Default textures for terrain and road (about 280 MB)
en.CompMore=More textures, an HDRI and two models (about 1 GB)

[Types]
Name: "full";    Description: "{cm:TypeFull}"
Name: "compact"; Description: "{cm:TypeCompact}"
Name: "custom";  Description: "{cm:TypeCustom}"; Flags: iscustom

[Components]
Name: "engine";       Description: "{cm:CompEngine}";  Types: full compact custom; Flags: fixed
Name: "content";      Description: "{cm:CompContent}"; Types: full
Name: "content\base"; Description: "{cm:CompBase}";    Types: full
Name: "content\more"; Description: "{cm:CompMore}";    Types: full

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; --- The engine -------------------------------------------------------------------
Source: "{#Bin}\sandbox.exe";              DestDir: "{app}"; Components: engine; Flags: ignoreversion
Source: "{#Bin}\player.exe";               DestDir: "{app}"; Components: engine; Flags: ignoreversion
Source: "{#Bin}\third-party-licenses.md";  DestDir: "{app}"; Components: engine; Flags: ignoreversion
Source: "..\README.md";                    DestDir: "{app}"; Components: engine; Flags: ignoreversion
Source: "..\images\fitzel.ico";            DestDir: "{app}"; Components: engine; Flags: ignoreversion
; Shaders, splash and Lua scripts from the repository, not from bin\: the build
; only ever copies INTO bin\, so a script deleted from the repo lives on there.
Source: "..\sandbox\assets\*";   DestDir: "{app}\assets";  Components: engine; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\images\splash.png";  DestDir: "{app}\assets";  Components: engine; Flags: ignoreversion
Source: "..\sandbox\scripts\*.lua"; DestDir: "{app}\scripts"; Components: engine; Flags: ignoreversion
; The Visual C++ runtime, next to the exe (app-local, as Microsoft allows): the
; build links it dynamically, and not every Windows has the redistributable.
Source: "{#CrtDir}\msvcp140.dll";      DestDir: "{app}"; Components: engine; Flags: ignoreversion
Source: "{#CrtDir}\vcruntime140.dll";  DestDir: "{app}"; Components: engine; Flags: ignoreversion
Source: "{#CrtDir}\vcruntime140_1.dll"; DestDir: "{app}"; Components: engine; Flags: ignoreversion

; --- Content -----------------------------------------------------------------------
; The editor looks for content\ next to its exe before anything else. Each
; pattern also takes the asset's .meta, which holds the id projects refer to it
; by. Per texture set only what the engine reads: the colour map and the
; OpenGL normal map (PNG where the pack has one -- its EXR is DWAA-compressed and
; does not decode). Displacement and roughness maps are never loaded.
Source: "CONTENT-LICENSES.txt"; DestDir: "{app}\content"; Components: content\base content\more; Flags: ignoreversion

; Terrain layers and the road's default surface.
; Tex(set, normal extension): one texture set's colour and normal map, for the
; component currently named in Component.
#define Tex(str Name, str Normal) \
  'Source: "' + Content + '\textures\' + Name + '_diff*_4k.*"; DestDir: "{app}\content\textures"; Components: ' + Component + '; Flags: ignoreversion' + NewLine + \
  'Source: "' + Content + '\textures\' + Name + '_nor_gl_4k.' + Normal + '*"; DestDir: "{app}\content\textures"; Components: ' + Component + '; Flags: ignoreversion' + NewLine
#define Component "content\base"
#emit Tex("coast_sand_01", "png")
#emit Tex("aerial_rocks_01", "png")
#emit Tex("rocky_terrain_02", "png")
#emit Tex("snow_02", "png")
#emit Tex("asphalt_02", "png")
Source: "{#Content}\textures\road_puddles.png*";    DestDir: "{app}\content\textures"; Components: content\base; Flags: ignoreversion
Source: "{#Content}\textures\road_wet_grain.png*";  DestDir: "{app}\content\textures"; Components: content\base; Flags: ignoreversion

#define Component "content\more"
#emit Tex("asphalt_04", "png")
#emit Tex("brown_mud_leaves_01", "png")
#emit Tex("concrete_block_wall_03", "png")
#emit Tex("cracked_concrete_02", "png")
#emit Tex("forest_leaves_02", "png")
#emit Tex("gray_rocks", "png")
#emit Tex("marble_cliff_03", "exr")
#emit Tex("marble_cliff_04", "png")
#emit Tex("moon_flat_macro_02", "png")
#emit Tex("rocky_trail", "exr")
Source: "{#Content}\textures\cowboy_town_hall_4k.exr*"; DestDir: "{app}\content\textures"; Components: content\more; Flags: ignoreversion
Source: "{#Content}\models\boulder_01_4k.glb*";         DestDir: "{app}\content\models";   Components: content\more; Flags: ignoreversion
Source: "{#Content}\models\Camera_01_4k.glb*";          DestDir: "{app}\content\models";   Components: content\more; Flags: ignoreversion
Source: "{#Content}\models\Bark001_2K-JPG_*";           DestDir: "{app}\content\models";   Components: content\more; Flags: ignoreversion
Source: "{#Content}\models\LeafSet024_1K-JPG_*";        DestDir: "{app}\content\models";   Components: content\more; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Fitzel"; Filename: "{app}\sandbox.exe"; WorkingDir: "{app}"; IconFilename: "{app}\fitzel.ico"
Name: "{autodesktop}\Fitzel";  Filename: "{app}\sandbox.exe"; WorkingDir: "{app}"; IconFilename: "{app}\fitzel.ico"; Tasks: desktopicon

[Run]
Filename: "{app}\sandbox.exe"; WorkingDir: "{app}"; Description: "{cm:LaunchProgram,Fitzel}"; Flags: nowait postinstall skipifsilent

; The uninstaller removes what it installed and nothing else: editor.json,
; graphics.json, imgui.ini, recovery\ and above all projects\ -- where the New
; Project wizard puts a user's work by default -- stay where they are.
