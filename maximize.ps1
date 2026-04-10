Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class WinAPI {
    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
}
"@
$p = Get-Process BrowserEngine -ErrorAction SilentlyContinue
if ($p) {
    [WinAPI]::ShowWindow($p[0].MainWindowHandle, 3) | Out-Null
    Write-Host "Maximized"
} else {
    Write-Host "No BrowserEngine process found"
}
