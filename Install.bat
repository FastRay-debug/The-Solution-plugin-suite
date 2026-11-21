@echo off
if /i not "%~1"=="__KEEP__" (
    cmd /k ""%~f0" __KEEP__"
    exit /b
)
setlocal enabledelayedexpansion
cd /d "%~dp0"

set "LOCAL_DIR=%~dp0The Solution"
set "VST3_SYS=C:\Program Files\Common Files\VST3\The Solution"
set "CLAP_SYS=C:\Program Files\Common Files\CLAP\The Solution"
set "AAX_SYS=C:\Program Files\Common Files\Avid\Audio\Plug-Ins\The Solution"
set "SKINS_SYS=%ProgramData%\Billet Audio\The Solution\Skins"
set "HAS_AAX=0"
if exist "aax_sdk\Interfaces\AAX.h" set "HAS_AAX=1"
if exist "C:\SDKs\AvidAAXSDK\Interfaces\AAX.h" set "HAS_AAX=1"
for /d %%A in ("C:\SDKs\AvidAAXSDK\*") do if exist "%%A\Interfaces\AAX.h" set "HAS_AAX=1"

:: Default Pre-Run Configuration
set "AUTO_DEPS=ON"
set "BUNDLE_SKINS=OFF"
set "CLEAN_BEFORE=OFF"
set "AUTO_INSTALL=ON"
set "BUILD_EXE=ON"
set "PICK=0"
set "PICK_LABEL=ALL 13 MODULES"

set "M1=TheBunker"  & set "N1=The Bunker"
set "M2=TheStage"   & set "N2=The Stage"
set "M3=TheLand"    & set "N3=The Land"
set "M4=TheLoft"    & set "N4=The Loft"
set "M5=TheOrbit"   & set "N5=The Orbit"
set "M6=TheBridge"  & set "N6=The Bridge"
set "M7=TheVoid"    & set "N7=The Void"
set "M8=TheAnchor"  & set "N8=The Anchor"
set "M9=TheAether"  & set "N9=The Aether"
set "M10=TheMosaic" & set "N10=The Mosaic"
set "M11=Innerspace"& set "N11=Innerspace"
set "M12=TheMatrix" & set "N12=The Matrix"
set "M13=TheScope"  & set "N13=The Scope"

:: Silent Pre-Flight Dependency Scan
set "NEED_CMAKE=0"
set "NEED_GIT=0"
set "NEED_VS=0"
set "NEED_WV2=0"

where cmake >nul 2>&1
if not errorlevel 1 goto :cmake_ok
if exist "%ProgramFiles%\CMake\bin\cmake.exe" goto :cmake_add_path
set "NEED_CMAKE=1"
goto :check_git
:cmake_add_path
set "PATH=%ProgramFiles%\CMake\bin;%PATH%"
:cmake_ok

:check_git
where git >nul 2>&1
if not errorlevel 1 goto :git_ok
if exist "%ProgramFiles%\Git\cmd\git.exe" goto :git_add_path
set "NEED_GIT=1"
goto :check_vs
:git_add_path
set "PATH=%ProgramFiles%\Git\cmd;%PATH%"
:git_ok

:check_vs
if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" goto :vs_ok
set "NEED_VS=1"
:vs_ok

:check_wv2
if exist "webview2\build\native\include\WebView2.h" goto :wv2_ok
set "NEED_WV2=1"
:wv2_ok

set /a ANY_MISSING=NEED_CMAKE+NEED_GIT+NEED_VS+NEED_WV2
set "DEP_STATUS=ALL READY"
if %ANY_MISSING% GTR 0 set "DEP_STATUS=%ANY_MISSING% MISSING (Will Auto-Install)"

:config_menu
set "CHK_D=[ ] OFF"
if /i "%AUTO_DEPS%"=="ON" set "CHK_D=[x] ON "
set "CHK_T=[ ] OFF"
if /i "%BUNDLE_SKINS%"=="ON" set "CHK_T=[x] ON "
set "CHK_C=[ ] OFF"
if /i "%CLEAN_BEFORE%"=="ON" set "CHK_C=[x] ON "
set "CHK_A=[ ] OFF"
if /i "%AUTO_INSTALL%"=="ON" set "CHK_A=[x] ON "
set "CHK_E=[ ] OFF"
if /i "%BUILD_EXE%"=="ON" set "CHK_E=[x] ON "

