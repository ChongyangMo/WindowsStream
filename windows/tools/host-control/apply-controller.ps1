param([switch]$Desktop, [string]$RunId)
$ErrorActionPreference = 'Stop'
$project = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$build = Join-Path $project 'windows\build'
$resultPath = Join-Path $build 'controller-deploy.ini'

if (-not $Desktop) {
    $user = (Get-CimInstance Win32_ComputerSystem).UserName
    if (-not $user) { throw '宿主机没有已登录的桌面会话，无法启动控制端窗口。' }
    $RunId = [Guid]::NewGuid().ToString('N')
    $taskName = 'WindowsStream-ApplyController-' + $RunId
    $powershell = Join-Path ([Environment]::GetFolderPath('System')) 'WindowsPowerShell\v1.0\powershell.exe'
    $arguments = '-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File "' + $PSCommandPath + '" -Desktop -RunId ' + $RunId
    $action = New-ScheduledTaskAction -Execute $powershell -Argument $arguments -WorkingDirectory $project
    $principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
    $settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::FromMinutes(2))
    Register-ScheduledTask -TaskName $taskName -Action $action -Principal $principal -Settings $settings | Out-Null
    try {
        Start-ScheduledTask -TaskName $taskName
        $deadline = [DateTime]::UtcNow.AddSeconds(40)
        while ([DateTime]::UtcNow -lt $deadline) {
            if (Test-Path -LiteralPath $resultPath) {
                $result = Get-Content -LiteralPath $resultPath -Raw -Encoding UTF8
                if ($result -match "(?m)^run=$RunId\r?$" -and $result -match '(?m)^success=([01])\r?$') {
                    if ($Matches[1] -ne '1') { throw '更新控制端失败，请查看 controller-deploy.log。' }
                    Write-Host '控制端已在宿主机桌面更新并启动。'
                    exit 0
                }
            }
            Start-Sleep -Milliseconds 500
        }
        throw '等待宿主机桌面更新超时，请查看 controller-deploy.log。'
    } finally {
        Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
    }
}

$success = $false
$transcribing = $false
$copying = $false
$controller = Join-Path $build 'controller\WindowsStream.exe'
$staged = Join-Path $build 'controller\WindowsStream.next.exe'
$backup = Join-Path $build 'controller\WindowsStream.previous.exe'
try {
    if ($RunId -notmatch '^[a-f0-9]{32}$') { throw '更新任务标识无效。' }
    Start-Transcript -Path (Join-Path $build 'controller-deploy.log') | Out-Null
    $transcribing = $true
    if (-not (Test-Path -LiteralPath $staged)) { throw '尚未生成新版控制端，请先编译。' }
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class WindowsStreamControllerWindow {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern IntPtr FindWindow(string className, IntPtr title);
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
}
'@
    if (Test-Path -LiteralPath $controller) { Copy-Item -LiteralPath $controller -Destination $backup -Force }
    $window = [WindowsStreamControllerWindow]::FindWindow('WindowsStreamWindow', [IntPtr]::Zero)
    if ($window -ne [IntPtr]::Zero) {
        $controllerId = [uint32]0
        [void][WindowsStreamControllerWindow]::GetWindowThreadProcessId($window, [ref]$controllerId)
        if ((Get-Process -Id $controllerId).Path -ne $controller) { throw '当前控制端不在此构建目录，未关闭其他程序。' }
        if (-not [WindowsStreamControllerWindow]::PostMessage($window, 0x111, [IntPtr]42002, [IntPtr]::Zero)) {
            throw '未能请求旧控制端正常退出。'
        }
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(12)
    do {
        $remaining = @(Get-Process -Name WindowsStream -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $controller })
        if (-not $remaining.Count) { break }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($remaining.Count) { throw '旧控制端仍在运行，未覆盖程序文件。' }
    $copying = $true
    Copy-Item -LiteralPath $staged -Destination $controller -Force
    Start-Process -FilePath $controller -WorkingDirectory (Split-Path -Parent $controller) | Out-Null
    Start-Sleep -Seconds 3
    if (-not (Get-Process -Name WindowsStream -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $controller })) {
        throw '新版控制端启动后退出，请查看 client.log。'
    }
    $success = $true
    Write-Host '控制端已更新，旧程序已备份，配置保持不变。'
} catch {
    Write-Host "更新失败：$($_.Exception.Message)"
    if ($copying -and (Test-Path -LiteralPath $backup) -and
        -not (Get-Process -Name WindowsStream -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $controller })) {
        Copy-Item -LiteralPath $backup -Destination $controller -Force
        Start-Process -FilePath $controller -WorkingDirectory (Split-Path -Parent $controller) | Out-Null
        Write-Host '已恢复并启动原控制端。'
    }
} finally {
    $result = "run=$RunId`r`nsuccess=$([int]$success)`r`n"
    [IO.File]::WriteAllText($resultPath, $result, [Text.UTF8Encoding]::new($true))
    if ($transcribing) { Stop-Transcript | Out-Null }
}
if (-not $success) { exit 1 }
