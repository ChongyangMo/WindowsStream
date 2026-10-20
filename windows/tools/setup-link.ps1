# One-time setup on controller A (run as administrator).
# 1. Keeps a persistent link-local address on the wired adapter so cable replug or reboot does not remove it.
# 2. Allows inbound UDP 7401 for the controller so the host can discover A automatically.
param(
    [string]$Address = '169.254.155.34',
    [string]$Adapter = ''
)
$ErrorActionPreference = 'Stop'

if ($Adapter) {
    $nic = Get-NetAdapter -Name $Adapter
} else {
    $nic = Get-NetAdapter -Physical | Where-Object { $_.Status -eq 'Up' -and $_.MediaType -eq '802.3' } | Select-Object -First 1
}
if (-not $nic) { throw 'No connected wired adapter found. Pass -Adapter with the adapter name.' }
Write-Host "Wired adapter: $($nic.InterfaceDescription) (index $($nic.ifIndex))"

$persistent = Get-NetIPAddress -InterfaceIndex $nic.ifIndex -IPAddress $Address -PolicyStore PersistentStore -ErrorAction SilentlyContinue
if ($persistent) {
    Write-Host "Persistent address $Address already configured."
} else {
    Get-NetIPAddress -InterfaceIndex $nic.ifIndex -IPAddress $Address -ErrorAction SilentlyContinue |
        Remove-NetIPAddress -Confirm:$false -ErrorAction SilentlyContinue
    New-NetIPAddress -InterfaceIndex $nic.ifIndex -IPAddress $Address -PrefixLength 16 | Out-Null
    Write-Host "Added persistent address $Address/16."
}

$program = Join-Path $PSScriptRoot 'WindowsStream.exe'
$rule = 'WindowsStream discovery from B'
if (Get-NetFirewallRule -DisplayName $rule -ErrorAction SilentlyContinue) {
    Set-NetFirewallRule -DisplayName $rule -Program $program -Enabled True
    Write-Host 'Firewall rule updated.'
} else {
    New-NetFirewallRule -DisplayName $rule -Direction Inbound -Action Allow -Protocol UDP -LocalPort 7401 -Program $program -Profile Any | Out-Null
    Write-Host 'Firewall rule added: inbound UDP 7401.'
}
Write-Host 'Done.'
