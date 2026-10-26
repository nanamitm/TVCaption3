# 本体と同じオブジェクトをリンクし、受信・表示切り替え経路の回帰テストを実行する。
param([string]$PlatformToolset = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$output = Join-Path $PSScriptRoot 'build\pipeline'
$objects = Join-Path $output 'obj'
New-Item -ItemType Directory -Force $objects | Out-Null
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ tools are required' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$toolsetOption = if ($PlatformToolset) { "/p:PlatformToolset=$PlatformToolset" } else { '' }
$batch = Join-Path $output 'run.cmd'
@"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b 1
cd /d "$repo"
msbuild src\TVCaption3.sln /m /nologo /verbosity:minimal /p:Configuration=Release /p:Platform=x64 /p:IntDir="$objects\\" /p:OutDir="$output\\" $toolsetOption
if errorlevel 1 exit /b 1
cl /nologo /c /EHsc /MD /O2 /std:c++20 /utf-8 /DUNICODE /D_UNICODE /Isrc /Isrc\aribcaption\include test\CaptionPipelineTest.cpp /Fo:"$output\CaptionPipelineTest.obj"
if errorlevel 1 exit /b 1
link /nologo /LTCG /SUBSYSTEM:CONSOLE /OUT:"$output\caption_pipeline_test.exe" "$output\CaptionPipelineTest.obj" "$objects\*.obj" /LIBPATH:"src\freetype\release dll\win64" /DELAYLOAD:freetype.dll freetype.lib delayimp.lib d2d1.lib dwrite.lib winmm.lib user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib comdlg32.lib advapi32.lib
if errorlevel 1 exit /b 1
"$output\caption_pipeline_test.exe"
"@ | Set-Content -LiteralPath $batch -Encoding ascii
& $batch
if ($LASTEXITCODE -ne 0) { throw 'Caption pipeline tests failed' }
