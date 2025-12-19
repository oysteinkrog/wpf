@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64

echo Building hw.vcxproj...
cd /d "C:\work\wpf\src\Microsoft.DotNet.Wpf\src\WpfGfx\core\hw"
msbuild hw.vcxproj /p:Configuration=Debug /p:Platform=x64 /t:Build /v:normal
