param (
    [Parameter(Mandatory)][string]$DeviceAddress,
    [Parameter(Mandatory)][string]$Release,
    [string]$ControllerProxy
)

$ErrorActionPreference = 'Stop'
$address = [Net.IPAddress]::Parse($DeviceAddress)
if ($address.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork) { throw '请输入被控端的 IPv4 地址。' }
$key = $address.ToString().Replace('.', '-')
$name = $address.ToString()
$releaseRoot = [IO.Path]::GetFullPath($Release)
$folder = Join-Path $releaseRoot "relay/$key"
$server = Join-Path $folder 'server'
$viewer = Join-Path $folder 'controller'
$hostFolder = Join-Path $folder 'host'
$bridge = Join-Path $viewer 'WindowsStreamBridge.exe'
$hostProgram = Join-Path $releaseRoot 'host/WindowsStreamHost.exe'
$serverConfig = Join-Path $server 'windowsstream-relay.ini'
$endpoint = 'wss://sdserver.luxvisions-inno.com/api/windowsstream/relay'
$target = '10.18.32.67:443'

function New-Secret {
    $random = [Security.Cryptography.RandomNumberGenerator]::Create()
    try { $bytes = [byte[]]::new(32); $random.GetBytes($bytes) }
    finally { $random.Dispose() }
    return [BitConverter]::ToString($bytes).Replace('-', '').ToLowerInvariant()
}

function Get-SecretHash([string]$Secret) {
    $hash = [Security.Cryptography.SHA256]::Create()
    try { $bytes = $hash.ComputeHash([Text.Encoding]::ASCII.GetBytes($Secret)) }
    finally { $hash.Dispose() }
    return [BitConverter]::ToString($bytes).Replace('-', '').ToLowerInvariant()
}

function Write-Ini([string]$Path, [string]$Content, [Text.Encoding]$Encoding) {
    if (Test-Path -LiteralPath $Path) { throw ('中转配置已存在：' + $Path) }
    [IO.File]::WriteAllText($Path, $Content + [Environment]::NewLine, $Encoding)
}

foreach ($file in @($bridge, $hostProgram)) {
    if (!(Test-Path -LiteralPath $file -PathType Leaf)) { throw ('请先构建：' + $file) }
}
foreach ($dir in @($server, $hostFolder)) { [void](New-Item -ItemType Directory -Path $dir -Force) }
foreach ($file in @($serverConfig, (Join-Path $hostFolder 'relay.ini'), (Join-Path $hostFolder 'host.ini'),
        (Join-Path $viewer 'relay.ini'), (Join-Path $viewer 'client.ini'))) {
    if (Test-Path -LiteralPath $file) { throw ('中转配置已存在：' + $file) }
}

$hostKey = New-Secret
$viewerKey = New-Secret
$streamKey = New-Secret
$utf8 = [Text.UTF8Encoding]::new($false)
Write-Ini $serverConfig @"
[$key]
host_sha256=$(Get-SecretHash $hostKey)
viewer_sha256=$(Get-SecretHash $viewerKey)
"@ $utf8
Write-Ini (Join-Path $hostFolder 'relay.ini') @"
[relay]
role=host
device=$key
endpoint=$endpoint
server_address=$target
local_address=127.0.0.1:17413
token=$hostKey
"@ $utf8
Write-Ini (Join-Path $viewer 'relay.ini') @"
[relay]
role=viewer
device=$key
endpoint=$endpoint
server_address=$target
http_proxy=$ControllerProxy
local_address=127.0.0.1:17414
token=$viewerKey
"@ $utf8

Write-Ini (Join-Path $hostFolder 'host.ini') @"
[connection]
mode=listen
bind_host=127.0.0.1
port=17413
token=$streamKey

[video]
resolution=limited
max_width=1920
max_height=1080
fps=30
bitrate_mbps=12

[permissions]
clipboard=0

[updates]
enabled=0

[background]
autostart=0
manage_autostart=0
tray=1
start_hidden=0
keep_awake=0
watchdog=0
idle_pulse=0

[proxy]
enabled=0
"@ $utf8
Write-Ini (Join-Path $viewer 'client.ini') @"
[connection]
name=$name（中转）
instance=relay-$key
host=127.0.0.1
port=17414
token=$streamKey

[proxy]
enabled=0

[updates]
enabled=0
"@ ([Text.Encoding]::Unicode)
Copy-Item -LiteralPath $hostProgram, $bridge, (Join-Path $PSScriptRoot 'Start-WindowsStream.cmd') -Destination $hostFolder
$launcher = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'start-host.ps1'), $utf8)
[IO.File]::WriteAllText((Join-Path $hostFolder 'start-host.ps1'), $launcher, [Text.UTF8Encoding]::new($true))
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Open-WindowsStream.cmd') -Destination $viewer
$launcher = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'open-controller.ps1'), $utf8)
[IO.File]::WriteAllText((Join-Path $viewer 'open-controller.ps1'), $launcher, [Text.UTF8Encoding]::new($true))
Compress-Archive -Path (Join-Path $hostFolder '*') -DestinationPath (Join-Path $folder "WindowsStream-$name-host.zip") -Force
Write-Output ('已生成 WindowsStream 中转被控端包和控制端配置：' + $name)
