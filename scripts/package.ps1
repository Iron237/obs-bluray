param([string]$SdkRoot=(Join-Path $env:LOCALAPPDATA 'StreamDeskSDK'))
$ErrorActionPreference='Stop'
$taskProject=Split-Path -Parent $PSScriptRoot
$taskBin=Join-Path $taskProject 'build\bin'
$taskPackage=Join-Path $taskProject 'dist\OBS-BluRay-0.1.1'
$taskPlugin=Join-Path $taskPackage 'obs-bluray'
$taskData=Join-Path $taskPlugin 'data'
$taskWorker=Join-Path $taskData 'worker'
New-Item -ItemType Directory -Force "$taskPlugin\bin\64bit",$taskWorker | Out-Null
Copy-Item -LiteralPath "$taskBin\obs-bluray.dll" -Destination "$taskPlugin\bin\64bit" -Force
Copy-Item -LiteralPath "$taskBin\obs-bluray-worker.exe" -Destination $taskWorker -Force
Get-ChildItem -LiteralPath $taskBin -File -Filter '*.dll' | Where-Object { $_.Name -notlike 'Qt6*' -or $_.Name -in 'Qt6Core.dll','Qt6Network.dll','Qt6Concurrent.dll' } | Where-Object Name -ne 'obs-bluray.dll' | Copy-Item -Destination $taskWorker -Force
foreach($taskDir in @('bin','lib\gstreamer-1.0','libexec\gstreamer-1.0')) {
    $taskSource=Join-Path "$SdkRoot\gstreamer" $taskDir
    $taskDest=Join-Path $taskWorker "runtime\gstreamer\$taskDir"
    New-Item -ItemType Directory -Force $taskDest | Out-Null
    Get-ChildItem -LiteralPath $taskSource -File | Where-Object {($taskDir -eq 'bin' -and $_.Extension -eq '.exe') -or ($taskDir -ne 'bin' -and $_.Extension -in '.dll','.exe')} | Copy-Item -Destination $taskDest -Force
}
Copy-Item -LiteralPath "$taskProject\obs-plugin\Install.ps1","$taskProject\obs-plugin\Install.cmd","$taskProject\README.md","$taskProject\LICENSE","$taskProject\VALIDATION.md" -Destination $taskPackage -Force
$taskLicense=Join-Path $taskPackage 'licenses'
New-Item -ItemType Directory -Force $taskLicense | Out-Null
Copy-Item -LiteralPath "$SdkRoot\gstreamer\share\licenses" -Destination "$taskLicense\gstreamer" -Recurse -Force
Copy-Item -LiteralPath "$SdkRoot\libbluray-1.4.0\COPYING" -Destination "$taskLicense\libbluray-COPYING" -Force
Copy-Item -LiteralPath "$SdkRoot\obs-sdk\obs-studio-32.2.2\COPYING" -Destination "$taskLicense\OBS-COPYING" -Force
Copy-Item -LiteralPath "$taskProject\licenses\qt" -Destination "$taskLicense\qt" -Recurse -Force
Copy-Item -LiteralPath "$taskProject\THIRD_PARTY.md" -Destination $taskPackage -Force
$taskManifest=Get-ChildItem -LiteralPath $taskPackage -File -Recurse | Where-Object Extension -in '.dll','.exe' | ForEach-Object {@{path=[IO.Path]::GetRelativePath($taskPackage,$_.FullName);sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}}
$taskManifest | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath "$taskPackage\manifest-sha256.json" -Encoding utf8
@{name='obs-bluray';version='0.1.1';platform='windows-x64';testedObsVersion='32.2.2';sourceVideoPolicy='async-unbuffered';associatedAssPolicy='async-unbuffered';binaryManifest='manifest-sha256.json'} | ConvertTo-Json | Set-Content -LiteralPath "$taskPackage\release.json" -Encoding utf8
Write-Output "Packaged $taskPackage"
