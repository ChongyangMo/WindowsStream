param([switch]$Package, [switch]$Background, [string]$InstallTo, [switch]$AdminRelay)

$ErrorActionPreference = 'Stop'
if ($Package) { [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false) }
try {
    . (Join-Path $PSScriptRoot 'takeover.ps1')
    . (Join-Path $PSScriptRoot 'pairing.ps1')
    $folder = if ($InstallTo) { (Resolve-Path -LiteralPath $InstallTo).Path } else { $PSScriptRoot }
    $hostProgram = Join-Path $folder 'WindowsStreamHost.exe'
    $relayProgram = Join-Path $folder 'WindowsStreamBridge.exe'
    $hostConfig = Join-Path $folder 'host.ini'
    $relayConfig = Join-Path $folder 'relay.ini'
    Invoke-StreamLock 'Local\WindowsStreamHostUpdate' {
        if (!$InstallTo -and (!$AdminRelay -or !(Test-Path -LiteralPath $hostConfig) -or
            (Test-Path -LiteralPath (Join-Path $folder 'identity.ini')))) {
            Initialize-RelayHost $folder
        }
        Assert-StreamFiles @($hostConfig)
        $relay = Test-Path -LiteralPath $relayConfig
        if ($relay -and (Read-RelaySection $relayConfig 'relay').role -ne 'host') { throw '中转配置必须使用被控端角色。' }
        if ($relay -and $AdminRelay -and !$Background) {
            . (Join-Path $PSScriptRoot 'install-admin-consent.ps1')
            Install-AdminConsentRelay $folder
        }
        $old = @(foreach ($entry in Get-StreamProcesses 'WindowsStreamHost.exe') {
            if ($entry.Instance.CommandLine -match '\s--(?:server-\S+|apply-update|stop-user-host|relay-autostart)(?:\s|$)') {
                $entry.Process.Dispose()
            } else { $entry }
        })
        if ($Background -and $old.Count) {
            foreach ($entry in $old) { $entry.Process.Dispose() }
            return
        }
        $bridges = @(foreach ($entry in $old) {
            if ($entry.Instance.ExecutablePath) {
                $bridge = Join-Path (Split-Path -Parent $entry.Instance.ExecutablePath) 'WindowsStreamBridge.exe'
                if ((Get-BridgeIdentity $bridge) -like 'host/*') { $bridge }
            }
        })
        if ($relay) { $bridges += $relayProgram }
        Stop-StreamProcesses $old 'host'
        foreach ($bridge in ($bridges | Select-Object -Unique)) { Stop-OldBridges $bridge }
        if ($InstallTo) {
            . (Join-Path $PSScriptRoot 'install-configured.ps1')
            Install-ConfiguredHost $PSScriptRoot $folder $relay
        }
        $required = @($hostProgram)
        if ($relay) { $required += $relayProgram }
        Assert-StreamFiles $required
        $options = @{ FilePath = $hostProgram; WorkingDirectory = $folder; PassThru = $true }
        if ($Background) { $options.ArgumentList = '--background' }
        $started = Start-Process @options
        try {
            if (!$started.WaitForInputIdle(8000) -or $started.HasExited) { throw '新版 WindowsStream 被控端未能启动。' }
        } finally { $started.Dispose() }
        if ($relay) { Start-Process -FilePath $relayProgram -WorkingDirectory $folder -WindowStyle Hidden }
    }
} catch {
    if ($Background) {
        Add-Content -LiteralPath (Join-Path $PSScriptRoot 'startup.log') -Encoding UTF8 -ErrorAction SilentlyContinue `
            -Value ("$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') 中转自启失败：$($_.Exception.Message)")
        exit 1
    }
    if ($Package) {
        [Console]::Error.WriteLine($_.Exception.Message)
        exit 1
    }
    Add-Type -AssemblyName PresentationFramework
    [void][Windows.MessageBox]::Show($_.Exception.Message, 'WindowsStream')
    exit 1
}
