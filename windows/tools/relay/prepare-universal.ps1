param(
    [Parameter(Mandatory)][string]$Release,
    [Parameter(Mandatory)][string]$Bridge,
    [string]$HostProgram,
    [string]$ControllerProxy
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'pairing.ps1')
$releaseRoot = (Resolve-Path -LiteralPath $Release).Path
if (!$HostProgram) { $HostProgram = Join-Path $releaseRoot 'host/WindowsStreamHost.exe' }
$hostProgram = (Resolve-Path -LiteralPath $HostProgram).Path
$bridgeProgram = (Resolve-Path -LiteralPath $Bridge).Path
$root = Join-Path $releaseRoot 'relay/universal'
$hostFolder = Join-Path $root 'host'
$server = Join-Path $root 'server'
$controller = Join-Path $releaseRoot 'controller'
foreach ($folder in @($hostFolder, $server)) { $null = New-Item -ItemType Directory -Path $folder -Force }

$registration = Join-Path $hostFolder 'enrollment.ini'
if (!(Test-Path -LiteralPath $registration)) {
    Write-RelayIni $registration @"
[enrollment]
key=$(New-RelayKey)
"@
}
$key = (Read-RelaySection $registration 'enrollment').key
if ($key -cnotmatch '^[0-9a-f]{64}$') { throw '内网中转登记配置无效。' }
$hash = ConvertTo-RelayHex (Get-RelayHash ([Text.Encoding]::ASCII.GetBytes($key)))
Write-RelayIni (Join-Path $server 'windowsstream-enrollment.ini') @"
[enrollment]
sha256=$hash
"@

function Copy-Launcher([string]$Name, [string]$Folder) {
    $text = [IO.File]::ReadAllText((Join-Path $PSScriptRoot $Name))
    [IO.File]::WriteAllText((Join-Path $Folder $Name), $text, [Text.UTF8Encoding]::new($true))
}

Copy-Item -LiteralPath $hostProgram -Destination $hostFolder -Force
Copy-Item -LiteralPath $bridgeProgram -Destination (Join-Path $hostFolder 'WindowsStreamBridge.exe') -Force
Copy-Item -LiteralPath $bridgeProgram -Destination (Join-Path $controller 'WindowsStreamBridge.exe') -Force

foreach ($launcher in @('takeover.ps1','pairing.ps1')) {
    Copy-Launcher $launcher $hostFolder
    Copy-Launcher $launcher $controller
}
Copy-Launcher 'start-host.ps1' $hostFolder
Copy-Launcher 'connect-universal.ps1' $controller
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Start-WindowsStream.cmd'), (Join-Path $PSScriptRoot 'host.ini.template') -Destination $hostFolder -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Connect-WindowsStream.cmd') -Destination $controller -Force
$network = Join-Path $controller 'relay-network.ini'
if (!(Test-Path -LiteralPath $network)) {
    Write-RelayIni $network @"
[relay]
http_proxy=$ControllerProxy
"@
}

$package = @('enrollment.ini','WindowsStreamHost.exe','WindowsStreamBridge.exe','host.ini.template','Start-WindowsStream.cmd','start-host.ps1','pairing.ps1','takeover.ps1')
$files = foreach ($name in $package) { Join-Path $hostFolder $name }
Compress-Archive -LiteralPath $files -DestinationPath (Join-Path $root 'WindowsStream-Universal-Host.zip') -Force
Write-Output '已生成通用被控端包和控制端配对入口。'
