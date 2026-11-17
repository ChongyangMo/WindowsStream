param([Parameter(Mandatory)][string]$Output)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'pairing.ps1')
$outputFile = [IO.Path]::GetFullPath($Output)
$directory = Split-Path -Parent $outputFile
$goRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../../relay')).Path
$go = (Read-RelaySection (Join-Path $goRoot '../config/build.ini') 'tools').go
if (!$go -or !(Test-Path -LiteralPath $go -PathType Leaf)) {
    throw '请在 windows\config\build.ini 中配置 Go 可执行文件。'
}
$null = New-Item -ItemType Directory -Path $directory -Force
Push-Location -LiteralPath $goRoot
try {
    & $go build -buildvcs=false -trimpath -mod=readonly -o $outputFile .
    if ($LASTEXITCODE -ne 0) { throw '中转组件构建失败。' }
} finally { Pop-Location }
if ((Split-Path -Leaf $directory) -eq '.runtime') {
    $runtime = Get-Item -LiteralPath $directory -Force
    $runtime.Attributes = $runtime.Attributes -bor [IO.FileAttributes]::Hidden
}
