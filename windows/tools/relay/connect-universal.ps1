param([string]$PairFile)

$ErrorActionPreference = 'Stop'
try {
    . (Join-Path $PSScriptRoot 'takeover.ps1')
    . (Join-Path $PSScriptRoot 'pairing.ps1')
    if (!$PairFile) {
        Add-Type -AssemblyName System.Windows.Forms
        $picker = [Windows.Forms.OpenFileDialog]::new()
        try {
            $picker.Title = '选择 WindowsStream 配对文件'
            $picker.Filter = 'WindowsStream 配对文件 (*.ini)|*.ini'
            if ($picker.ShowDialog() -ne [Windows.Forms.DialogResult]::OK) { exit 0 }
            $PairFile = $picker.FileName
        } finally { $picker.Dispose() }
    }
    $controllerFolder = Split-Path -Parent $PSScriptRoot
    $controller = Join-Path $controllerFolder 'WindowsStream.exe'
    $source = Join-Path $PSScriptRoot 'WindowsStreamBridge.exe'
    $networkFile = Join-Path $PSScriptRoot 'relay-network.ini'
    Assert-StreamFiles @($controller, $source, $PairFile, $networkFile)
    $pair = Read-RelaySection $PairFile 'pairing'
    foreach ($field in @('device', 'viewer_key', 'stream_key')) {
        if ($pair[$field] -cnotmatch '^[a-f0-9]{64}$') { throw 'WindowsStream 配对文件无效。' }
    }
    if ($pair.name -cnotmatch '^[a-zA-Z0-9._-]{1,63}$') { throw '配对文件的电脑名称无效。' }
    $network = Read-RelaySection $networkFile 'relay'

    Invoke-StreamLock 'Local\WindowsStreamControllerLaunch' {
        $store = Join-Path $controllerFolder 'relay-devices'
        $folder = Join-Path $store $pair.device
        $profile = Join-Path $folder 'client.ini'
        $bridge = Join-Path $folder 'WindowsStreamBridge.exe'
        $relay = Join-Path $folder 'relay.ini'
        $instance = 'relay-' + $pair.device.Substring(0, 32)
        $port = (Read-RelaySection $profile 'connection').port
        if ($port -notmatch '^18\d{3}$') { $port = $null }
        if (!$port) {
            $used = [Collections.Generic.HashSet[int]]::new()
            foreach ($listener in [Net.NetworkInformation.IPGlobalProperties]::GetIPGlobalProperties().GetActiveTcpListeners()) {
                $null = $used.Add($listener.Port)
            }
            if (Test-Path -LiteralPath $store) {
                foreach ($existing in Get-ChildItem -LiteralPath $store -Directory) {
                    $reserved = (Read-RelaySection (Join-Path $existing.FullName 'client.ini') 'connection').port
                    if ($reserved -match '^18\d{3}$') { $null = $used.Add([int]$reserved) }
                }
            }
            $port = 18000..18999 | Where-Object { !$used.Contains($_) } | Select-Object -First 1
            if (!$port) { throw '本机中转端口已用完。' }
        }
        $null = New-Item -ItemType Directory -Path $folder -Force
        $running = @(Get-StreamProcesses 'WindowsStreamBridge.exe')
        $existingBridge = $false
        foreach ($entry in $running) {
            if ($entry.Instance.ExecutablePath -eq $bridge) { $existingBridge = $true }
            $entry.Process.Dispose()
        }
        if (!$existingBridge) {
            Copy-Item -LiteralPath $source -Destination $bridge -Force
            Write-RelayIni $relay @"
[relay]
role=viewer
device=$($pair.device)
endpoint=wss://sdserver.luxvisions-inno.com/api/windowsstream/relay
server_address=10.18.32.67:443
http_proxy=$($network.http_proxy)
local_address=127.0.0.1:$port
token=$($pair.viewer_key)
"@
            Write-RelayIni $profile @"
[connection]
name=$($pair.name)（中转）
instance=$instance
host=127.0.0.1
port=$port
token=$($pair.stream_key)

[proxy]
enabled=0

[updates]
enabled=0
"@ ([Text.Encoding]::Unicode)
            Start-Process -FilePath $bridge -WorkingDirectory $folder -WindowStyle Hidden
        }
        $workspace = Start-Process -FilePath $controller -WorkingDirectory $controllerFolder -PassThru
        try { $null = $workspace.WaitForInputIdle(8000) }
        catch [InvalidOperationException] { if (!$workspace.HasExited) { throw } }
        catch [ComponentModel.Win32Exception] { if (!$workspace.HasExited) { throw } }
        finally { $workspace.Dispose() }
        Start-Process -FilePath $controller -WorkingDirectory $controllerFolder -ArgumentList @('--profile', ('"' + $profile + '"'))
    }
} catch {
    Add-Type -AssemblyName PresentationFramework
    [void][Windows.MessageBox]::Show($_.Exception.Message, 'WindowsStream')
    exit 1
}
