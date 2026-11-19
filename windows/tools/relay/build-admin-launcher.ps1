param(
    [Parameter(Mandatory)][string]$Launcher,
    [string]$ManifestTool
)

$ErrorActionPreference = 'Stop'
$source = (Resolve-Path -LiteralPath $Launcher).Path
if (!$ManifestTool) {
    $kits = Get-ItemPropertyValue -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -Name KitsRoot10
    $ManifestTool = Get-ChildItem -LiteralPath (Join-Path $kits 'bin') -Directory |
        Where-Object Name -Match '^10\.\d+\.\d+\.\d+$' |
        Sort-Object { [version]$_.Name } -Descending |
        ForEach-Object { Join-Path $_.FullName 'x64\mt.exe' } |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Select-Object -First 1
    if (!$ManifestTool) { throw '未找到 Windows SDK 的 mt.exe，请通过 -ManifestTool 指定路径。' }
}

$manifest = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../../resources/host-admin.manifest')).Path
$folder = Split-Path -Parent $source
$destination = Join-Path $folder 'WindowsStream-Host-Admin.exe'
if ($source -eq $destination) { throw '管理员版启动程序的输入与输出不能相同。' }
$pending = Join-Path $folder ('.windowsstream-admin-' + [guid]::NewGuid().ToString('N') + '.exe')
try {
    [IO.File]::Copy($source, $pending)
    & $ManifestTool -nologo -manifest $manifest "-outputresource:$pending;#1"
    if ($LASTEXITCODE -ne 0) { throw '写入被控端管理员权限清单失败。' }
    if ([IO.File]::Exists($destination)) {
        [IO.File]::Replace($pending, $destination, [NullString]::Value)
    } else {
        [IO.File]::Move($pending, $destination)
    }
} finally {
    [IO.File]::Delete($pending)
}
Write-Output "已生成管理员版被控端：$destination"
