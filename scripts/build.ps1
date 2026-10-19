param([string]$SdkRoot=(Join-Path $env:LOCALAPPDATA 'StreamDeskSDK'),[string]$ObsRoot=$env:OBS_ROOT,[switch]$Deploy)
. "$PSScriptRoot/dev-env.ps1"
$taskProject=Split-Path -Parent $PSScriptRoot
$taskQt=Join-Path $SdkRoot 'Qt/6.8.3/msvc2022_64'
$env:PATH="$taskQt/bin;$SdkRoot/gstreamer/bin;"+$env:PATH
if(-not $ObsRoot){$ObsRoot=Join-Path $env:ProgramFiles 'obs-studio'}
$taskObsVersion=(Get-Item "$ObsRoot/bin/64bit/obs64.exe").VersionInfo.ProductVersion
if(-not(Test-Path "$SdkRoot/obs-sdk/obs.lib")){& "$PSScriptRoot/setup-obs-sdk.ps1" -SdkRoot $SdkRoot -ObsRoot $ObsRoot}
& $taskCmake -S $taskProject -B "$taskProject/build" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$taskQt" "-DSDK_ROOT=$SdkRoot" "-DOBS_SDK_ROOT=$SdkRoot/obs-sdk/obs-studio-$taskObsVersion/libobs"
if($LASTEXITCODE){throw 'CMake configure failed'}
& $taskCmake --build "$taskProject/build" -j 6
if($LASTEXITCODE){throw 'Build failed'}
if($Deploy){
    $taskBin=Join-Path $taskProject 'build/bin'
    Copy-Item "$SdkRoot/libbluray-build/src/bluray-3.dll" $taskBin -Force
    Copy-Item -LiteralPath "$SdkRoot/mediainfo/26.10/MediaInfo.dll" -Destination $taskBin -Force
    Get-ChildItem "$SdkRoot/gstreamer/bin/*.dll" | Copy-Item -Destination $taskBin -Force
    foreach($taskDll in @('Qt6Core.dll','Qt6Network.dll','Qt6Concurrent.dll','Qt6Test.dll','Qt6Gui.dll','Qt6Widgets.dll','Qt6Svg.dll')){Copy-Item "$taskQt/bin/$taskDll" $taskBin -Force}
    New-Item -ItemType Directory -Force "$taskBin/platforms" | Out-Null
    foreach($taskPlatform in @('qwindows.dll','qoffscreen.dll')){Copy-Item -LiteralPath "$taskQt/plugins/platforms/$taskPlatform" -Destination "$taskBin/platforms" -Force}
    New-Item -ItemType Directory -Force "$taskBin/iconengines" | Out-Null
    Copy-Item -LiteralPath "$taskQt/plugins/iconengines/qsvgicon.dll" -Destination "$taskBin/iconengines" -Force
}
