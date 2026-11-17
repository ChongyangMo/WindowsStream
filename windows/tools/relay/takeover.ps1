Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class WindowsStreamTakeover
{
    private delegate bool VisitWindow(IntPtr window, IntPtr context);
    [DllImport("user32.dll")]
    private static extern bool EnumWindows(VisitWindow visit, IntPtr context);
    [DllImport("user32.dll")]
    private static extern bool EnumChildWindows(IntPtr parent, VisitWindow visit, IntPtr context);
    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetClassName(IntPtr window, StringBuilder name, int length);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool WritePrivateProfileString(string section, string key, string value, string file);

    public static void RequestExit(uint processId, bool host)
    {
        VisitWindow close = (window, context) =>
        {
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            if (owner != processId) return true;
            var name = new StringBuilder(128);
            GetClassName(window, name, name.Capacity);
            var windowClass = name.ToString();
            if (host && windowClass == "WindowsStreamHostWindow")
                PostMessage(window, 0x0111, new IntPtr(41002), IntPtr.Zero);
            if (!host && (windowClass == "WindowsStreamWorkspace" ||
                          windowClass.StartsWith("WindowsStreamWindow", StringComparison.Ordinal)))
                PostMessage(window, 0x0010, IntPtr.Zero, IntPtr.Zero);
            return true;
        };
        EnumWindows((window, context) =>
        {
            if (!host) EnumChildWindows(window, close, IntPtr.Zero);
            return close(window, context);
        }, IntPtr.Zero);
    }
}
'@

$streamSession = [Diagnostics.Process]::GetCurrentProcess().SessionId
$streamUser = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value

function Invoke-StreamLock([string]$Name, [scriptblock]$Action) {
    $mutex = [Threading.Mutex]::new($false, $Name)
    $locked = $false
    try {
        try { $locked = $mutex.WaitOne(10000) }
        catch [Threading.AbandonedMutexException] { $locked = $true }
        if (!$locked) { throw '另一个 WindowsStream 实例正在启动或更新，请稍后重试。' }
        & $Action
    } finally {
        if ($locked) { $mutex.ReleaseMutex() }
        $mutex.Dispose()
    }
}

function Get-StreamProcesses([string]$Name) {
    $instances = Get-CimInstance Win32_Process -Filter "Name='$Name' AND SessionId=$streamSession"
    foreach ($instance in $instances) {
        try {
            $owner = Invoke-CimMethod -InputObject $instance -MethodName GetOwnerSid
            if ($owner.ReturnValue -ne 0 -or $owner.Sid -ne $streamUser) { continue }
            $process = [Diagnostics.Process]::GetProcessById($instance.ProcessId)
            $null = $process.Handle
            if ([Math]::Abs(($process.StartTime - $instance.CreationDate).TotalMilliseconds) -gt 1) {
                $process.Dispose()
                continue
            }
        } catch { continue }
        [pscustomobject]@{ Instance = $instance; Process = $process }
    }
}

function Wait-StreamExit($Entries, [int]$Milliseconds) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    foreach ($entry in $Entries) {
        $remaining = [Math]::Max(0, $Milliseconds - [int]$timer.ElapsedMilliseconds)
        if (!$entry.Process.WaitForExit($remaining)) { return $false }
    }
    return $true
}

function Stop-StreamProcesses($Entries, [ValidateSet('host','viewer','bridge')][string]$Mode) {
    try {
        if ($Mode -ne 'bridge') {
            foreach ($entry in $Entries) {
                if (!$entry.Process.HasExited) {
                    [WindowsStreamTakeover]::RequestExit($entry.Process.Id, $Mode -eq 'host')
                }
            }
            if (Wait-StreamExit $Entries 3500) { return }
        }
        foreach ($entry in $Entries) {
            if ($entry.Process.HasExited) { continue }
            try { $entry.Process.Kill() }
            catch [InvalidOperationException] { if (!$entry.Process.HasExited) { throw } }
        }
        if (!(Wait-StreamExit $Entries 5000)) { throw '旧版 WindowsStream 未能退出。' }
    } finally {
        foreach ($entry in $Entries) { $entry.Process.Dispose() }
    }
}

function Get-BridgeIdentity([string]$Program) {
    $ini = Join-Path (Split-Path -Parent $Program) 'relay.ini'
    if (!(Test-Path -LiteralPath $ini -PathType Leaf)) { return }
    $values = @{}
    foreach ($line in [IO.File]::ReadLines($ini)) {
        if ($line -match '^\s*(role|local_address)\s*=\s*(\S+)\s*$') { $values[$Matches[1]] = $Matches[2] }
    }
    if ($values.role -and $values.local_address) { return $values.role + '/' + $values.local_address }
}

function Stop-OldBridges([string]$Program) {
    $identity = Get-BridgeIdentity $Program
    $matching = @(foreach ($entry in Get-StreamProcesses 'WindowsStreamBridge.exe') {
        $path = $entry.Instance.ExecutablePath
        if ($path -and ($path -eq $Program -or ($identity -and (Get-BridgeIdentity $path) -eq $identity))) {
            $entry
        } else {
            $entry.Process.Dispose()
        }
    })
    Stop-StreamProcesses $matching 'bridge'
}

function Assert-StreamFiles([string[]]$Paths) {
    foreach ($path in $Paths) {
        if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw ('WindowsStream 文件缺失：' + (Split-Path -Leaf $path)) }
    }
}
