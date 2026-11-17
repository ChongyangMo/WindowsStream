param([switch]$Package, [switch]$Background)

$ErrorActionPreference = 'Stop'
if ($Package) { [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false) }
try {
    . (Join-Path $PSScriptRoot 'takeover.ps1')
    $hostProgram = Join-Path $PSScriptRoot 'WindowsStreamHost.exe'
    $relayProgram = Join-Path $PSScriptRoot 'WindowsStreamBridge.exe'
    Assert-StreamFiles @($hostProgram, $relayProgram)
    Invoke-StreamLock 'Local\WindowsStreamHostUpdate' {
        $pairing = Join-Path $PSScriptRoot 'pairing.ps1'
        if (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'enrollment.ini')) {
            . $pairing
            Initialize-RelayHost $PSScriptRoot
        }
        Assert-StreamFiles @((Join-Path $PSScriptRoot 'host.ini'), (Join-Path $PSScriptRoot 'relay.ini'))
        $old = @(foreach ($entry in Get-StreamProcesses 'WindowsStreamHost.exe') {
            if ($entry.Instance.CommandLine -match '\s--(?:server-\S+|apply-update|stop-user-host|relay-autostart)(?:\s|$)') {
                $entry.Process.Dispose()
            } else { $entry }
        })
        if ($Background -and $old.Count) {
            foreach ($entry in $old) { $entry.Process.Dispose() }
            return
        }
        Stop-StreamProcesses $old 'host'
        Stop-OldBridges $relayProgram
        $options = @{ FilePath = $hostProgram; WorkingDirectory = $PSScriptRoot; PassThru = $true }
        if ($Background) { $options.ArgumentList = '--background' }
        $started = Start-Process @options
        try {
            if (!$started.WaitForInputIdle(8000) -or $started.HasExited) { throw '新版 WindowsStream 被控端未能启动。' }
        } finally { $started.Dispose() }
        Start-Process -FilePath $relayProgram -WorkingDirectory $PSScriptRoot -WindowStyle Hidden
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
