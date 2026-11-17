param (
    [Parameter(Mandatory)][string]$Release,
    [Parameter(Mandatory)][string]$PreviousRelease,
    [switch]$Universal
)

$ErrorActionPreference = 'Stop'
$base = 'http://10.18.32.67:8001/api/devsync'
$proxy = 'http://127.0.0.1:1080'
$site = 'https://sdserver.luxvisions-inno.com'
$stageExe = 'maintenance/gitea-server/netdisk.next.exe'
$stageManifest = 'maintenance/gitea-server/package-hashes.txt'
$configName = if ($Universal) { 'windowsstream-enrollment.ini' } else { 'windowsstream-relay.ini' }
$serverConfig = 'config/' + $configName
$oldExe = Join-Path $PreviousRelease 'netdisk.next.exe'
$oldManifest = Join-Path $PreviousRelease 'package-hashes.txt'
$newExe = Join-Path $Release 'netdisk.next.exe'
$newManifest = Join-Path $Release 'package-hashes.txt'
$newConfig = Join-Path $Release $configName

function Get-LocalHash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }

function Get-RemoteHash([string]$Path) {
    $result = Invoke-WebRequest "$base/file_hash?path=$([uri]::EscapeDataString($Path))" -Proxy $proxy -TimeoutSec 10 -SkipHttpErrorCheck
    if ($result.StatusCode -eq 404) { return $null }
    if ($result.StatusCode -ne 200) { throw ('无法读取发布文件哈希：' + $Path) }
    return ($result.Content | ConvertFrom-Json).sha256
}

function Confirm-Remote([string]$Path, [string]$Local) {
    if ((Get-RemoteHash $Path) -ne (Get-LocalHash $Local)) { throw ('服务器文件版本不一致：' + $Path) }
}

function Upload-Artifact([string]$Local, [string]$Path) {
    Invoke-WebRequest "$base/file?path=$([uri]::EscapeDataString($Path))" -Proxy $proxy -Method Put -InFile $Local -ContentType 'application/octet-stream' -TimeoutSec 120 | Out-Null
    Confirm-Remote $Path $Local
}

function Get-Status { Invoke-RestMethod "$base/status" -Proxy $proxy -TimeoutSec 10 }

function Wait-Backend([string]$PreviousSuccess) {
    $deadline = [DateTime]::UtcNow.AddMinutes(3)
    do {
        Start-Sleep -Seconds 2
        try { $status = Get-Status } catch { continue }
        if ($status.restart.in_progress) { continue }
        if ($status.restart.last_error) { throw ('后端发布失败：' + $status.restart.last_error) }
        if ($status.backend_healthy -and $status.restart.last_success_at -ne $PreviousSuccess) { return }
    } while ([DateTime]::UtcNow -lt $deadline)
    throw '等待生产后端恢复超时。'
}

function Restart-Backend {
    $before = (Get-Status).restart.last_success_at
    Invoke-RestMethod "$base/restart" -Method Post -Proxy $proxy -TimeoutSec 15 | Out-Null
    Wait-Backend $before
}

function Get-SiteStatus([string]$Path) {
    (Invoke-WebRequest ($site + $Path) -Proxy $proxy -TimeoutSec 12 -SkipHttpErrorCheck).StatusCode
}

$status = Get-Status
if ($status.restart.in_progress -or !$status.backend_healthy) { throw '生产后端当前不可发布。' }
Confirm-Remote 'netdisk.exe' $oldExe
Confirm-Remote $stageExe $oldExe
Confirm-Remote $stageManifest $oldManifest
$existingConfig = Get-RemoteHash $serverConfig
if ($existingConfig -and $existingConfig -ne (Get-LocalHash $newConfig)) {
    throw '服务器已有不同的中转配置。'
}
if ($Universal) {
    Confirm-Remote 'config/windowsstream-relay.ini' (Join-Path $PreviousRelease 'windowsstream-relay.ini')
}

$manifest = [IO.File]::ReadAllText($oldManifest)
$entry = [regex]'(?m)^[a-fA-F0-9]{64}(?=[ \t]+netdisk\.next\.exe\r?$)'
if ($entry.Matches($manifest).Count -ne 1) { throw '后端发布清单条目不唯一。' }
[IO.File]::WriteAllText($newManifest, $entry.Replace($manifest, (Get-LocalHash $newExe)), [Text.UTF8Encoding]::new($false))

try {
    if (!$existingConfig) { Upload-Artifact $newConfig $serverConfig }
    Upload-Artifact $newExe $stageExe
    Upload-Artifact $newManifest $stageManifest
    Write-Output '中转发布包已上传并校验，服务云将短暂重启。'
    Restart-Backend
    Confirm-Remote 'netdisk.exe' $newExe
    if ((Get-SiteStatus '/api/windowsstream/relay') -ne 401 -or
        (Get-SiteStatus '/api/luke/remote/frame') -ne 401 -or (Get-SiteStatus '/api/info') -ne 200) {
        throw '中转、Luke 或服务云健康接口校验失败。'
    }
    Write-Output '服务器中转已上线，服务云和 Luke 认证入口已通过检查。'
} catch {
    $failure = $_
    if (!(Get-Status).restart.in_progress) {
        Upload-Artifact $oldExe $stageExe
        Upload-Artifact $oldManifest $stageManifest
        if ((Get-RemoteHash 'netdisk.exe') -ne (Get-LocalHash $oldExe)) {
            Restart-Backend
            Confirm-Remote 'netdisk.exe' $oldExe
        }
        if (!$existingConfig -and (Get-RemoteHash $serverConfig) -eq (Get-LocalHash $newConfig)) {
            Invoke-WebRequest "$base/path?path=$([uri]::EscapeDataString($serverConfig))" -Proxy $proxy -Method Delete -TimeoutSec 12 | Out-Null
        }
        Write-Output '已恢复发布前的服务云版本。'
    }
    throw $failure
}
