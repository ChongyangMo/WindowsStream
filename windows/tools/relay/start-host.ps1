$ErrorActionPreference = 'Stop'
try {
    $hostProgram = Join-Path $PSScriptRoot 'WindowsStreamHost.exe'
    $relayProgram = Join-Path $PSScriptRoot 'WindowsStreamBridge.exe'
    $hostProcesses = @(Get-CimInstance Win32_Process -Filter "Name='WindowsStreamHost.exe'" |
        Where-Object SessionId -eq ([Diagnostics.Process]::GetCurrentProcess().SessionId))
    if ($hostProcesses | Where-Object ExecutablePath -ne $hostProgram) {
        throw '请先从正在运行的 WindowsStream 被控端点「退出」，再启动中转。'
    }
    if (!$hostProcesses.Count) {
        Start-Process -FilePath $hostProgram -WorkingDirectory $PSScriptRoot
    }
    $relays = @(Get-CimInstance Win32_Process -Filter "Name='WindowsStreamBridge.exe'" |
        Where-Object ExecutablePath -eq $relayProgram)
    if (!$relays.Count) {
        Start-Process -FilePath $relayProgram -WorkingDirectory $PSScriptRoot -WindowStyle Hidden
    }
} catch {
    Add-Type -AssemblyName PresentationFramework
    [void][Windows.MessageBox]::Show($_.Exception.Message, 'WindowsStream')
    exit 1
}
