param(
    [string]$Prefix = "shot",
    [string]$OutDir = ".",
    [int]$IntervalSec = 10,
    [int]$Count = 8,
    [string]$WindowClass = "TD5RE_Window",
    [string]$WindowTitle = ""
)

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$sig = @"
using System;
using System.Runtime.InteropServices;
public class W32 {
    [DllImport("user32.dll", CharSet = CharSet.Auto, SetLastError = true)]
    public static extern IntPtr FindWindow(string lpClassName, string lpWindowName);
    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    [DllImport("user32.dll")]
    public static extern bool BringWindowToTop(IntPtr hWnd);
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
Add-Type -TypeDefinition $sig -ErrorAction SilentlyContinue

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$cls   = if ([string]::IsNullOrEmpty($WindowClass)) { $null } else { $WindowClass }
$title = if ([string]::IsNullOrEmpty($WindowTitle)) { $null } else { $WindowTitle }

for ($i = 0; $i -lt $Count; $i++) {
    Start-Sleep -Seconds $IntervalSec

    $hwnd = [W32]::FindWindow($cls, $title)
    if ($hwnd -eq [IntPtr]::Zero) {
        Write-Host "[$i] window not found (cls=$cls title=$title), capturing primary"
        $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    } else {
        [W32]::ShowWindow($hwnd, 9) | Out-Null   # SW_RESTORE
        [W32]::BringWindowToTop($hwnd) | Out-Null
        [W32]::SetForegroundWindow($hwnd) | Out-Null
        Start-Sleep -Milliseconds 200
        $r = New-Object W32+RECT
        [W32]::GetWindowRect($hwnd, [ref]$r) | Out-Null
        $bounds = New-Object System.Drawing.Rectangle $r.Left, $r.Top, ($r.Right - $r.Left), ($r.Bottom - $r.Top)
    }

    $bmp = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
    $stamp = "{0:D2}" -f $i
    $path = Join-Path $OutDir ("{0}_{1}.png" -f $Prefix, $stamp)
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose()
    $bmp.Dispose()
    Write-Host "saved $path ($($bounds.Width)x$($bounds.Height))"
}
