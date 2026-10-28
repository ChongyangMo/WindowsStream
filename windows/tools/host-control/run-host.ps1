param([Parameter(Mandatory=$true)][string]$Command)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'settings.ps1')
try {
    $readyPath = Join-Path $PSScriptRoot 'host-ready.ini'
    if (-not (Test-Path -LiteralPath $readyPath)) { throw '宿主机尚未完成一次性配置，请先在宿主机运行 setup-host.cmd。' }
    $settings = Read-ConnectionSettings $readyPath
    if (-not $settings.user -or $settings.user -match '[\r\n"]') { throw '宿主机账户配置无效。' }
    $directory = Join-Path ([Environment]::GetFolderPath('UserProfile')) '.ssh'
    $key = Join-Path $directory 'windowsstream_host_ed25519'
    if (-not (Test-Path -LiteralPath $key)) { throw '未找到这台虚拟机的连接密钥。' }
    $hostKey = (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'host_key.pub') -Raw).Trim()
    if ($hostKey -notmatch '^ssh-ed25519 [A-Za-z0-9+/]+={0,2}(?: [^\r\n]*)?$') { throw '宿主机身份文件无效。' }
    $knownHosts = Join-Path $directory 'windowsstream_host_known_hosts'
    [IO.File]::WriteAllText($knownHosts, "[$($settings.host)]:$($settings.port) $hostKey`n", [Text.Encoding]::ASCII)
    $clientConfig = Join-Path $directory 'windowsstream_host_config'
    $content = @"
Host windowsstream-host
    HostName $($settings.host)
    Port $($settings.port)
    User "$($settings.user)"
    IdentityFile "$($key.Replace('\','/'))"
    UserKnownHostsFile "$($knownHosts.Replace('\','/'))"
    IdentitiesOnly yes
    StrictHostKeyChecking yes
    BatchMode yes
    ConnectTimeout 5
    ServerAliveInterval 10
    ServerAliveCountMax 3
"@
    [IO.File]::WriteAllText($clientConfig, $content + "`n", [Text.UTF8Encoding]::new($false))
    $prefix = '$ErrorActionPreference = ''Stop''; $ProgressPreference = ''SilentlyContinue''; [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false); '
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($prefix + $Command))
    $ssh = Join-Path ([Environment]::GetFolderPath('System')) 'OpenSSH\ssh.exe'
    & $ssh -T -F $clientConfig windowsstream-host "powershell.exe -NoProfile -NonInteractive -OutputFormat Text -EncodedCommand $encoded"
    exit $LASTEXITCODE
} catch {
    Write-Host "宿主机连接失败：$($_.Exception.Message)"
    exit 1
}
