<#
.SYNOPSIS
    Generates the MSIX tile and logo assets from the application icon master.

.DESCRIPTION
    The three front ends share one icon. The master is a 2048x2048 PNG with an alpha channel and it
    is **not committed** to any of the three repositories -- LinuxTSPlayer's tools/build-icons.sh
    says the same of its own outputs. Only the generated sizes are committed, and this script is what
    generates them for Windows.

    Generated rather than hand-exported, for the same reason the props file at the CMake/MSBuild seam
    is generated: an asset committed beside the icon it came from is a copy that can silently fall
    out of date, and a stale tile is a thing nobody notices for months.

    Every asset is emitted scale-qualified -- Square150x150Logo.scale-200.png rather than
    Square150x150Logo.png -- which is what the resource system actually wants. Package.appxmanifest
    keeps referring to the unqualified name; MRT resolves it to the right variant at runtime from the
    display's scale factor. That is why the manifest needs no change when this set grows.

    Square44x44Logo additionally gets targetsize variants, which are a different axis from scale and
    not interchangeable with it: scale-* is for tiles, targetsize-* is what the taskbar, the Start
    list and Explorer's "Open with" draw. The altform-unplated variants are the same images without
    the system's background plate, used where the shell composites the icon onto its own surface.

.PARAMETER Source
    The 2048x2048 master. Defaults to $env:TS_ICON_MASTER, then to the known location on this
    machine. Fails with a readable message rather than silently upscaling something smaller.

.PARAMETER Destination
    Where to write. Defaults to src/app/Assets.
#>
[CmdletBinding()]
param(
    [string] $Source,
    [string] $Destination = (Join-Path $PSScriptRoot '..\src\app\Assets')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $Source) {
    $candidates = @(
        $env:TS_ICON_MASTER,
        'D:\ts-icon\ts-iOS-Default-1024@2x.png'
    ) | Where-Object { $_ }

    $Source = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}

if (-not $Source -or -not (Test-Path $Source)) {
    throw @"
Icon master not found.

The master is a 2048x2048 PNG and is deliberately not committed. Point at it with

    -Source <path>

or set TS_ICON_MASTER. Anything smaller will upscale: Square150x150Logo at 400% needs 600px and
SplashScreen at 400% needs 1240px of width, both beyond the largest size any of the three
repositories commits.
"@
}

Add-Type -AssemblyName System.Drawing

$src = [System.Drawing.Image]::FromFile((Resolve-Path $Source))

if ($src.Width -lt 600 -or $src.Height -lt 600) {
    $src.Dispose()
    throw "Source is $($src.Width)x$($src.Height); at least 600x600 is needed for the 400% tile."
}

if (-not (Test-Path $Destination)) {
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
}

# Base sizes at 100%. The square tiles scale the icon to fill; the two wide ones letterbox it centred
# on transparency, because stretching a square logo to 310x150 is the one thing that makes a tile
# look broken rather than merely low-resolution.
$scaled = @(
    @{ Name = 'Square44x44Logo';   W = 44;  H = 44  },
    @{ Name = 'Square71x71Logo';   W = 71;  H = 71  },
    @{ Name = 'Square150x150Logo'; W = 150; H = 150 },
    @{ Name = 'Square310x310Logo'; W = 310; H = 310 },
    @{ Name = 'Wide310x150Logo';   W = 310; H = 150 },
    @{ Name = 'StoreLogo';         W = 50;  H = 50  },
    @{ Name = 'SplashScreen';      W = 620; H = 300 },
    @{ Name = 'LockScreenLogo';    W = 24;  H = 24  }
)

# The scale factors Windows actually ships displays at.
$scales = @(100, 125, 150, 200, 400)

# targetsize is the shell's axis, not the tile system's.
$targetSizes = @(16, 24, 32, 48, 256)

