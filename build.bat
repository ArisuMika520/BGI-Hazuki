@echo off
setlocal
chcp 65001 >nul

echo [BGI_Hazuki] 正在定位 Visual Studio MSVC 环境...
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo 未找到 vswhere.exe，请确认已安装 Visual Studio。
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"

if "%VSROOT%"=="" (
    echo 未找到可用的 MSVC 工具链。
    exit /b 1
)

call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo MSVC 环境初始化失败。
    exit /b 1
)

if not exist bin mkdir bin
if exist obj rmdir /s /q obj
if not exist obj mkdir obj
if not exist obj\image mkdir obj\image
if not exist obj\text mkdir obj\text
if not exist obj\asset mkdir obj\asset
if not exist obj\core mkdir obj\core
if not exist obj\gui mkdir obj\gui

powershell -NoProfile -ExecutionPolicy Bypass -File tools\make_icon.ps1 -Source static\icon.png -Output static\icon.ico
if errorlevel 1 (
    echo 图标生成失败。
    exit /b 1
)

rc /nologo /foobj\app_icon.res resources\app_icon.rc
if errorlevel 1 (
    echo 图标资源编译失败。
    exit /b 1
)

del /q *.obj 2>nul
del /q *.pdb 2>nul
del /q *.ilk 2>nul
if exist BGI_Unpacker\build rmdir /s /q BGI_Unpacker\build

echo [BGI_Hazuki] 开始编译...
cl /nologo /std:c++17 /EHsc /W4 /utf-8 /DUNICODE /D_UNICODE /Iinclude /Foobj\image\ /Fdobj\image\image_tool.pdb src\image_tool_main.cpp src\bgi_cbg_codec.cpp src\png_io_gdiplus.cpp obj\app_icon.res /Febin\BGI_Hazuki_ImageTool.exe /link gdiplus.lib
if errorlevel 1 (
    echo 编译失败。
    exit /b 1
)

cl /nologo /std:c++17 /EHsc /W4 /utf-8 /DUNICODE /D_UNICODE /Iinclude /Foobj\text\ /Fdobj\text\text_tool.pdb src\text_tool_main.cpp src\dsc_text_tool.cpp obj\app_icon.res /Febin\BGI_Hazuki_TextTool.exe
if errorlevel 1 (
    echo 编译失败。
    exit /b 1
)

cl /nologo /std:c++17 /EHsc /W4 /utf-8 /DUNICODE /D_UNICODE /Iinclude /Foobj\asset\ /Fdobj\asset\asset_tool.pdb src\asset_tool_main.cpp src\asset_probe.cpp src\bgi_cbg_codec.cpp src\png_io_gdiplus.cpp src\dsc_text_tool.cpp obj\app_icon.res /Febin\BGI_Hazuki_AssetTool.exe /link gdiplus.lib
if errorlevel 1 (
    echo 编译失败。
    exit /b 1
)

cl /nologo /std:c++17 /EHsc /W4 /utf-8 /DUNICODE /D_UNICODE /DHAZUKI_CORE_BUILD /LD /Iinclude /Foobj\core\ /Fdobj\core\core.pdb src\unpack_pipeline.cpp src\bgi_arc.cpp src\asset_probe.cpp src\bgi_cbg_codec.cpp src\png_io_gdiplus.cpp src\dsc_text_tool.cpp /Febin\BGI_Hazuki_Core.dll /link /IMPLIB:obj\core\BGI_Hazuki_Core.lib gdiplus.lib
if errorlevel 1 (
    echo 编译失败。
    exit /b 1
)

cl /nologo /std:c++17 /EHsc /W4 /utf-8 /DUNICODE /D_UNICODE /DHAZUKI_CORE_USE /Iinclude /Foobj\gui\ /Fdobj\gui\gui_tool.pdb src\gui_main.cpp obj\app_icon.res /Febin\BGI_Hazuki_GUI.exe /link obj\core\BGI_Hazuki_Core.lib gdiplus.lib comctl32.lib comdlg32.lib shell32.lib user32.lib gdi32.lib /SUBSYSTEM:WINDOWS
if errorlevel 1 (
    echo 编译失败。
    exit /b 1
)

set "PYINSTALLER=%~dp0..\.venv\Scripts\pyinstaller.exe"
if not exist "%PYINSTALLER%" (
    set "PYINSTALLER="
    for %%i in (pyinstaller.exe) do set "PYINSTALLER=%%~$PATH:i"
)

if "%PYINSTALLER%"=="" (
    echo 未找到 PyInstaller，无法编译 BGI_Unpacker.exe。
    exit /b 1
)

pushd BGI_Unpacker
"%PYINSTALLER%" --noconfirm BGI_Unpacker.spec
if errorlevel 1 (
    popd
    echo 编译失败。
    exit /b 1
)
popd

del /q *.obj 2>nul
del /q *.pdb 2>nul
del /q *.ilk 2>nul

echo [BGI_Hazuki] 编译完成: bin\BGI_Hazuki_ImageTool.exe
echo [BGI_Hazuki] 编译完成: bin\BGI_Hazuki_TextTool.exe
echo [BGI_Hazuki] 编译完成: bin\BGI_Hazuki_AssetTool.exe
echo [BGI_Hazuki] 编译完成: bin\BGI_Hazuki_Core.dll
echo [BGI_Hazuki] 编译完成: bin\BGI_Hazuki_GUI.exe
echo [BGI_Hazuki] 编译完成: BGI_Unpacker\dist\BGI_Unpacker.exe
exit /b 0
