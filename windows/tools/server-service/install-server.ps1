$ErrorActionPreference = 'Stop'
$installed = $false
$previouslyRunning = $false
$backupExe = $null
$backupIni = $null
try {
    $settings = @{}
    foreach ($line in [IO.File]::ReadAllLines((Join-Path $PSScriptRoot 'server-service.ini'))) {
        if ($line.Trim() -match '^[;#\[]' -or -not $line.Trim()) { continue }
        $pair = $line -split '=', 2
        if ($pair.Count -eq 2) { $settings[$pair[0].Trim()] = $pair[1].Trim() }
    }
    if ($settings.enabled -ne '1') { Write-Host '服务器服务模式未启用。'; exit 0 }
    if (-not (Get-NetIPAddress -AddressFamily IPv4 | Where-Object IPAddress -eq $settings.server_ip)) {
        throw '当前电脑不是配置指定的服务器，未安装系统服务。'
    }
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw '服务器服务模式需要管理员权限。' }
    $root = [IO.Path]::GetFullPath($settings.install_root).TrimEnd('\')
    if ($root -ne 'C:\ProgramData\WindowsStreamServer') { throw '服务安装目录必须使用受保护的指定目录。' }
    $source = Join-Path $PSScriptRoot 'WindowsStreamHost.next.exe'
    $sourceIni = Join-Path $PSScriptRoot 'host.ini'
    if (-not (Test-Path -LiteralPath $source -PathType Leaf) -or -not (Test-Path -LiteralPath $sourceIni -PathType Leaf)) {
        throw '服务器升级文件或配对配置缺失。'
    }
    $name = 'WindowsStreamServer'
    $exe = Join-Path $root 'WindowsStreamHost.exe'
    $ini = Join-Path $root 'host.ini'
    $binary = '"' + $exe + '" --server-service'
    $service = Get-CimInstance Win32_Service -Filter "Name='WindowsStreamServer'"
    if ($service -and ($service.PathName -ne $binary -or $service.StartName -notin @('LocalSystem','NT AUTHORITY\SYSTEM'))) {
        throw '同名服务不是此服务器组件，未修改现有服务。'
    }
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    & icacls.exe $root /inheritance:r /grant:r '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-544:(OI)(CI)F' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw '无法保护服务器程序与配对配置目录。' }
    $tag = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
    if (Test-Path -LiteralPath $exe) { $backupExe = $exe + '.' + $tag + '.bak'; Copy-Item -LiteralPath $exe -Destination $backupExe }
    if (Test-Path -LiteralPath $ini) { $backupIni = $ini + '.' + $tag + '.bak'; Copy-Item -LiteralPath $ini -Destination $backupIni }
    if ($service) {
        $previouslyRunning = $service.State -eq 'Running'
        Stop-Service -Name $name -ErrorAction Stop
        (Get-Service -Name $name).WaitForStatus('Stopped', [TimeSpan]::FromSeconds(15))
    }
    # 先正常关闭旧用户进程；不同会话的旧实例按原部署路径精确回收。
    Start-Process -FilePath $source -ArgumentList '--stop-user-host' -WorkingDirectory $PSScriptRoot -WindowStyle Hidden -Wait | Out-Null
    Start-Sleep -Seconds 2
    $legacy = Join-Path $PSScriptRoot 'WindowsStreamHost.exe'
    $guardRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '.windowsstream-runtime')) + '\'
    $guards = @(Get-CimInstance Win32_Process -Filter "Name='WindowsStreamKeepAlive.exe'" | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($guardRoot, [StringComparison]::OrdinalIgnoreCase) })
    foreach ($process in $guards) { Stop-Process -Id $process.ProcessId -Force -ErrorAction SilentlyContinue }
    $old = @(Get-CimInstance Win32_Process -Filter "Name='WindowsStreamHost.exe'" | Where-Object ExecutablePath -eq $legacy)
    foreach ($process in @($old | Where-Object CommandLine -match '--watchdog') + @($old | Where-Object CommandLine -notmatch '--watchdog')) {
        Stop-Process -Id $process.ProcessId -Force -ErrorAction SilentlyContinue
    }
    Copy-Item -LiteralPath $source -Destination $exe -Force
    Copy-Item -LiteralPath $sourceIni -Destination $ini -Force
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class WindowsStreamServerIni {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern bool WritePrivateProfileString(string section, string key, string value, string path);
}
'@
    if (-not [WindowsStreamServerIni]::WritePrivateProfileString('server','secure_desktop','1',$ini)) { throw '无法写入服务器桌面配置。' }
    if ((Get-FileHash -LiteralPath $exe).Hash -ne (Get-FileHash -LiteralPath $source).Hash) { throw '服务器程序复制校验失败。' }
    if (-not $service) {
        New-Service -Name $name -DisplayName 'WindowsStream Server' -BinaryPathName $binary -StartupType Automatic -Description '服务器远程桌面会话代理' | Out-Null
    } else { Set-Service -Name $name -StartupType Automatic }
    $installed = $true
    Start-Service -Name $name
    (Get-Service -Name $name).WaitForStatus('Running', [TimeSpan]::FromSeconds(15))
    Start-Sleep -Seconds 5
    $status = Get-CimInstance Win32_Service -Filter "Name='WindowsStreamServer'"
    $agents = @(Get-CimInstance Win32_Process -Filter "Name='WindowsStreamHost.exe'" | Where-Object { $_.ExecutablePath -eq $exe -and $_.CommandLine -match '--server-agent' })
    [IO.File]::WriteAllText((Join-Path $PSScriptRoot 'service-state.json'), (@{state=$status.State;process=$status.ProcessId;agents=@($agents | Select-Object ProcessId,SessionId);hash=(Get-FileHash -LiteralPath $exe).Hash;time=(Get-Date -Format 's')} | ConvertTo-Json -Depth 4), [Text.UTF8Encoding]::new($false))
    if ($status.State -ne 'Running' -or -not $agents.Count) { throw '系统服务或交互会话代理尚未就绪。' }
    Write-Host '服务器系统服务及会话代理已启动，登录画面仍需通过实际远程连接检查。'
} catch {
    Write-Host ('服务器服务更新失败：' + $_.Exception.Message)
    if ($installed) { Stop-Service -Name 'WindowsStreamServer' -ErrorAction SilentlyContinue }
    if ($backupExe -and (Test-Path -LiteralPath $backupExe)) { Copy-Item -LiteralPath $backupExe -Destination $exe -Force }
    if ($backupIni -and (Test-Path -LiteralPath $backupIni)) { Copy-Item -LiteralPath $backupIni -Destination $ini -Force }
    if ($previouslyRunning) { Start-Service -Name 'WindowsStreamServer' -ErrorAction SilentlyContinue }
    exit 1
}
