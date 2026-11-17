$ErrorActionPreference = 'Stop'
try {
    . (Join-Path $PSScriptRoot 'takeover.ps1')
    $bridge = Join-Path $PSScriptRoot 'WindowsStreamBridge.exe'
    $controller = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..\..\controller\WindowsStream.exe')).Path
    $controllerFolder = Split-Path -Parent $controller
    $profile = Join-Path $PSScriptRoot 'client.ini'
    Assert-StreamFiles @($bridge, $profile, (Join-Path $PSScriptRoot 'relay.ini'), (Join-Path $controllerFolder 'client.ini'))
    Invoke-StreamLock 'Local\WindowsStreamControllerLaunch' {
        $old = @(Get-StreamProcesses 'WindowsStream.exe')
        $guards = @($old | Where-Object { $_.Instance.CommandLine -match '\s--proxy-guard(?:\s|$)' })
        try {
            $apps = @($old | Where-Object { $_.Instance.CommandLine -notmatch '\s--proxy-guard(?:\s|$)' })
            Stop-StreamProcesses $apps 'viewer'
            if (!(Wait-StreamExit $guards 5000)) { throw '旧版控制端仍在恢复系统代理，请稍后重试。' }
        } finally { foreach ($entry in $guards) { $entry.Process.Dispose() } }
        Stop-OldBridges $bridge
        Start-Process -FilePath $bridge -WorkingDirectory $PSScriptRoot -WindowStyle Hidden
        $started = Start-Process -FilePath $controller -WorkingDirectory $controllerFolder -PassThru
        try {
            if (!$started.WaitForInputIdle(8000) -or $started.HasExited) { throw '新版 WindowsStream 控制端未能启动。' }
        } finally { $started.Dispose() }
        Start-Process -FilePath $controller -ArgumentList @('--profile', ('"' + $profile + '"')) -WorkingDirectory $controllerFolder
    }
} catch {
    Add-Type -AssemblyName PresentationFramework
    [void][Windows.MessageBox]::Show($_.Exception.Message, 'WindowsStream')
    exit 1
}
