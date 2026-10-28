$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'settings.ps1')
. (Join-Path $PSScriptRoot 'ssh-service.ps1')
. (Join-Path $PSScriptRoot 'ssh-task.ps1')
$transcribing = $false
try {
    Start-Transcript -Path (Join-Path $PSScriptRoot 'setup-host.log') -Append | Out-Null
    $transcribing = $true
    $settings = Read-ConnectionSettings (Join-Path $PSScriptRoot 'connection.ini')
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw '请在宿主机右键 setup-host.cmd，选择以管理员身份运行。'
    }
    $address = Get-NetIPAddress -AddressFamily IPv4 -IPAddress $settings.host -ErrorAction SilentlyContinue
    if (-not $address) { throw '这台电脑没有配置中的宿主机地址，请在宿主机运行此脚本。' }
    $adapter = Get-NetAdapter -InterfaceIndex $address.InterfaceIndex
    if ("$($adapter.InterfaceDescription) $($adapter.Name)" -notmatch '(?i)vmware') {
        throw '配置的监听地址不属于 VMware 虚拟网卡。'
    }
    $account = Get-CimInstance Win32_UserAccount -Filter "SID='$($identity.User.Value)'"
    if (-not $account.LocalAccount) { throw '此安装脚本需要使用宿主机的本地管理员账户运行。' }
    $user = $account.Name.ToLowerInvariant()
    $publicKey = (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'vm_key.pub') -Raw).Trim()
    if ($publicKey -notmatch '^ssh-ed25519 [A-Za-z0-9+/]+={0,2}(?: [^\r\n]*)?$') { throw '虚拟机公钥文件无效。' }
    $stateDirectory = Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) 'WindowsStreamHostControl'
    $marker = Join-Path $stateDirectory 'managed.ini'
    $sshData = Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) 'ssh'
    $serverConfig = Join-Path $sshData 'sshd_config'
    $ownConfiguration = (Test-Path -LiteralPath $serverConfig) -and
        ((Get-Content -LiteralPath $serverConfig -TotalCount 1) -eq '# WindowsStreamHostControl')
    $existing = Get-Service -Name sshd -ErrorAction SilentlyContinue
    if ($existing -and $existing.Status -eq 'Running' -and -not $ownConfiguration) {
        throw '宿主机已有运行中的 SSH 服务，未覆盖其配置。请先告知现有连接方式。'
    }
    $sshDirectory = Join-Path ([Environment]::GetFolderPath('System')) 'OpenSSH'
    $server = Join-Path $sshDirectory 'sshd.exe'
    if (-not (Test-Path -LiteralPath $server)) {
        Write-Host '正在安装 Windows 自带的宿主机连接组件，首次安装可能需要几分钟。'
        Add-WindowsCapability -Online -Name 'OpenSSH.Server~~~~0.0.1.0' | Out-Null
        if (-not (Test-Path -LiteralPath $server)) { throw '连接组件安装未完成，请查看日志。' }
    }
    New-Item -ItemType Directory -Path $stateDirectory -Force | Out-Null
    New-Item -ItemType Directory -Path $sshData -Force | Out-Null
    $backup = Join-Path $stateDirectory 'sshd_config.previous'
    if (-not $ownConfiguration -and (Test-Path -LiteralPath $serverConfig) -and -not (Test-Path -LiteralPath $backup)) {
        Copy-Item -LiteralPath $serverConfig -Destination $backup
    }
    $authorizedKeys = Join-Path $stateDirectory 'vm_authorized_keys'
    [IO.File]::WriteAllText($authorizedKeys,
        ('from="' + $settings.client + '",no-agent-forwarding,no-port-forwarding,no-X11-forwarding ' + $publicKey + "`n"),
        [Text.Encoding]::ASCII)
    Set-ControlFileAcl $authorizedKeys
    $keyPath = $authorizedKeys.Replace('\','/')
    $configuration = @"
# WindowsStreamHostControl
Port $($settings.port)
ListenAddress $($settings.host)
AddressFamily inet
HostKey $($sshData.Replace('\','/'))/ssh_host_ed25519_key
SyslogFacility LOCAL0
LogLevel VERBOSE
PubkeyAuthentication yes
AuthenticationMethods publickey
PasswordAuthentication no
AuthorizedKeysFile "$keyPath"
AllowUsers "$user@$($settings.client)"
AllowAgentForwarding no
AllowTcpForwarding no
GatewayPorts no
"@
    $candidate = Join-Path $stateDirectory 'sshd_config.next'
    [IO.File]::WriteAllText($candidate, $configuration + "`n", [Text.UTF8Encoding]::new($false))
    & (Join-Path $sshDirectory 'ssh-keygen.exe') -A
    if ($LASTEXITCODE) { throw '生成宿主机身份密钥失败。' }
    & $server -t -f $candidate
    if ($LASTEXITCODE) { throw '宿主机连接配置检查失败，未替换原配置。' }
    Set-ControlFileAcl $candidate
    Copy-Item -LiteralPath $candidate -Destination $serverConfig -Force
    Set-ControlFileAcl $serverConfig
    if (-not (Get-Service -Name sshd -ErrorAction SilentlyContinue)) {
        New-Service -Name sshd -BinaryPathName ('"' + $server + '"') -DisplayName 'OpenSSH SSH Server' -StartupType Automatic | Out-Null
    }
    $ruleName = 'WindowsStream-HostControl-VM'
    $rule = @{
        Name=$ruleName; Direction='Inbound'; Action='Allow'; Enabled='True'; Protocol='TCP'
        LocalAddress=$settings.host; LocalPort=$settings.port; RemoteAddress=$settings.client
        InterfaceAlias=$adapter.Name; Program=$server; Profile='Any'; EdgeTraversalPolicy='Block'
    }
    if (Get-NetFirewallRule -Name $ruleName -ErrorAction SilentlyContinue) { Set-NetFirewallRule @rule | Out-Null }
    else { New-NetFirewallRule @rule -DisplayName 'WindowsStream 宿主机连接' | Out-Null }
    $connectionMode = Start-ControlSshService -Server $server -Configuration $serverConfig -DataDirectory $sshData -Address $settings.host -Port $settings.port
    if (-not (Get-NetTCPConnection -State Listen -LocalAddress $settings.host -LocalPort $settings.port -ErrorAction SilentlyContinue)) {
        throw '连接服务已启动，但没有监听指定地址，请检查 OpenSSH 日志。'
    }
    $hostKey = (Get-Content -LiteralPath (Join-Path $sshData 'ssh_host_ed25519_key.pub') -Raw).Trim()
    $project = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
    $ready = "[connection]`r`nhost=$($settings.host)`r`nport=$($settings.port)`r`nclient=$($settings.client)`r`nuser=$user`r`nproject=$project`r`nmode=$connectionMode`r`n"
    [IO.File]::WriteAllText($marker, $ready, [Text.UTF8Encoding]::new($true))
    [IO.File]::WriteAllText((Join-Path $PSScriptRoot 'host-ready.ini'), $ready, [Text.UTF8Encoding]::new($true))
    [IO.File]::WriteAllText((Join-Path $PSScriptRoot 'host_key.pub'), $hostKey + "`n", [Text.Encoding]::ASCII)
    Write-Host '宿主机连接已启用，虚拟机现在可以直接编译和管理程序；重启宿主机后会自动恢复。'
} catch {
    Write-Host "配置失败：$($_.Exception.Message)"
    exit 1
} finally {
    if ($transcribing) { Stop-Transcript | Out-Null }
}
