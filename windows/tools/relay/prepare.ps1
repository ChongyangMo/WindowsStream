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
$folder = Join-Path (Split-Path -Parent $releaseRoot) "obj/relay/$key"
$server = Join-Path $folder 'server'
$viewer = Join-Path $folder 'controller'
$hostFolder = Join-Path $folder 'host'
$bridge = Join-Path $viewer 'WindowsStreamBridge.exe'
$hostProgram = Join-Path $releaseRoot 'host/WindowsStream-Host.exe'
$hostConfig = @((Join-Path $hostFolder 'host.ini'), (Join-Path $hostFolder 'relay.ini'))
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

function Export-ConfiguredHost {
    $archive = Join-Path $folder "WindowsStream-$name-host.zip"
    $pending = Join-Path $folder ('.windowsstream-' + [Guid]::NewGuid().ToString('N') + '.zip')
    try {
        Compress-Archive -LiteralPath (@($hostProgram) + $hostConfig) -DestinationPath $pending
        if (Test-Path -LiteralPath $archive -PathType Leaf) {
            [IO.File]::Replace($pending, $archive, [NullString]::Value)
        } else {
            [IO.File]::Move($pending, $archive)
        }
    } finally {
        if (Test-Path -LiteralPath $pending -PathType Leaf) { [IO.File]::Delete($pending) }
    }
}

if (!(Test-Path -LiteralPath $hostProgram -PathType Leaf)) { throw ('请先构建：' + $hostProgram) }
$existing = @($hostConfig | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf })
if ($existing.Count -eq $hostConfig.Count) {
    Export-ConfiguredHost
    Write-Output ('已沿用原配对并更新被控端包：' + $name)
    return
}
$configuration = $hostConfig + @($serverConfig, (Join-Path $viewer 'relay.ini'), (Join-Path $viewer 'client.ini'))
if ($configuration | Where-Object { Test-Path -LiteralPath $_ }) {
    throw '已有设备的被控端配置不完整，请补齐后再打包。'
}
if (!(Test-Path -LiteralPath $bridge -PathType Leaf)) { throw ('请先构建：' + $bridge) }
foreach ($dir in @($server, $hostFolder)) { [void](New-Item -ItemType Directory -Path $dir -Force) }

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
relay_autostart=1
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
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Open-WindowsStream.cmd') -Destination $viewer
$launcher = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'open-controller.ps1'), $utf8)
[IO.File]::WriteAllText((Join-Path $viewer 'open-controller.ps1'), $launcher, [Text.UTF8Encoding]::new($true))
$launcher = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'takeover.ps1'), $utf8)
[IO.File]::WriteAllText((Join-Path $viewer 'takeover.ps1'), $launcher, [Text.UTF8Encoding]::new($true))
Export-ConfiguredHost
Write-Output ('已生成 WindowsStream 中转被控端包和控制端配置：' + $name)
