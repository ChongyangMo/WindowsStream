$ErrorActionPreference = 'Stop'
try {
    $principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw '请在宿主机右键 disable-host.cmd，选择以管理员身份运行。'
    }
    $data = [Environment]::GetFolderPath('CommonApplicationData')
    $config = Join-Path $data 'ssh\sshd_config'
    if (-not (Test-Path -LiteralPath $config) -or
        (Get-Content -LiteralPath $config -TotalCount 1) -ne '# WindowsStreamHostControl') {
        throw '当前 SSH 配置不属于此连接工具，未修改服务。'
    }
    if (Get-ScheduledTask -TaskName 'WindowsStream-HostControl' -ErrorAction SilentlyContinue) {
        Stop-ScheduledTask -TaskName 'WindowsStream-HostControl' -ErrorAction SilentlyContinue
        Unregister-ScheduledTask -TaskName 'WindowsStream-HostControl' -Confirm:$false
    }
    Stop-Service -Name sshd -ErrorAction SilentlyContinue
    Set-Service -Name sshd -StartupType Manual
    Get-NetFirewallRule -Name 'WindowsStream-HostControl-VM' -ErrorAction SilentlyContinue | Remove-NetFirewallRule
    $backup = Join-Path $data 'WindowsStreamHostControl\sshd_config.previous'
    if (Test-Path -LiteralPath $backup) { Copy-Item -LiteralPath $backup -Destination $config -Force }
    foreach ($name in @('host-ready.ini','host_key.pub')) {
        $path = Join-Path $PSScriptRoot $name
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
    }
    Write-Host '宿主机远程连接已停止，自动启动及对应防火墙规则已关闭。'
} catch {
    Write-Host "关闭失败：$($_.Exception.Message)"
    exit 1
}
