# 本体と同じオブジェクトをリンクし、受信・表示切り替え経路の回帰テストを実行する。
# ObjectDirに本体のビルドが置いたオブジェクトの場所を渡すと、ビルドし直さない。
param([string]$PlatformToolset = '', [string]$ObjectDir = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$output = Join-Path $PSScriptRoot 'build\pipeline'
New-Item -ItemType Directory -Force $output | Out-Null
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ tools are required' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$buildStep = ''
if ($ObjectDir) {
    # 本体のビルドが置いたオブジェクトをそのまま使い、同じ構成を二度ビルドしない
    $objects = (Resolve-Path -LiteralPath $ObjectDir).Path.TrimEnd('\')
    if (-not (Get-ChildItem -LiteralPath $objects -Filter *.obj)) {
        throw "No object files in $objects. Build Release|x64 first"
    }
}
else {
    $objects = Join-Path $output 'obj'
    New-Item -ItemType Directory -Force $objects | Out-Null
    if (-not $PlatformToolset) {
        # vcxprojはVS2017/2019のときしかPlatformToolsetを決めないので、指定が無ければ入って
        # いるうち新しいものを選ぶ。無指定のままだとMSBuildがv100を選んでMSB8020で止まる
        $PlatformToolset = Get-ChildItem (Join-Path $vs 'MSBuild\Microsoft\VC') -Directory -ErrorAction SilentlyContinue |
            ForEach-Object { Get-ChildItem (Join-Path $_.FullName 'Platforms\x64\PlatformToolsets') -Directory -ErrorAction SilentlyContinue } |
            Where-Object { $_.Name -match '^v\d{3}$' } | Select-Object -ExpandProperty Name |
            Sort-Object -Descending | Select-Object -First 1
        if (-not $PlatformToolset) { throw 'Visual Studio C++ platform toolset not found' }
    }
    $buildStep = @"
msbuild src\TVCaption3.sln /m /nologo /verbosity:minimal /p:Configuration=Release /p:Platform=x64 /p:IntDir="$objects\\" /p:OutDir="$output\\" /p:PlatformToolset=$PlatformToolset
if errorlevel 1 exit /b 1
"@
}
$batch = Join-Path $output 'run.cmd'
@"
@echo off
set "PATH=$(Split-Path $vswhere);%PATH%"
call "$vcvars" >nul
if errorlevel 1 exit /b 1
cd /d "$repo"
$buildStep
cl /nologo /c /EHsc /MD /O2 /std:c++20 /utf-8 /DUNICODE /D_UNICODE /Isrc /Isrc\aribcaption\include test\CaptionPipelineTest.cpp /Fo:"$output\CaptionPipelineTest.obj"
if errorlevel 1 exit /b 1
link /nologo /LTCG /SUBSYSTEM:CONSOLE /OUT:"$output\caption_pipeline_test.exe" "$output\CaptionPipelineTest.obj" "$objects\*.obj" /LIBPATH:"src\freetype\release dll\win64" /DELAYLOAD:freetype.dll freetype.lib delayimp.lib d2d1.lib dwrite.lib winmm.lib user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib comdlg32.lib advapi32.lib
if errorlevel 1 exit /b 1
"$output\caption_pipeline_test.exe"
"@ | Set-Content -LiteralPath $batch -Encoding ascii
& $batch
if ($LASTEXITCODE -ne 0) { throw 'Caption pipeline tests failed' }
