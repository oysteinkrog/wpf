@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul
dumpbin /exports C:\WORK\wpf\artifacts\bin\wpfgfx\x64\Debug\wpfgfx_cor3.dll | findstr /i "InteropDeviceBitmap11"
