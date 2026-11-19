function Wait-AdminConsentAgent([string]$EventId) {
    $eventName = 'Global\WindowsStreamConsent-' + $EventId
    for ($attempt = 0; $attempt -lt 16; $attempt++) {
        try {
            $ready = [Threading.EventWaitHandle]::OpenExisting($eventName, [Security.AccessControl.EventWaitHandleRights]::Synchronize)
            $ready.Dispose()
            return
        } catch [Threading.WaitHandleCannotBeOpenedException] {
            Start-Sleep -Milliseconds 500
        }
    }
    throw 'UAC 会话代理未启动，请使用原来的 Windows 管理员账号登录后重试。'
}

function Install-AdminConsentRelay([string]$Folder) {
    $name = 'WindowsStreamAdminConsent'
    $root = Join-Path (Split-Path -Parent (Split-Path -Parent $Folder)) 'consent'
    $source = Join-Path $Folder 'WindowsStreamHost.exe'
    $relayIni = Join-Path $Folder 'relay.ini'
    $connection = Read-RelaySection (Join-Path $Folder 'host.ini') 'connection'
    $relay = Read-RelaySection $relayIni 'relay'
    $owner = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $normalPort = 0
    if ($connection.mode -ne 'listen' -or $connection.bind_host -ne '127.0.0.1' -or
        ![int]::TryParse($connection.port, [ref]$normalPort) -or $normalPort -lt 1 -or $normalPort -gt 65535 -or
        $relay.local_address -ne "127.0.0.1:$normalPort" -or $relay.device -cnotmatch '^[a-z0-9-]{1,64}$' -or
        $connection.token.Length -lt 32) {
        throw 'UAC 中转需要原配对的本机回环连接配置。'
    }
    $port = if ($normalPort -eq 17415) { 17416 } else { 17415 }
    $eventId = ConvertTo-RelayHex (Get-RelayHash ([Text.Encoding]::ASCII.GetBytes($relay.device)))
    $encoded = [Text.UnicodeEncoding]::new($false, $true)
    $config = @"
[connection]
mode=listen
bind_host=127.0.0.1
port=$port
token=$($connection.token)

[video]
resolution=limited
max_width=1920
max_height=1080
fps=15
bitrate_mbps=8

[consent]
user_sid=$owner
event_id=$eventId
"@
    $null = New-Item -ItemType Directory -Path $root -Force
    $exe = Join-Path $root 'WindowsStreamHost.exe'
    $ini = Join-Path $root 'host.ini'
    $command = ('"{0}" --consent-service' -f $exe)
    $service = Get-CimInstance Win32_Service -Filter "Name='$name'"
    if ($service -and ($service.PathName -ne $command -or $service.StartName -notin @('LocalSystem', 'NT AUTHORITY\SYSTEM'))) {
        throw '同名 UAC 中转服务来自其他位置。'
    }
    $sameExe = (Test-Path -LiteralPath $exe -PathType Leaf) -and
        (Get-FileHash -LiteralPath $exe).Hash -eq (Get-FileHash -LiteralPath $source).Hash
    $sameIni = (Test-Path -LiteralPath $ini -PathType Leaf) -and
        [IO.File]::ReadAllText($ini).TrimEnd() -ceq $config.TrimEnd()
    if (![WindowsStreamTakeover]::WritePrivateProfileString('relay', 'consent_address', "127.0.0.1:$port", $relayIni)) {
        throw '无法保存 UAC 中转地址。'
    }
    if ($service -and $service.State -eq 'Running' -and $sameExe -and $sameIni) {
        Wait-AdminConsentAgent $eventId
        return
    }

    . (Join-Path $PSScriptRoot 'install-configured.ps1')
    $savedExe = $exe + '.rollback'
    $savedIni = $ini + '.rollback'
    $wasRunning = $service -and $service.State -eq 'Running'
    $created = $false
    $changed = $false
    try {
        if ($wasRunning) {
            Stop-Service -Name $name
            (Get-Service -Name $name).WaitForStatus('Stopped', [TimeSpan]::FromSeconds(12))
        }
        if (Test-Path -LiteralPath $exe) { Copy-Item -LiteralPath $exe -Destination $savedExe -Force }
        if (Test-Path -LiteralPath $ini) { Copy-Item -LiteralPath $ini -Destination $savedIni -Force }
        $changed = $true
        Install-HostFile $source $exe
        Write-RelayIni $ini $config $encoded
        if (!$service) {
            $properties = @{
                Name = $name
                DisplayName = 'WindowsStream 管理员版 UAC'
                BinaryPathName = $command
                StartupType = 'Automatic'
                Description = '在已配对的测试机上转发 Windows UAC 确认窗口'
            }
            New-Service @properties | Out-Null
            $created = $true
        }
        Start-Service -Name $name
        (Get-Service -Name $name).WaitForStatus('Running', [TimeSpan]::FromSeconds(12))
        Wait-AdminConsentAgent $eventId
        Write-Host '管理员版 UAC 中转服务已启动。'
    } catch {
        if ($created -or $changed) { Stop-Service -Name $name -ErrorAction SilentlyContinue }
        if ($created) { & (Join-Path ([Environment]::SystemDirectory) 'sc.exe') delete $name | Out-Null }
        if ($changed) {
            if (Test-Path -LiteralPath $savedExe) { Copy-Item -LiteralPath $savedExe -Destination $exe -Force }
            if (Test-Path -LiteralPath $savedIni) { Copy-Item -LiteralPath $savedIni -Destination $ini -Force }
        }
        if ($wasRunning) { Start-Service -Name $name -ErrorAction SilentlyContinue }
        throw
    } finally {
        [IO.File]::Delete($savedExe)
        [IO.File]::Delete($savedIni)
    }
}
