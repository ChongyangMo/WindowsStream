param([switch]$Remove)
$ErrorActionPreference = 'Stop'
try {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw '请在宿主机右键此脚本对应的 CMD 文件，选择以管理员身份运行。'
    }
    $rulePrefix = 'WindowsStream-VM-Proxy-'
    if ($Remove) {
        Get-NetFirewallRule -Name "$rulePrefix*" -ErrorAction SilentlyContinue | Remove-NetFirewallRule
        Write-Host '虚拟机代理的防火墙规则已移除。'
        exit 0
    }
    $program = Join-Path $PSScriptRoot 'WindowsStream.exe'
    $ini = Join-Path $PSScriptRoot 'client.ini'
    if (-not (Test-Path -LiteralPath $program)) {
        $program = Join-Path $PSScriptRoot '..\build\Release\controller\WindowsStream.exe'
        $ini = Join-Path $PSScriptRoot '..\build\Release\controller\client.ini'
    }
    $program = (Resolve-Path -LiteralPath $program).Path
    if (-not (Test-Path -LiteralPath $ini)) { throw '未找到控制端配置，请先构建 WindowsStream。' }
    $port = 1080
    $section = ''
    foreach ($line in Get-Content -LiteralPath $ini) {
        if ($line -match '^\s*\[([^\]]+)\]') { $section = $Matches[1]; continue }
        if ($section -eq 'proxy' -and $line -match '^\s*local_port\s*=\s*(\d+)\s*$') { $port = [int]$Matches[1] }
    }
    if ($port -lt 1 -or $port -gt 65535) { throw '控制端的代理端口无效。' }
    $adapters = @(Get-NetAdapter -IncludeHidden | Where-Object {
        $_.Status -eq 'Up' -and "$($_.InterfaceDescription) $($_.Name)" -match '(?i)vmware' -and
        "$($_.InterfaceDescription) $($_.Name)" -match '(?i)vmnet|virtual ethernet'
    })
    if (-not $adapters.Count) { throw '未找到已启用的 VMware 虚拟网卡，请检查 VMware 网络设置。' }
    $desiredRules = @()
    foreach ($adapter in $adapters) {
        foreach ($ip in Get-NetIPAddress -InterfaceIndex $adapter.ifIndex -AddressFamily IPv4) {
            if ($ip.AddressState -ne 'Preferred' -or $ip.PrefixLength -lt 8 -or $ip.PrefixLength -gt 30) { continue }
            $bytes = [Net.IPAddress]::Parse($ip.IPAddress).GetAddressBytes()
            if (-not ($bytes[0] -eq 10 -or ($bytes[0] -eq 172 -and $bytes[1] -ge 16 -and $bytes[1] -le 31) -or
                ($bytes[0] -eq 192 -and $bytes[1] -eq 168))) { continue }
            $ruleName = $rulePrefix + $adapter.ifIndex + '-' + $ip.IPAddress.Replace('.', '-')
            for ($index = 0; $index -lt 4; $index++) {
                $bits = [Math]::Max(0, [Math]::Min(8, $ip.PrefixLength - 8 * $index))
                $bytes[$index] = $bytes[$index] -band [int](256 - [Math]::Pow(2, 8 - $bits))
            }
            $subnet = ($bytes -join '.') + '/' + $ip.PrefixLength
            $desiredRules += $ruleName
            $parameters = @{
                Name = $ruleName; Direction = 'Inbound'; Action = 'Allow'; Enabled = 'True'; Protocol = 'TCP'
                LocalPort = $port; LocalAddress = $ip.IPAddress; RemoteAddress = $subnet
                InterfaceAlias = $adapter.Name; Program = $program; Profile = 'Any'; EdgeTraversalPolicy = 'Block'
            }
            if (Get-NetFirewallRule -Name $ruleName -ErrorAction SilentlyContinue) {
                Set-NetFirewallRule @parameters | Out-Null
            } else {
                New-NetFirewallRule @parameters -DisplayName 'WindowsStream 虚拟机代理' | Out-Null
            }
            Write-Host "已允许虚拟机通过 $($ip.IPAddress):$port 访问代理。"
        }
    }
    if (-not $desiredRules.Count) { throw 'VMware 虚拟网卡没有可用的私有 IPv4 地址。' }
    Get-NetFirewallRule -Name "$rulePrefix*" -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -notin $desiredRules } | Remove-NetFirewallRule
    Write-Host '防火墙设置完成；请保持 WindowsStream 运行，并开启允许虚拟机访问。'
} catch {
    Write-Host "设置失败：$($_.Exception.Message)"
    exit 1
}
