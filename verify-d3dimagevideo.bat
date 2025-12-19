@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul

echo Syntax checking D3DImageVideo.cs...
cd /d "C:\work\wpf\src\Microsoft.DotNet.Wpf\src\PresentationCore\System\Windows\InterOp"

REM Just parse without full compilation to verify syntax
csc /nologo /target:module /noconfig /nostdlib /nowarn:CS0518,CS0162,CS0626,CS0649,CS8019,CS0114,CS0436 /langversion:preview D3DImageVideo.cs
