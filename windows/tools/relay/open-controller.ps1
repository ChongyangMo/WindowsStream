$ErrorActionPreference = 'Stop'
try {
    $bridge = Join-Path $PSScriptRoot 'WindowsStreamBridge.exe'
    $controller = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..\..\controller\WindowsStream.exe')).Path
    $profile = Join-Path $PSScriptRoot 'client.ini'
    $running = @(Get-CimInstance Win32_Process -Filter "Name='WindowsStreamBridge.exe'" |
        Where-Object ExecutablePath -eq $bridge)
    if (!$running.Count) {
        Start-Process -FilePath $bridge -WorkingDirectory $PSScriptRoot -WindowStyle Hidden
    }
    Start-Process -FilePath $controller -ArgumentList @('--profile', ('"' + $profile + '"')) -WorkingDirectory (Split-Path -Parent $controller)
} catch {
    Add-Type -AssemblyName PresentationFramework
    [void][Windows.MessageBox]::Show($_.Exception.Message, 'WindowsStream')
    exit 1
}