function Write-Asset {
    param(
        [System.Drawing.Image] $Image,
        [int] $Width,
        [int] $Height,
        [string] $Path
    )

    $bmp = New-Object System.Drawing.Bitmap($Width, $Height, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    try {
        $g.Clear([System.Drawing.Color]::Transparent)
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.PixelOffsetMode   = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $g.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality

        # Fit, never fill: preserve the aspect ratio and centre what is left over.
        $scale = [Math]::Min($Width / $Image.Width, $Height / $Image.Height)
        $w = [int][Math]::Round($Image.Width * $scale)
        $h = [int][Math]::Round($Image.Height * $scale)
        $g.DrawImage($Image, [int][Math]::Round(($Width - $w) / 2), [int][Math]::Round(($Height - $h) / 2), $w, $h)
    }
    finally {
        $g.Dispose()
    }

    $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

$count = 0
try {
    # Anything previously generated goes, so a renamed or dropped asset cannot linger in the package.
    Get-ChildItem $Destination -Filter '*.png' -ErrorAction SilentlyContinue | Remove-Item -Force

    foreach ($a in $scaled) {
        foreach ($s in $scales) {
            $w = [int][Math]::Round($a.W * $s / 100)
            $h = [int][Math]::Round($a.H * $s / 100)
            Write-Asset -Image $src -Width $w -Height $h -Path (Join-Path $Destination "$($a.Name).scale-$s.png")
            $count++
        }
    }

    foreach ($t in $targetSizes) {
        Write-Asset -Image $src -Width $t -Height $t -Path (Join-Path $Destination "Square44x44Logo.targetsize-$t.png")
        Write-Asset -Image $src -Width $t -Height $t -Path (Join-Path $Destination "Square44x44Logo.targetsize-$t`_altform-unplated.png")
        $count += 2
    }

    # ---------------------------------------------------------------------------------------------
    # The window icon, which the tiles above do not cover.
    #
    # A packaged app's taskbar and Start entries come from the manifest's logos, but the title bar,
    # the Alt-Tab card and the window's system menu take their icon from the HWND, and WinUI 3 never
    # sets one from the package. The result is a correct icon everywhere except the window itself.
    # AppWindow::SetIcon wants a real .ico file, so one is assembled here.
    #
    # Written by hand rather than through System.Drawing's Icon type, which cannot author a
    # multi-resolution icon at all. The container is simple: a six-byte header, a sixteen-byte
    # directory entry per image, then the images. The payloads are PNG rather than BMP, which every
    # Windows since Vista accepts and which keeps the alpha channel without the AND-mask dance.
    # ---------------------------------------------------------------------------------------------
    $icoSizes = @(16, 20, 24, 32, 40, 48, 64, 128, 256)
    $payloads = @()

    foreach ($s in $icoSizes) {
        $bmp = New-Object System.Drawing.Bitmap($s, $s, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        try {
            $g.Clear([System.Drawing.Color]::Transparent)
            $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $g.PixelOffsetMode   = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $g.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
            $g.DrawImage($src, 0, 0, $s, $s)
        }
        finally {
            $g.Dispose()
        }

        $ms = New-Object System.IO.MemoryStream
        $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        $bmp.Dispose()
        $payloads += , $ms.ToArray()
        $ms.Dispose()
    }

    $icoPath = Join-Path $Destination 'AppIcon.ico'
    $fs = [System.IO.File]::Create($icoPath)
    $bw = New-Object System.IO.BinaryWriter($fs)
    try {
        $bw.Write([UInt16]0)                    # reserved
        $bw.Write([UInt16]1)                    # type: icon
        $bw.Write([UInt16]$icoSizes.Count)

        # Images start after the header and the whole directory.
        $offset = 6 + (16 * $icoSizes.Count)

        for ($i = 0; $i -lt $icoSizes.Count; $i++) {
            $s = $icoSizes[$i]
            # 256 is encoded as zero; the field is one byte and 256 does not fit.
            $bw.Write([Byte]($(if ($s -ge 256) { 0 } else { $s })))
            $bw.Write([Byte]($(if ($s -ge 256) { 0 } else { $s })))
            $bw.Write([Byte]0)                  # palette entries: none, this is truecolour
            $bw.Write([Byte]0)                  # reserved
            $bw.Write([UInt16]1)                # colour planes
            $bw.Write([UInt16]32)               # bits per pixel
            $bw.Write([UInt32]$payloads[$i].Length)
            $bw.Write([UInt32]$offset)
            $offset += $payloads[$i].Length
        }

        foreach ($p in $payloads) {
            $bw.Write($p)
        }
    }
    finally {
        $bw.Dispose()
        $fs.Dispose()
    }

    $count++
    Write-Host "  AppIcon.ico  $($icoSizes -join ', ')"
}
finally {
    $src.Dispose()
}

Write-Host "Wrote $count assets to $Destination from $Source"
