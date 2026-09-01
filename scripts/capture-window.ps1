param(
    [string]$Out = 'out\window_topmost_capture.png'
)

$ErrorActionPreference = 'Stop'

function Fail([string]$Reason) {
    [Console]::Error.WriteLine("capture-window: FAIL: $Reason")
    exit 1
}

try {
    Add-Type -AssemblyName System.Drawing
    Add-Type @'
using System;
using System.Runtime.InteropServices;

public static class ReRevvedWindowCapture {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [DllImport("user32.dll")]
    public static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool SetWindowPos(IntPtr hwnd, IntPtr insertAfter, int x, int y,
        int width, int height, uint flags);
}
'@

    $hwnd = [ReRevvedWindowCapture]::GetForegroundWindow()
    if ($hwnd -eq [IntPtr]::Zero) {
        Fail 'no foreground window is available'
    }

    $topmost = [IntPtr](-1)
    $notTopmost = [IntPtr](-2)
    [uint32]$flags = 0x43
    if (-not [ReRevvedWindowCapture]::SetWindowPos($hwnd, $topmost, 0, 0, 0, 0, $flags)) {
        throw "SetWindowPos failed with Win32 error $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    }

    try {
        $rect = New-Object ReRevvedWindowCapture+RECT
        if (-not [ReRevvedWindowCapture]::GetWindowRect($hwnd, [ref]$rect)) {
            throw "GetWindowRect failed with Win32 error $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
        }
        $width = $rect.Right - $rect.Left
        $height = $rect.Bottom - $rect.Top
        if ($width -le 0 -or $height -le 0) {
            throw "foreground window has invalid dimensions ${width}x${height}"
        }

        $outPath = [IO.Path]::GetFullPath($Out)
        $parent = [IO.Path]::GetDirectoryName($outPath)
        if ($parent -and -not [IO.Directory]::Exists($parent)) {
            [IO.Directory]::CreateDirectory($parent) | Out-Null
        }

        $bitmap = New-Object System.Drawing.Bitmap($width, $height, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        try {
            $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0,
                (New-Object System.Drawing.Size($width, $height)), [System.Drawing.CopyPixelOperation]::SourceCopy)
            $bitmap.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)
        }
        finally {
            $graphics.Dispose()
            $bitmap.Dispose()
        }
    }
    finally {
        [ReRevvedWindowCapture]::SetWindowPos($hwnd, $notTopmost, 0, 0, 0, 0, $flags) | Out-Null
    }

    [Console]::WriteLine("capture-window: PASS: $outPath")
}
catch {
    Fail $_.Exception.Message
}
