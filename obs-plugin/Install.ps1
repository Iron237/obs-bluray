param([string]$ObsRoot='')
$ErrorActionPreference='Stop'
if(Get-Process obs64 -ErrorAction SilentlyContinue){throw '请先关闭 OBS，然后重新运行安装。'}
if(-not $ObsRoot){
    $taskCandidates=@($env:OBS_ROOT,"${env:ProgramFiles}\obs-studio")
    $ObsRoot=$taskCandidates | Where-Object {Test-Path -LiteralPath "$_\bin\64bit\obs64.exe"} | Select-Object -First 1
}
if(-not $ObsRoot -or -not(Test-Path -LiteralPath "$ObsRoot\bin\64bit\obs64.exe")){throw '找不到 OBS。请运行 Install.ps1 -ObsRoot 您的OBS目录'}
$taskVersion=(Get-Item -LiteralPath "$ObsRoot\bin\64bit\obs64.exe").VersionInfo.ProductVersion
if([version]$taskVersion -lt [version]'32.2.2'){throw '此包需要 OBS 32.2.2 或更新版本。'}
$taskBinary=Join-Path $ObsRoot 'obs-plugins\64bit\obs-bluray.dll'
$taskData=Join-Path $ObsRoot 'data\obs-plugins\obs-bluray'
New-Item -ItemType Directory -Force (Split-Path -Parent $taskBinary),$taskData | Out-Null
Copy-Item -LiteralPath "$PSScriptRoot\obs-bluray\bin\64bit\obs-bluray.dll" -Destination $taskBinary -Force
Get-ChildItem -LiteralPath "$PSScriptRoot\obs-bluray\data" | Copy-Item -Destination $taskData -Recurse -Force
Write-Output "已安装到 $ObsRoot。启动 OBS，在来源中添加：蓝光原盘 / BDMV (StreamDesk)。"
