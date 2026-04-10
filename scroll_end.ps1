Add-Type @"
using System;
using System.Runtime.InteropServices;
public class WinInput {
    [DllImport("user32.dll")] public static extern IntPtr FindWindow(string cls, string wnd);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern void keybd_event(byte bVk, byte bScan, int dwFlags, int dwExtraInfo);
}
"@
$h = [WinInput]::FindWindow([NullString]::Value, "Scratch Browser")
if ($h -eq [IntPtr]::Zero) { Write-Host "Window not found"; exit 1 }
[WinInput]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 300
# Press End key to scroll to bottom
[WinInput]::keybd_event(0x23, 0, 0, 0) # End key down
Start-Sleep -Milliseconds 50
[WinInput]::keybd_event(0x23, 0, 2, 0) # End key up
Start-Sleep -Milliseconds 500
Write-Host "Pressed End key"
