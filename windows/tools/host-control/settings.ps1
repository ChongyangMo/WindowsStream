function Read-ConnectionSettings {
    param([string]$Path)
    $values = @{}
    $section = ''
    foreach ($line in Get-Content -LiteralPath $Path -Encoding UTF8) {
        if ($line -match '^\s*\[([^\]]+)\]\s*$') { $section = $Matches[1]; continue }
        if ($section -eq 'connection' -and $line -match '^\s*([^=;#]+?)\s*=(.*)$') {
            $values[$Matches[1].Trim()] = $Matches[2].Trim()
        }
    }
    foreach ($name in @('host','client')) {
        $address = $null
        if (-not [Net.IPAddress]::TryParse($values[$name], [ref]$address) -or
            $address.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork) { throw "配置中的 $name 地址无效。" }
    }
    $port = 0
    if (-not [int]::TryParse($values.port, [ref]$port) -or $port -lt 1024 -or $port -gt 65535) { throw '连接端口无效。' }
    $values.port = $port
    return $values
}

function Set-ControlFileAcl {
    param([string]$Path, [switch]$IncludeCurrentUser)
    $allowed = @('S-1-5-18','S-1-5-32-544')
    if ($IncludeCurrentUser) { $allowed += [Security.Principal.WindowsIdentity]::GetCurrent().User.Value }
    $grants = @($allowed | ForEach-Object { '*' + $_ + ':F' })
    $icacls = Join-Path ([Environment]::GetFolderPath('System')) 'icacls.exe'
    & $icacls $Path /inheritance:r /grant:r $grants | Out-Null
    if ($LASTEXITCODE) { throw '设置连接密钥或配置文件权限失败。' }
    $entries = @((Get-Acl -LiteralPath $Path).Access)
    foreach ($entry in $entries) {
        $sid = $entry.IdentityReference.Translate([Security.Principal.SecurityIdentifier]).Value
        if ($sid -in $allowed) { continue }
        & $icacls $Path /remove ('*' + $sid) | Out-Null
        if ($LASTEXITCODE) { throw "清理连接文件的旧权限失败：$Path" }
    }
    foreach ($entry in (Get-Acl -LiteralPath $Path).Access) {
        if ($entry.IdentityReference.Translate([Security.Principal.SecurityIdentifier]).Value -notin $allowed) {
            throw "连接文件仍有多余权限：$Path"
        }
    }
}
