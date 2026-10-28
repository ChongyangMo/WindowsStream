function Repair-ControlSshDirectory {
    param([string]$Path)
    New-Item -ItemType Directory -Path $Path -Force | Out-Null
    $icacls = Join-Path ([Environment]::GetFolderPath('System')) 'icacls.exe'
    & $icacls $Path /inheritance:r /grant:r '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-544:(OI)(CI)F' '*S-1-5-11:(OI)(CI)RX' | Out-Null
    if ($LASTEXITCODE) { throw "修复连接服务目录权限失败：$Path" }
    & $icacls $Path /remove:g '*S-1-1-0' '*S-1-5-32-545' | Out-Null
    if ($LASTEXITCODE) { throw "清理连接服务目录权限失败：$Path" }
    & $icacls $Path /setowner '*S-1-5-32-544' | Out-Null
    if ($LASTEXITCODE) { throw "设置连接服务目录所有者失败：$Path" }
}

function Write-ControlSshDiagnostics {
    param([string]$Server, [string]$Configuration, [string]$DataDirectory)
    Write-Host '正在收集宿主机连接服务的具体错误。'
    try {
        Get-CimInstance Win32_Service -Filter "Name='sshd'" |
            Select-Object Name,State,StartName,PathName,ExitCode,ServiceSpecificExitCode | Format-List | Out-Host
        Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\sshd' |
            Select-Object ImagePath,ObjectName,ServiceSidType,RequiredPrivileges | Format-List | Out-Host
        Get-Item -LiteralPath $Server | Select-Object FullName,@{Name='Version';Expression={$_.VersionInfo.FileVersion}} | Format-List | Out-Host
        foreach ($path in @($DataDirectory,(Join-Path $DataDirectory 'logs'),$Configuration,(Join-Path $DataDirectory 'ssh_host_ed25519_key'))) {
            if (Test-Path -LiteralPath $path) {
                $acl = Get-Acl -LiteralPath $path
                [PSCustomObject]@{Path=$path;Owner=$acl.Owner;Access=$acl.AccessToString} | Format-List | Out-Host
            }
        }
        $events = @(Get-WinEvent -LogName 'OpenSSH/Operational' -MaxEvents 12 -ErrorAction SilentlyContinue)
        $events += @(Get-WinEvent -FilterHashtable @{LogName='System';StartTime=(Get-Date).AddMinutes(-30)} -MaxEvents 120 -ErrorAction SilentlyContinue |
            Where-Object { $_.ProviderName -eq 'Service Control Manager' -and $_.Message -match 'OpenSSH|sshd' } | Select-Object -First 8)
        $events | Select-Object TimeCreated,Id,Message | Format-List | Out-Host
        $log = Join-Path $DataDirectory 'logs\sshd.log'
        if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log -Tail 60 | Out-Host }
        if ((Get-Service -Name sshd).Status -eq 'Running') { return }
        $info = [Diagnostics.ProcessStartInfo]::new()
        $info.FileName = $Server
        $info.Arguments = '-D -e -ddd -f "' + $Configuration + '"'
        $info.UseShellExecute = $false
        $info.CreateNoWindow = $true
        $info.RedirectStandardOutput = $true
        $info.RedirectStandardError = $true
        $process = [Diagnostics.Process]::Start($info)
        $output = $process.StandardOutput.ReadToEndAsync()
        $errorOutput = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(3000)) {
            Write-Host '前台诊断进程能够保持运行，正在结束本次诊断。'
            $process.Kill(); $process.WaitForExit()
        }
        Write-Host ('前台诊断退出码：' + $process.ExitCode)
        $output.GetAwaiter().GetResult() | Out-Host
        $errorOutput.GetAwaiter().GetResult() | Out-Host
        $process.Dispose()
    } catch {
        Write-Host "收集服务诊断时遇到错误：$($_.Exception.Message)"
    }
}

function Start-ControlSshService {
    param([string]$Server, [string]$Configuration, [string]$DataDirectory, [string]$Address, [int]$Port)
    Repair-ControlSshDirectory $DataDirectory
    Repair-ControlSshDirectory (Join-Path $DataDirectory 'logs')
    $key = Join-Path $DataDirectory 'ssh_host_ed25519_key'
    $icacls = Join-Path ([Environment]::GetFolderPath('System')) 'icacls.exe'
    if (Test-Path -LiteralPath $key) {
        Set-ControlFileAcl $key
        & $icacls $key /setowner '*S-1-5-32-544' | Out-Null
        if ($LASTEXITCODE) { throw '设置连接服务密钥所有者失败。' }
    }
    & $icacls $Configuration /setowner '*S-1-5-32-544' | Out-Null
    if ($LASTEXITCODE) { throw '设置连接配置文件所有者失败。' }
    Set-ControlFileAcl $Configuration
    if (Get-ScheduledTask -TaskName 'WindowsStream-HostControl' -ErrorAction SilentlyContinue) {
        Start-ControlSshTask -Server $Server -Configuration $Configuration -DataDirectory $DataDirectory -Address $Address -Port $Port
        return 'task'
    }
    $service = Get-CimInstance Win32_Service -Filter "Name='sshd'"
    # 使用 Windows 默认配置位置，服务启动参数保持为原生形式。
    $binary = '"' + $Server + '"'
    $changed = Invoke-CimMethod -InputObject $service -MethodName Change -Arguments @{PathName=$binary}
    if ($changed.ReturnValue -ne 0) { throw "修复连接服务启动路径失败：$($changed.ReturnValue)" }
    Set-Service -Name sshd -StartupType Automatic
    try {
        Restart-Service -Name sshd
        (Get-Service -Name sshd).WaitForStatus('Running', [TimeSpan]::FromSeconds(15))
        return 'service'
    } catch {
        Write-ControlSshDiagnostics -Server $Server -Configuration $Configuration -DataDirectory $DataDirectory
        Write-Host 'Windows 服务启动失败，正在改用系统后台任务。'
        Start-ControlSshTask -Server $Server -Configuration $Configuration -DataDirectory $DataDirectory -Address $Address -Port $Port
        return 'task'
    }
}
