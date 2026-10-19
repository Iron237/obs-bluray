param([string]$SdkRoot=(Join-Path $env:LOCALAPPDATA 'StreamDeskSDK'),[string]$ObsRoot=$env:OBS_ROOT)
$ErrorActionPreference='Stop'
if(-not $ObsRoot){$ObsRoot=Join-Path $env:ProgramFiles 'obs-studio'}
New-Item -ItemType Directory -Force "$SdkRoot/downloads" | Out-Null
$taskVsWhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$taskVs=& $taskVsWhere -latest -products '*' -property installationPath
$taskEnv=cmd.exe /d /s /c "`"${taskVs}\Common7\Tools\VsDevCmd.bat`" -no_logo -arch=x64 -host_arch=x64 && set"
foreach($taskLine in $taskEnv){if($taskLine -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
$taskSdk=Join-Path $SdkRoot 'obs-sdk'
$taskVersion=(Get-Item -LiteralPath "$ObsRoot\bin\64bit\obs64.exe").VersionInfo.ProductVersion
$taskHeaders=Join-Path $taskSdk "obs-studio-$taskVersion\libobs"
if(-not(Test-Path -LiteralPath $taskHeaders)){
    $taskArchive=Join-Path $SdkRoot "downloads\obs-$taskVersion-source.zip"
    Invoke-WebRequest -Uri "https://github.com/obsproject/obs-studio/archive/refs/tags/$taskVersion.zip" -OutFile $taskArchive
    Expand-Archive -LiteralPath $taskArchive -DestinationPath $taskSdk -Force
}
"#pragma once`n#define OBS_RELEASE_CANDIDATE 0`n#define OBS_BETA 0`n#define OBS_INSTALL_PREFIX `"`"`n#define OBS_DATA_PATH `"data`"" | Set-Content -LiteralPath "$taskHeaders\obsconfig.h" -Encoding ascii
$taskExports=& dumpbin.exe /nologo /exports "$ObsRoot\bin\64bit\obs.dll"
$taskNames=$taskExports | ForEach-Object {if($_ -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\S+)'){$matches[1]}}
@('LIBRARY obs.dll','EXPORTS')+$taskNames | Set-Content -LiteralPath "$taskSdk\obs.def" -Encoding ascii
& lib.exe /nologo /def:"$taskSdk\obs.def" /out:"$taskSdk\obs.lib" /machine:x64
if($LASTEXITCODE){throw 'Cannot build OBS import library'}
Write-Output "OBS SDK ready: $taskVersion"
