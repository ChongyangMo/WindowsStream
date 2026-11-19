param(
    [Parameter(Mandatory)][string]$Source,
    [Parameter(Mandatory)][string]$Destination
)

$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

public static class WindowsStreamVersionResource
{
    private static readonly IntPtr VersionType = new IntPtr(16);
    private static readonly IntPtr VersionName = new IntPtr(1);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr LoadLibraryEx(string path, IntPtr file, uint flags);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool FreeLibrary(IntPtr module);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr FindResource(IntPtr module, IntPtr name, IntPtr type);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint SizeofResource(IntPtr module, IntPtr resource);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr LoadResource(IntPtr module, IntPtr resource);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr LockResource(IntPtr resource);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr BeginUpdateResource(string path, bool deleteExisting);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool UpdateResource(IntPtr update, IntPtr type, IntPtr name, ushort language, IntPtr data, uint size);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool EndUpdateResource(IntPtr update, bool discard);

    public static void Copy(string source, string destination)
    {
        var module = LoadLibraryEx(source, IntPtr.Zero, 0x22);
        if (module == IntPtr.Zero) throw new Win32Exception();
        try
        {
            var resource = FindResource(module, VersionName, VersionType);
            if (resource == IntPtr.Zero) throw new Win32Exception();
            var size = SizeofResource(module, resource);
            var loaded = LoadResource(module, resource);
            var bytes = loaded == IntPtr.Zero ? IntPtr.Zero : LockResource(loaded);
            if (size == 0 || bytes == IntPtr.Zero) throw new Win32Exception();
            var update = BeginUpdateResource(destination, false);
            if (update == IntPtr.Zero) throw new Win32Exception();
            if (!UpdateResource(update, VersionType, VersionName, 0x0409, bytes, size))
            {
                var error = new Win32Exception();
                EndUpdateResource(update, true);
                throw error;
            }
            if (!EndUpdateResource(update, false)) throw new Win32Exception();
        }
        finally
        {
            FreeLibrary(module);
        }
    }
}
'@

try {
    [WindowsStreamVersionResource]::Copy($Source, $Destination)
} catch {
    throw ('写入 WindowsStream 发布版号失败：' + $_.Exception.Message)
}
