param([Parameter(Mandatory)][int]$ProcessId)
$ErrorActionPreference = 'Stop'
# Only close the process created by the caller's smoke test, including hidden
# Qt windows. WM_CLOSE lets PCSX2 shut down the VM and flush its logs normally.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class GuestSmokeClose {
    public delegate bool EnumProc(IntPtr window, IntPtr data);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc callback, IntPtr data);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
    public static void Close(uint process) {
        EnumWindows((window, data) => {
            uint owner; GetWindowThreadProcessId(window, out owner);
            if (owner == process) PostMessage(window, 0x0010, IntPtr.Zero, IntPtr.Zero);
            return true;
        }, IntPtr.Zero);
    }
}
'@
$process = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
if ($process) {
    [GuestSmokeClose]::Close($ProcessId)
    if (!$process.WaitForExit(30000)) {
        throw "Test process $ProcessId did not shut down within thirty seconds."
    }
}
