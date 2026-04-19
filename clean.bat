@echo off
setlocal
chcp 65001 >nul

echo [BGI_Hazuki] 清理中间产物与测试输出...

if exist obj rmdir /s /q obj
if exist unpack rmdir /s /q unpack
if exist BGI_Unpacker\build rmdir /s /q BGI_Unpacker\build
if exist static\icon.ico del /q static\icon.ico
del /q *.obj 2>nul
del /q *.pdb 2>nul
del /q *.ilk 2>nul

echo [BGI_Hazuki] 已清理 obj、unpack、BGI_Unpacker\build、static\icon.ico 以及根目录遗留中间文件。
echo [BGI_Hazuki] Final exes kept in bin and BGI_Unpacker\dist.
exit /b 0