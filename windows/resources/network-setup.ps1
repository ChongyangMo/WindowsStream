$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName PresentationFramework
$ini = Join-Path (Split-Path -Parent $program) 'client.ini'
$log = Join-Path (Split-Path -Parent $program) 'client.log'

function Read-Setting([string]$Section, [string]$Key, [string]$Default) {
    $active = $false
    foreach ($line in [IO.File]::ReadLines($ini)) {
        if ($line -match '^\s*\[([^\]]+)\]\s*$') {
            $active = $Matches[1] -eq $Section
        } elseif ($active -and $line -match '^\s*([^=;]+?)\s*=\s*(.*?)\s*$' -and $Matches[1] -eq $Key) {
            return $Matches[2]
        }
    }
    return $Default
}

function Write-SetupLog([string]$Message) {
    Add-Content -LiteralPath $log -Encoding UTF8 -ErrorAction SilentlyContinue -Value (
        (Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + ' 网络配置：' + $Message)
}

function Configure-Wired {
    $address = Read-Setting 'network_setup' 'wired_address' '169.254.155.34'
    $adapter = Read-Setting 'network_setup' 'wired_adapter' ''
    $parsed = [Net.IPAddress]::Parse($address)
    $bytes = $parsed.GetAddressBytes()
    if ($parsed.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork -or
        $bytes[0] -ne 169 -or $bytes[1] -ne 254 -or $bytes[2] -in 0,255) {
        throw '网线直连地址应使用 169.254.1.0 到 169.254.254.255。'
    }
    $candidates = @(Get-NetAdapter -Physical | Where-Object {
        $_.Status -eq 'Up' -and $_.MediaType -eq '802.3' -and (!$adapter -or $_.Name -eq $adapter)
    })
    if (!$candidates.Count) { throw '未找到已连接的有线网卡，请检查网线和网卡。' }
    if ($candidates.Count -gt 1) { throw '有多个已连接的有线网卡，请在 client.ini 的 [network_setup] 中填写 wired_adapter。' }
    $nic = $candidates[0]
    $persistent = Get-NetIPAddress -InterfaceIndex $nic.ifIndex -IPAddress $address -PolicyStore PersistentStore -ErrorAction SilentlyContinue
    if (!$persistent) {
        Get-NetIPAddress -InterfaceIndex $nic.ifIndex -IPAddress $address -ErrorAction SilentlyContinue |
            Remove-NetIPAddress -Confirm:$false -ErrorAction Stop
        New-NetIPAddress -InterfaceIndex $nic.ifIndex -IPAddress $address -PrefixLength 16 | Out-Null
        Write-SetupLog ("已为网卡 $($nic.Name) 设置持久直连地址 $address/16")
    }
    $rule = 'WindowsStream discovery from B'
    if (Get-NetFirewallRule -DisplayName $rule -ErrorAction SilentlyContinue) {
        Set-NetFirewallRule -DisplayName $rule -Program $program -Enabled True
    } else {
        New-NetFirewallRule -DisplayName $rule -Direction Inbound -Action Allow -Protocol UDP -LocalPort 7401 -Program $program -Profile Any | Out-Null
    }
    return '网线直连配置完成。'
}

function Configure-VirtualMachines {
    $prefix = 'WindowsStream-VM-Proxy-'
    if ($action -eq 'remove-vm') {
        Get-NetFirewallRule -Name "$prefix*" -ErrorAction SilentlyContinue | Remove-NetFirewallRule
        return '虚拟机代理的防火墙规则已移除。'
    }
    $port = [int](Read-Setting 'proxy' 'local_port' '1080')
    if ($port -lt 1 -or $port -gt 65535) { throw '控制端的代理端口无效。' }
    $adapters = @(Get-NetAdapter -IncludeHidden | Where-Object {
        $_.Status -eq 'Up' -and "$($_.InterfaceDescription) $($_.Name)" -match '(?i)vmware' -and
        "$($_.InterfaceDescription) $($_.Name)" -match '(?i)vmnet|virtual ethernet'
    })
    if (!$adapters.Count) { throw '未找到已启用的 VMware 虚拟网卡，请检查 VMware 网络设置。' }
    $desired = @()
    foreach ($adapter in $adapters) {
        foreach ($ip in Get-NetIPAddress -InterfaceIndex $adapter.ifIndex -AddressFamily IPv4) {
            if ($ip.AddressState -ne 'Preferred' -or $ip.PrefixLength -lt 8 -or $ip.PrefixLength -gt 30) { continue }
            $bytes = [Net.IPAddress]::Parse($ip.IPAddress).GetAddressBytes()
            if (!($bytes[0] -eq 10 -or ($bytes[0] -eq 172 -and $bytes[1] -ge 16 -and $bytes[1] -le 31) -or
                ($bytes[0] -eq 192 -and $bytes[1] -eq 168))) { continue }
            $name = $prefix + $adapter.ifIndex + '-' + $ip.IPAddress.Replace('.', '-')
            for ($index = 0; $index -lt 4; $index++) {
                $bits = [Math]::Max(0, [Math]::Min(8, $ip.PrefixLength - 8 * $index))
                $bytes[$index] = $bytes[$index] -band [int](256 - [Math]::Pow(2, 8 - $bits))
            }
            $subnet = ($bytes -join '.') + '/' + $ip.PrefixLength
            $parameters = @{
                Name=$name; Direction='Inbound'; Action='Allow'; Enabled='True'; Protocol='TCP'
                LocalPort=$port; LocalAddress=$ip.IPAddress; RemoteAddress=$subnet
                InterfaceAlias=$adapter.Name; Program=$program; Profile='Any'; EdgeTraversalPolicy='Block'
            }
            if (Get-NetFirewallRule -Name $name -ErrorAction SilentlyContinue) {
                Set-NetFirewallRule @parameters | Out-Null
            } else {
                New-NetFirewallRule @parameters -DisplayName 'WindowsStream 虚拟机代理' | Out-Null
            }
            $desired += $name
            Write-SetupLog ("已允许虚拟机通过 $($ip.IPAddress):$port 访问代理")
        }
    }
    if (!$desired.Count) { throw 'VMware 虚拟网卡没有可用的私有 IPv4 地址。' }
    Get-NetFirewallRule -Name "$prefix*" -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -notin $desired } | Remove-NetFirewallRule
    return '防火墙配置完成，请在控制端开启「允许虚拟机访问」。'
}

try {
    if (!(Test-Path -LiteralPath $ini -PathType Leaf)) { throw '未找到控制端 client.ini 配置。' }
    switch ($action) {
        'wired' { $message = Configure-Wired }
        'vm' { $message = Configure-VirtualMachines }
        'remove-vm' { $message = Configure-VirtualMachines }
        default { throw '网络配置操作无效。' }
    }
    Write-SetupLog $message
    [void][Windows.MessageBox]::Show($message, 'WindowsStream', 'OK', 'Information')
    exit 0
} catch {
    $message = '网络配置失败：' + $_.Exception.Message
    Write-SetupLog $message
    [void][Windows.MessageBox]::Show($message, 'WindowsStream', 'OK', 'Error')
    exit 1
}
