# TtmlCaption.cpp のテストをビルドして実行する。Visual Studio の C++ ツールが必要
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$output = Join-Path $repo 'test\build'
New-Item -ItemType Directory -Force $output | Out-Null
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ tools are required' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$dantto4k = 'src\dantto4k\src'
$sources = @('test\TtmlCaptionTest.cpp', 'src\TtmlCaption.cpp',
    "$dantto4k\pugixml.cpp", "$dantto4k\ttml\parser.cpp", "$dantto4k\ttml\resolver.cpp", "$dantto4k\ttml\style.cpp")
$batch = Join-Path $output 'run.cmd'
@"
@echo off
set "PATH=$(Split-Path $vswhere);%PATH%"
call "$vcvars" >nul
if errorlevel 1 exit /b 1
cd /d "$repo"
cl /nologo /EHsc /MD /O2 /std:c++20 /utf-8 /DNOMINMAX /I"src" /I"src\aribcaption\include" /I"$dantto4k" $($sources -join ' ') /Fo:"$output\\" /Fe:"$output\ttml_caption_test.exe"
if errorlevel 1 exit /b 1
"$output\ttml_caption_test.exe"
"@ | Set-Content -LiteralPath $batch -Encoding ascii
& $batch
if ($LASTEXITCODE -ne 0) { throw 'TTML caption tests failed' }
