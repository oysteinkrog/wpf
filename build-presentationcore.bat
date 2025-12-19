@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul

echo Building PresentationCore via build.cmd...
cd /d "C:\work\wpf"
build.cmd -plat x64 -c Debug -project PresentationCore
