function Get-HostBuildStamp([string]$Path) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { return [uint32]0 }
    $reader = [IO.BinaryReader]::new([IO.File]::OpenRead($Path))
    try {
        if ($reader.BaseStream.Length -lt 64 -or $reader.ReadUInt16() -ne 0x5a4d) { return [uint32]0 }
        $reader.BaseStream.Position = 60
        $offset = $reader.ReadInt32()
        if ($offset -lt 64 -or $offset -gt $reader.BaseStream.Length - 12) { return [uint32]0 }
        $reader.BaseStream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x4550) { return [uint32]0 }
        $reader.BaseStream.Position = $offset + 8
        return $reader.ReadUInt32()
    } finally { $reader.Dispose() }
}

function Install-HostFile([string]$Source, [string]$Target) {
    if ((Test-Path -LiteralPath $Target -PathType Leaf) -and
        (Get-FileHash -LiteralPath $Source).Hash -eq (Get-FileHash -LiteralPath $Target).Hash) { return }
    $pending = Join-Path (Split-Path -Parent $Target) ('.windowsstream-' + [guid]::NewGuid().ToString('N'))
    try {
        [IO.File]::Copy($Source, $pending)
        if (Test-Path -LiteralPath $Target -PathType Leaf) {
            [IO.File]::Replace($pending, $Target, [NullString]::Value)
        } else {
            [IO.File]::Move($pending, $Target)
        }
    } finally {
        if (Test-Path -LiteralPath $pending -PathType Leaf) { [IO.File]::Delete($pending) }
    }
}

function Install-ConfiguredHost([string]$Payload, [string]$Folder, [bool]$Relay) {
    $hostName = 'WindowsStreamHost.exe'
    $bundled = Join-Path $Payload $hostName
    $installed = Join-Path $Folder $hostName
    $newerInstalled = (Get-HostBuildStamp $installed) -gt (Get-HostBuildStamp $bundled)
    $files = @($hostName)
    if ($Relay) { $files += @('WindowsStreamBridge.exe', 'start-host.ps1', 'takeover.ps1', 'pairing.ps1') }
    foreach ($name in $files) {
        $target = Join-Path $Folder $name
        if (!$newerInstalled -or !(Test-Path -LiteralPath $target -PathType Leaf)) {
            Install-HostFile (Join-Path $Payload $name) $target
        }
        $item = Get-Item -LiteralPath $target -Force
        $item.Attributes = $item.Attributes -bor [IO.FileAttributes]::Hidden
    }

    $ini = Join-Path $Folder 'host.ini'
    $background = Read-RelaySection $ini 'background'
    if ($Relay -and !$background.ContainsKey('relay_autostart') -and $background.manage_autostart -eq '0' -and
        ![WindowsStreamTakeover]::WritePrivateProfileString('background', 'manage_autostart', '1', $ini)) {
        throw '无法迁移中转被控端的自启动设置。'
    }
    $relayAutoStart = if ($Relay) { '1' } else { '0' }
    if (($Relay -or $background.ContainsKey('relay_autostart')) -and $background.relay_autostart -ne $relayAutoStart -and
        ![WindowsStreamTakeover]::WritePrivateProfileString('background', 'relay_autostart', $relayAutoStart, $ini)) {
        throw '无法保存被控端的启动方式。'
    }
}