cls
echo ============================================================
echo   The Solution Suite - Configure ^& Run ^(13 Modules^)
echo ============================================================
echo   PRE-FLIGHT DEPS : %DEP_STATUS%
echo.
echo   TOGGLE OPTIONS ^(Type letter + Enter to flip [x] ON / [ ] OFF^):
echo   ------------------------------------------------------------
echo     [D]  %CHK_D%  Auto-Install Missing Tools ^& WebView2 SDK
echo     [T]  %CHK_T%  Embed Skins In Every Plugin ^(OFF = One Shared Skins Folder, Smaller^)
echo     [C]  %CHK_C%  Clean build\ Folder First ^(Keeps deps_cache^)
echo     [A]  %CHK_A%  Install VST3/CLAP/AAX to System + Smoke Launch
echo     [E]  %CHK_E%  Build Standalone The_Solution_Suite_Setup.exe ^(Inno Setup^)
echo.
echo   BUILD TARGETS ^(Selected: ^>^> %PICK_LABEL% ^<^<^):
echo   ------------------------------------------------------------
for /L %%I in (1,1,13) do (
    set "ST=NOT INSTALLED"
    if exist "%VST3_SYS%\!N%%I!.vst3" set "ST=INSTALLED"
    echo     %%I. !N%%I!  - !ST!
)
echo   ------------------------------------------------------------
echo   Change Targets: [0] All 13  [M] Missing Only  [1-13 / 1,2,13]
echo                   [I] Gather/Install Only ^(No Compile^)
echo   Other:          [X] Clean "The Solution\"   [Q] Quit
echo   ============================================================
echo   Step 1: Type D, T, C, A, E or target ^(0/M/1-13/I^) to change setup.
echo   Step 2: Press [R] or [ENTER] to RUN everything with no prompts!
echo ============================================================
echo.
set "CMD="
set /p CMD="Toggle (D/T/C/A/E), Target (0/M/1-13/I), or [ENTER]/R to RUN: "

if "%CMD%"=="" goto :run_all
if /i "%CMD%"=="R" goto :run_all
if /i "%CMD%"=="Q" exit /b 0
if /i "%CMD%"=="D" goto :tog_deps
if /i "%CMD%"=="T" goto :tog_skins
if /i "%CMD%"=="C" goto :tog_clean
if /i "%CMD%"=="A" goto :tog_install
if /i "%CMD%"=="E" goto :tog_exe
if /i "%CMD%"=="X" goto :clean_collected
if /i "%CMD%"=="I" (
    set "PICK=I"
    set "PICK_LABEL=GATHER / INSTALL ONLY (NO COMPILE)"
    goto :config_menu
)
if /i "%CMD%"=="0" (
    set "PICK=0"
    set "PICK_LABEL=ALL 13 MODULES"
    goto :config_menu
)
if /i "%CMD%"=="M" (
    set "PICK=M"
    set "PICK_LABEL=MISSING MODULES ONLY"
    goto :config_menu
)

set "VALID_MOD=0"
for %%K in (%CMD%) do (
    if defined M%%K set "VALID_MOD=1"
)
if "%VALID_MOD%"=="1" (
    set "PICK=%CMD%"
    set "PICK_LABEL=CUSTOM MODULES: %CMD%"
    goto :config_menu
)
goto :config_menu

:tog_deps
if /i "%AUTO_DEPS%"=="ON" (set "AUTO_DEPS=OFF") else (set "AUTO_DEPS=ON")
goto :config_menu

:tog_skins
if /i "%BUNDLE_SKINS%"=="ON" (set "BUNDLE_SKINS=OFF") else (set "BUNDLE_SKINS=ON")
goto :config_menu

:tog_clean
if /i "%CLEAN_BEFORE%"=="ON" (set "CLEAN_BEFORE=OFF") else (set "CLEAN_BEFORE=ON")
goto :config_menu

:tog_install
if /i "%AUTO_INSTALL%"=="ON" (set "AUTO_INSTALL=OFF") else (set "AUTO_INSTALL=ON")
goto :config_menu

:tog_exe
if /i "%BUILD_EXE%"=="ON" (set "BUILD_EXE=OFF") else (set "BUILD_EXE=ON")
goto :config_menu

:clean_collected
echo.
if exist "%LOCAL_DIR%" rmdir /s /q "%LOCAL_DIR%"
echo [OK] Cleaned "%LOCAL_DIR%".
timeout /t 2 >nul
goto :config_menu

:run_all
cls
echo ============================================================
echo   Running Automated Pipeline: %PICK_LABEL%
echo   Skins: %BUNDLE_SKINS% ^| Clean: %CLEAN_BEFORE% ^| Auto-Install: %AUTO_INSTALL%
echo ============================================================
echo.

