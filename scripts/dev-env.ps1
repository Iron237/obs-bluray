$ErrorActionPreference='Stop'
$taskVsWhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$taskVs=& $taskVsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(-not $taskVs){throw 'Install Visual Studio 2022 C++ desktop tools.'}
$taskEnvironment=cmd.exe /d /s /c "`"${taskVs}\Common7\Tools\VsDevCmd.bat`" -no_logo -arch=x64 -host_arch=x64 && set"
foreach($taskLine in $taskEnvironment){if($taskLine -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
$taskCmake=Join-Path $taskVs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$taskNinja=Join-Path $taskVs 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja'
$env:PATH="$taskNinja;"+$env:PATH
$env:VSLANG='1033'
