function New-RelayKey {
    $bytes = [byte[]]::new(32)
    $random = [Security.Cryptography.RandomNumberGenerator]::Create()
    try { $random.GetBytes($bytes) }
    finally { $random.Dispose() }
    return [BitConverter]::ToString($bytes).Replace('-', '').ToLowerInvariant()
}

function Get-RelayHash([byte[]]$Bytes) {
    $hash = [Security.Cryptography.SHA256]::Create()
    try { return $hash.ComputeHash($Bytes) }
    finally { $hash.Dispose() }
}

function ConvertTo-RelayHex([byte[]]$Bytes) {
    return [BitConverter]::ToString($Bytes).Replace('-', '').ToLowerInvariant()
}

function Read-RelaySection([string]$Path, [string]$Section) {
    $result = @{}
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { return $result }
    $active = $false
    foreach ($line in [IO.File]::ReadLines($Path)) {
        if ($line -match '^\s*\[([^\]]+)\]\s*$') {
            $active = $Matches[1] -eq $Section
        } elseif ($active -and $line -match '^\s*([a-z_]+)\s*=\s*(.*?)\s*$') {
            $result[$Matches[1]] = $Matches[2]
        }
    }
    return $result
}

function Write-RelayIni([string]$Path, [string]$Content, [Text.Encoding]$Encoding = [Text.UTF8Encoding]::new($false)) {
    $pending = $Path + '.new'
    [IO.File]::WriteAllText($pending, $Content + [Environment]::NewLine, $Encoding)
    Move-Item -LiteralPath $pending -Destination $Path -Force
}

function Initialize-RelayHost([string]$Folder) {
    $registration = Join-Path $Folder 'enrollment.ini'
    if (!(Test-Path -LiteralPath $registration -PathType Leaf)) { return }

    $identityFile = Join-Path $Folder 'identity.ini'
    $hostFile = Join-Path $Folder 'host.ini'
    $relayFile = Join-Path $Folder 'relay.ini'
    $pairFile = Join-Path $Folder 'WindowsStream-Pair.ini'
    $key = (Read-RelaySection $registration 'enrollment').key
    if ($key -cnotmatch '^[a-f0-9]{64}$') { throw '内网中转登记配置无效。' }
    if (!(Test-Path -LiteralPath $identityFile -PathType Leaf)) {
        if ((Test-Path -LiteralPath $relayFile) -or (Test-Path -LiteralPath $hostFile)) {
            throw '请将通用被控端解压到新目录再启动。'
        }
        Write-RelayIni $identityFile @"
[identity]
host_key=$(New-RelayKey)
viewer_key=$(New-RelayKey)
stream_key=$(New-RelayKey)
"@
    }
    $keys = Read-RelaySection $identityFile 'identity'
    foreach ($name in @('host_key', 'viewer_key', 'stream_key')) {
        if ($keys[$name] -cnotmatch '^[a-f0-9]{64}$') { throw '本机配对配置损坏。' }
    }
    $hostHash = Get-RelayHash ([Text.Encoding]::ASCII.GetBytes($keys.host_key))
    $viewerHash = Get-RelayHash ([Text.Encoding]::ASCII.GetBytes($keys.viewer_key))
    $device = ConvertTo-RelayHex (Get-RelayHash ([byte[]]($hostHash + $viewerHash)))
    $computer = [Net.Dns]::GetHostName() -replace '[^a-zA-Z0-9._-]', '-'

    if (!(Test-Path -LiteralPath $hostFile)) {
        $template = [IO.File]::ReadAllText((Join-Path $Folder 'host.ini.template'))
        Write-RelayIni $hostFile ($template.Replace('@stream_key@', $keys.stream_key))
    }
    if (!(Test-Path -LiteralPath $relayFile)) {
        Write-RelayIni $relayFile @"
[relay]
role=host
device=$device
endpoint=wss://sdserver.luxvisions-inno.com/api/windowsstream/relay
server_address=10.18.32.67:443
local_address=127.0.0.1:17413
token=$($keys.host_key)
enrollment=$key
viewer_sha256=$(ConvertTo-RelayHex $viewerHash)
"@
    }
    if (!(Test-Path -LiteralPath $pairFile)) {
        Write-RelayIni $pairFile @"
[pairing]
name=$computer
device=$device
viewer_key=$($keys.viewer_key)
stream_key=$($keys.stream_key)
"@
        Write-Host '已生成 WindowsStream-Pair.ini，请将配对文件交给控制端。'
    }
}
