$ErrorActionPreference = 'Stop'
try {
    $values = @{}
    foreach ($line in [IO.File]::ReadAllLines((Join-Path $PSScriptRoot 'access.ini'))) {
        if ($line.Trim() -match '^[;#\[]' -or -not $line.Trim()) { continue }
        $pair = $line -split '=', 2
        if ($pair.Count -eq 2) { $values[$pair[0].Trim()] = $pair[1].Trim() }
    }
    foreach ($key in 'server_ip','source_ip') {
        $address = $null
        if (-not [Net.IPAddress]::TryParse($values[$key], [ref]$address) -or
            $address.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork -or
            $values[$key] -notmatch '^10\.18\.\d{1,3}\.\d{1,3}$') { throw '服务器或来源地址配置无效。' }
    }
    if (-not (Get-NetIPAddress -AddressFamily IPv4 | Where-Object IPAddress -eq $values.server_ip)) {
        throw '请把此文件夹复制到配置指定的数据库服务器上运行，当前电脑不符合服务器地址。'
    }
    if ($values.databases -notmatch '^[A-Za-z_][A-Za-z0-9_]*(,[A-Za-z_][A-Za-z0-9_]*)*$' -or
        $values.username -notmatch '^[A-Za-z_][A-Za-z0-9_]*$' -or
        $values.auth_method -notin @('md5','scram-sha-256')) { throw '数据库、用户或密码认证方式配置无效。' }
    $hba = $values.hba_file
    if (-not $hba) {
        $services = @(Get-CimInstance Win32_Service | Where-Object { $_.State -eq 'Running' -and $_.PathName -match '(?i)pg_ctl\.exe' })
        if ($services.Count -ne 1 -or $services[0].PathName -notmatch '(?i)(?:^|\s)-D\s*(?:"([^"]+)"|(\S+))') {
            throw '无法唯一定位正在运行的数据库，请在 access.ini 的 hba_file 中填写实际 pg_hba.conf 完整路径。'
        }
        $data = if ($Matches[1]) { $Matches[1] } else { $Matches[2] }
        $hba = Join-Path $data 'pg_hba.conf'
        $configuration = Join-Path $data 'postgresql.conf'
        if (Test-Path -LiteralPath $configuration) {
            foreach ($line in [IO.File]::ReadAllLines($configuration)) {
                if ($line -match "^\s*hba_file\s*=\s*'([^']+)'") {
                    $hba = if ([IO.Path]::IsPathRooted($Matches[1])) { $Matches[1] } else { Join-Path $data $Matches[1] }
                }
            }
        }
    }
    $hba = [IO.Path]::GetFullPath($hba)
    if (-not (Test-Path -LiteralPath $hba -PathType Leaf) -or [IO.Path]::GetFileName($hba) -ne 'pg_hba.conf') {
        throw '数据库访问配置文件不存在或文件名不正确。'
    }
    $original = [IO.File]::ReadAllBytes($hba)
    $rule = 'host    ' + $values.databases + '    ' + $values.username + '    ' + $values.source_ip + '/32    ' + $values.auth_method
    $block = "# WindowsStream database access`r`n" + $rule + "`r`n"
    if ([Text.Encoding]::ASCII.GetString($original).Contains($block)) {
        Write-Host '当前来源的数据库访问规则已经存在，请从数据库客户端重试。'
        exit 0
    }
    if ($original.Length -gt 1 -and (($original[0] -eq 255 -and $original[1] -eq 254) -or
        ($original[0] -eq 254 -and $original[1] -eq 255))) { throw '访问配置使用不支持的编码，未修改文件。' }
    $prefix = [Text.Encoding]::ASCII.GetBytes($block)
    [byte[]]$updated = $prefix + $original
    if ($original.Length -ge 3 -and $original[0] -eq 239 -and $original[1] -eq 187 -and $original[2] -eq 191) {
        [byte[]]$updated = $original[0..2] + $prefix + $original[3..($original.Length - 1)]
    }
    $tag = [Guid]::NewGuid().ToString('N')
    $temporary = $hba + '.windowsstream-' + $tag + '.tmp'
    $backup = $hba + '.windowsstream-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + $tag + '.bak'
    try {
        [IO.File]::WriteAllBytes($temporary, $updated)
        [IO.File]::Replace($temporary, $hba, $backup)
    } finally {
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
    }
    Write-Host ('原配置已备份：' + $backup)
    Write-Host ('已写入限定来源的密码认证规则：' + $rule)
    Write-Host 'Windows 上的新连接会读取此规则，无需重启数据库。请从数据库客户端重新测试，连接结果以客户端实际返回为准。'
} catch {
    Write-Host ('数据库访问修复未完成：' + $_.Exception.Message)
    exit 1
}