if /i "%AUTO_DEPS%"=="ON" if %ANY_MISSING% GTR 0 (
    echo [Step 1/4] Installing missing dependencies...
    if %NEED_CMAKE%==1 (
        winget install --id Kitware.CMake -e --source winget --accept-package-agreements --accept-source-agreements
        if exist "%ProgramFiles%\CMake\bin\cmake.exe" set "PATH=%ProgramFiles%\CMake\bin;%PATH%"
    )
    if %NEED_GIT%==1 (
        winget install --id Git.Git -e --source winget --accept-package-agreements --accept-source-agreements
        if exist "%ProgramFiles%\Git\cmd\git.exe" set "PATH=%ProgramFiles%\Git\cmd;%PATH%"
    )
    if %NEED_VS%==1 (
        winget install --id Microsoft.VisualStudio.2022.BuildTools -e --source winget --accept-package-agreements --accept-source-agreements --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
    )
    if %NEED_WV2%==1 (
        powershell -NoProfile -Command "[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12; Invoke-WebRequest -Uri 'https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2' -OutFile 'webview2.zip'; Expand-Archive -Path 'webview2.zip' -DestinationPath 'webview2' -Force; Remove-Item 'webview2.zip' -Force"
    )
) else (
    echo [Step 1/4] Dependencies ready.
)

if /i "%CLEAN_BEFORE%"=="ON" (
    echo [Step 2/4] Cleaning build\ folder ^(keeping deps_cache ^& webview2^)...
    if exist "build" rmdir /s /q "build"
    if exist "build_log.txt" del /f /q "build_log.txt"
) else (
    echo [Step 2/4] Skipping clean ^(incremental build^).
)

if /i "%PICK%"=="I" (
    echo [Step 3/4] Skipping compile ^(Gather/Install Only^)...
    set "FIRST_EXE=The Stage.exe"
    set "INST_ARG=0"
    goto :step_install
)

set "TARGETS="
set "FIRST_EXE="
set "BUILD_ALL=0"
set "INST_ARG=%PICK%"

if "%PICK%"=="0" (
    set "BUILD_ALL=1"
    set "FIRST_EXE=The Stage.exe"
) else if /i "%PICK%"=="M" (
    for /L %%I in (1,1,13) do (
        if not exist "%VST3_SYS%\!N%%I!.vst3" (
            set "TARGETS=!TARGETS! !M%%I!_VST3 !M%%I!_CLAP !M%%I!_Standalone"
            if "%HAS_AAX%"=="1" set "TARGETS=!TARGETS! !M%%I!_AAX"
            if "!FIRST_EXE!"=="" set "FIRST_EXE=!N%%I!.exe"
        )
    )
    if "!TARGETS!"=="" (
        echo [ALL INSTALLED] Every module is already installed in "%VST3_SYS%".
        pause
        goto :config_menu
    )
) else (
    for %%K in (%PICK%) do (
        if defined M%%K (
            set "TARGETS=!TARGETS! !M%%K!_VST3 !M%%K!_CLAP !M%%K!_Standalone"
        if "%HAS_AAX%"=="1" set "TARGETS=!TARGETS! !M%%K!_AAX"
            if "!FIRST_EXE!"=="" set "FIRST_EXE=!N%%K!.exe"
        )
    )
)

echo [Step 3/4] Configuring ^& compiling ^(BUNDLE_SKINS=%BUNDLE_SKINS%^)...
powershell -NoProfile -Command "cmake -B build -DBUNDLE_SKINS=%BUNDLE_SKINS% 2>&1 | Tee-Object -FilePath build_log.txt; exit $LASTEXITCODE"
if errorlevel 1 (
    echo [ERROR] CMake configure failed. Opening build_log.txt...
    notepad build_log.txt
    pause
    goto :config_menu
)

if "%BUILD_ALL%"=="1" (
    powershell -NoProfile -Command "cmake --build build --config Release 2>&1 | Tee-Object -FilePath build_log.txt -Append; exit $LASTEXITCODE"
) else (
    powershell -NoProfile -Command "cmake --build build --config Release --target %TARGETS% 2>&1 | Tee-Object -FilePath build_log.txt -Append; exit $LASTEXITCODE"
)
if errorlevel 1 (
    echo [ERROR] Build failed. Opening build_log.txt...
    notepad build_log.txt
    pause
    goto :config_menu
)

:step_install
echo [Step 4/4] Gathering plugins...
if not exist "%LOCAL_DIR%\VST3" mkdir "%LOCAL_DIR%\VST3"
if not exist "%LOCAL_DIR%\CLAP" mkdir "%LOCAL_DIR%\CLAP"
if not exist "%LOCAL_DIR%\Standalone" mkdir "%LOCAL_DIR%\Standalone"
if not exist "%LOCAL_DIR%\AAX" mkdir "%LOCAL_DIR%\AAX"
if not exist "%LOCAL_DIR%\Skins" mkdir "%LOCAL_DIR%\Skins"
copy /Y "UI\skins\*.jpg" "%LOCAL_DIR%\Skins\" >nul

