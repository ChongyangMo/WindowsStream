function Start-ControlSshTask {
    param([string]$Server, [string]$Configuration, [string]$DataDirectory, [string]$Address, [int]$Port)
    $taskName = 'WindowsStream-HostControl'
    $existing = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
    if ($existing -and $existing.State -eq 'Running') { Stop-ScheduledTask -TaskName $taskName }
    $log = Join-Path $DataDirectory 'logs\host-control.log'
    $arguments = '-D -f "' + $Configuration + '" -E "' + $log + '"'
    $action = New-ScheduledTaskAction -Execute $Server -Argument $arguments -WorkingDirectory (Split-Path -Parent $Server)
    $principal = New-ScheduledTaskPrincipal -UserId 'S-1-5-18' -LogonType ServiceAccount -RunLevel Highest
    $trigger = New-ScheduledTaskTrigger -AtStartup
    $taskSettings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -StartWhenAvailable `
        -RestartCount 999 -RestartInterval ([TimeSpan]::FromMinutes(1)) -AllowStartIfOnBatteries `
        -DontStopIfGoingOnBatteries -MultipleInstances IgnoreNew
    Register-ScheduledTask -TaskName $taskName -Action $action -Principal $principal -Trigger $trigger `
        -Settings $taskSettings -Force | Out-Null
    Set-Service -Name sshd -StartupType Disabled
    Start-ScheduledTask -TaskName $taskName
    $deadline = [DateTime]::UtcNow.AddSeconds(12)
    do {
        $listener = Get-NetTCPConnection -State Listen -LocalAddress $Address -LocalPort $Port -ErrorAction SilentlyContinue
        if ($listener) {
            $process = Get-Process -Id $listener.OwningProcess -ErrorAction SilentlyContinue
            if ($process -and $process.Path -eq $Server) {
                Write-Host '宿主机连接已通过系统后台任务启动，开机自动恢复。'
                return
            }
        }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)
    Get-ScheduledTaskInfo -TaskName $taskName | Select-Object LastRunTime,LastTaskResult | Format-List | Out-Host
    if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log -Tail 80 | Out-Host }
    throw '系统后台任务未能启动连接程序，具体错误已写入安装日志。'
}
