@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64
echo VS2022 environment loaded
cl.exe 2>&1 | findstr /C:"Version"
cd /d C:\work\wpf
build.cmd -plat x64 -c Debug