set /a COUNT=0
if "%INST_ARG%"=="0" (
    for /L %%I in (1,1,13) do call :copy_one "!N%%I!"
) else if /i "%INST_ARG%"=="M" (
    for /L %%I in (1,1,13) do (
        if not exist "%VST3_SYS%\!N%%I!.vst3" call :copy_one "!N%%I!"
    )
) else (
    for %%K in (%INST_ARG%) do (
        if defined N%%K call :copy_one "!N%%K!"
    )
)

if /i "%AUTO_INSTALL%"=="ON" if %COUNT% GTR 0 (
    echo Installing shared skins to "%SKINS_SYS%"...
    if not exist "%SKINS_SYS%" mkdir "%SKINS_SYS%"
    copy /Y "%LOCAL_DIR%\Skins\*.jpg" "%SKINS_SYS%\" >nul
    echo Installing VST3 and CLAP plugins to system Common Files...
    powershell -NoProfile -Command "Start-Process cmd -Verb RunAs -Wait -ArgumentList '/c if not exist \"%VST3_SYS%\" mkdir \"%VST3_SYS%\" & xcopy \"%LOCAL_DIR%\VST3\*\" \"%VST3_SYS%\\\" /E /I /Y /Q & if not exist \"%CLAP_SYS%\" mkdir \"%CLAP_SYS%\" & copy /Y \"%LOCAL_DIR%\CLAP\*.clap\" \"%CLAP_SYS%\\\" & if exist \"%LOCAL_DIR%\AAX\*.aaxplugin\" (if not exist \"%AAX_SYS%\" mkdir \"%AAX_SYS%\" & xcopy \"%LOCAL_DIR%\AAX\*\" \"%AAX_SYS%\\\" /E /I /Y /Q)'"
    start "" "%LOCAL_DIR%"
    if not "%FIRST_EXE%"=="" (
        if exist "%LOCAL_DIR%\Standalone\%FIRST_EXE%" start "" "%LOCAL_DIR%\Standalone\%FIRST_EXE%"
    )
)

echo.
if /i not "%BUILD_EXE%"=="ON" goto :skip_inno
if not exist "TheSolutionInstaller.iss" (
    echo [Step 5/5] Skipping installer .exe: TheSolutionInstaller.iss not found.
    goto :skip_inno
)
echo [Step 5/5] Building Standalone The_Solution_Suite_Setup.exe via Inno Setup...
set "ISCC="
for /f "delims=" %%I in ('where ISCC.exe 2^>nul') do set "ISCC=%%I"
if not defined ISCC if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not defined ISCC if exist "%ProgramFiles%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"
if not defined ISCC if exist "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not defined ISCC (
    winget install --id JRSoftware.InnoSetup -e --source winget --accept-package-agreements --accept-source-agreements
    if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
    if exist "%ProgramFiles%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"
    if exist "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
)
if defined ISCC (
    "%ISCC%" "TheSolutionInstaller.iss"
    if exist "OutputInstaller\The_Solution_Suite_Setup.exe" start "" "OutputInstaller"
) else (
    echo [WARN] ISCC.exe not found; plugins are still installed in The Solution\ and system folders.
)
:skip_inno
echo.
echo ============================================================
echo   [SUCCESS] Automated Pipeline Complete!
echo ============================================================
pause
goto :config_menu

:copy_one
set "PNAME=%~1"
for /d /r "build" %%D in (*.vst3) do (
    if /i "%%~nxD"=="%PNAME%.vst3" if exist "%%D\Contents" (
        xcopy "%%D" "%LOCAL_DIR%\VST3\%%~nxD\" /E /I /Y /Q >nul
        set /a COUNT+=1
    )
)
for /r "build" %%F in (*.clap) do (
    if /i "%%~nxF"=="%PNAME%.clap" (
        copy /Y "%%F" "%LOCAL_DIR%\CLAP\%%~nxF" >nul
        set /a COUNT+=1
    )
)
for /r "build" %%F in (*.exe) do (
    if /i "%%~nxF"=="%PNAME%.exe" (
        copy /Y "%%F" "%LOCAL_DIR%\Standalone\%%~nxF" >nul
        set /a COUNT+=1
    )
)
for /d /r "build" %%D in (*.aaxplugin) do (
    if /i "%%~nxD"=="%PNAME%.aaxplugin" (
        xcopy "%%D" "%LOCAL_DIR%\AAX\%%~nxD\" /E /I /Y /Q >nul
        set /a COUNT+=1
    )
)
goto :eof
