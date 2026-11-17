param(
    [Parameter(Mandatory)][string]$Release,
    [string]$Bridge,
    [string]$HostProgram,
    [string]$ControllerProxy
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'pairing.ps1')
$releaseRoot = (Resolve-Path -LiteralPath $Release).Path
$hostOutput = Join-Path $releaseRoot 'host'
if (!$HostProgram) { $HostProgram = Join-Path $hostOutput '.runtime/WindowsStreamHost.exe' }
$hostProgram = (Resolve-Path -LiteralPath $HostProgram).Path
$root = Join-Path (Split-Path -Parent $releaseRoot) 'obj/relay/universal'
$hostFolder = Join-Path $root 'host'
$server = Join-Path $root 'server'
$controller = Join-Path $releaseRoot 'controller'
$controllerRuntime = Join-Path $controller '.runtime'
foreach ($folder in @($hostFolder, $server, $controllerRuntime, $hostOutput)) {
    $null = New-Item -ItemType Directory -Path $folder -Force
}

$goRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../../relay')).Path
$go = (Read-RelaySection (Join-Path $goRoot '../config/build.ini') 'tools').go
if (!(Test-Path -LiteralPath $go -PathType Leaf)) {
    throw '请在 windows\config\build.ini 中配置 Go 可执行文件。'
}
if (!$Bridge) {
    $Bridge = Join-Path $root 'WindowsStreamBridge.exe'
    & (Join-Path $PSScriptRoot 'build-bridge.ps1') -Output $Bridge
}
$bridgeProgram = (Resolve-Path -LiteralPath $Bridge).Path

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
$scripts = @('takeover.ps1','pairing.ps1','start-host.ps1','install-configured.ps1')
foreach ($launcher in $scripts) {
    Copy-Launcher $launcher $hostFolder
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'host.ini.template') -Destination $hostFolder -Force
$network = Join-Path $controllerRuntime 'relay-network.ini'
if (!(Test-Path -LiteralPath $network)) {
    Write-RelayIni $network @"
[relay]
http_proxy=$ControllerProxy
"@
}

$package = @('enrollment.ini','WindowsStreamHost.exe','WindowsStreamBridge.exe','host.ini.template') + $scripts
$files = foreach ($name in $package) { Join-Path $hostFolder $name }
$payload = Join-Path $root 'host-payload.zip'
Compress-Archive -LiteralPath $files -DestinationPath $payload -Force

$overlay = Join-Path $root 'host-package-overlay.json'
$replacement = @{}
$replacement[(Join-Path $goRoot 'cmd/host-package/payload.zip')] = $payload
[IO.File]::WriteAllText($overlay, (@{Replace=$replacement} | ConvertTo-Json -Compress), [Text.UTF8Encoding]::new($false))
$standalone = Join-Path $hostOutput 'WindowsStream-Host.exe'
Push-Location -LiteralPath $goRoot
try {
    & $go build -buildvcs=false -trimpath -mod=readonly -tags hostpackage -ldflags '-H=windowsgui' -overlay $overlay -o $standalone ./cmd/host-package
    if ($LASTEXITCODE -ne 0) { throw '被控端启动程序构建失败。' }
} finally { Pop-Location }
foreach ($folder in @($controllerRuntime, (Join-Path $hostOutput '.runtime'))) {
    $runtime = Get-Item -LiteralPath $folder -Force -ErrorAction SilentlyContinue
    if ($runtime) { $runtime.Attributes = $runtime.Attributes -bor [IO.FileAttributes]::Hidden }
}
Write-Output '已生成统一版被控端。'
