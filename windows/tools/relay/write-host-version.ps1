param([Parameter(Mandatory)][string]$OutputPath)

$ErrorActionPreference = 'Stop'
$parts = [DateTime]::UtcNow.ToString('yy.MMdd.HHmm.ss', [Globalization.CultureInfo]::InvariantCulture).Split('.')
$version = $parts -join '.'
$numbers = ($parts | ForEach-Object { [int]$_ }) -join ','
$header = @"
#define WINDOWSSTREAM_HOST_VERSION_TEXT "$version"
#define WINDOWSSTREAM_HOST_VERSION_NUMBERS $numbers
"@
[IO.File]::WriteAllText($OutputPath, $header + [Environment]::NewLine, [Text.Encoding]::ASCII)
