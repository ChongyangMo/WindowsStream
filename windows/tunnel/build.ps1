param(
    [ValidateSet('Build', 'Rebuild', 'Clean')][string]$Action = 'Build',
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release'
)
$ErrorActionPreference = 'Stop'
$windowsRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$outputDirectory = Join-Path $windowsRoot "build\$Configuration\controller"
$outputFile = Join-Path $outputDirectory 'WindowsStreamTunnel.exe'

try {
    if ($Action -in 'Clean', 'Rebuild') {
        foreach ($file in @($outputFile, (Join-Path $outputDirectory 'wintun.dll'))) {
            if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file -Force }
        }
        if ($Action -eq 'Clean') { exit 0 }
    }

    $settingsFile = Join-Path $windowsRoot 'config\build.ini'
    $goExecutable = ''
    $section = ''
    foreach ($line in Get-Content -LiteralPath $settingsFile -Encoding UTF8) {
        if ($line -match '^\s*\[([^\]]+)\]') { $section = $Matches[1]; continue }
        if ($section -eq 'tools' -and $line -match '^\s*go\s*=\s*(.*?)\s*$') { $goExecutable = $Matches[1] }
    }
    if (-not (Test-Path -LiteralPath $goExecutable -PathType Leaf)) {
        throw '请在 windows\config\build.ini 中填写 Go 可执行文件的完整路径。'
    }
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
    Push-Location -LiteralPath $PSScriptRoot
    try {
        $arguments = @('build', '-trimpath', '-mod=readonly', '-o', $outputFile)
        if ($Configuration -eq 'Debug') { $arguments += '-gcflags=all=-N -l' }
        $arguments += '.'
        & $goExecutable @arguments
        if ($LASTEXITCODE -ne 0) { throw 'IP 隧道组件编译失败。' }
    } finally { Pop-Location }
    Copy-Item -LiteralPath (Join-Path $windowsRoot 'third_party\wintun\wintun.dll') -Destination $outputDirectory -Force
    Write-Host 'IP 隧道组件已编译；控制端仍保持原有的禁用策略。'
} catch {
    Write-Error $_.Exception.Message
    exit 1
}
