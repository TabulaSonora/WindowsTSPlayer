<#
.SYNOPSIS
    Generates the MSIX tile and logo assets from the shared application icon.

.DESCRIPTION
    The three front ends share one icon, and it is committed as PNGs rather than as the 2048x2048
    master (see LinuxTSPlayer's tools/build-icons.sh, which says so). This script is the Windows
    counterpart: it takes the largest committed size and produces the handful of assets
    Package.appxmanifest names.

    Generated rather than committed, for the same reason the props file at the CMake/MSBuild seam is
    generated: an asset checked in beside the icon it came from is a copy that can silently fall out
    of date, and the failure -- a stale tile -- is one nobody notices for months.

    A caveat worth knowing before this is trusted for the Store. The largest committed source is
    512px, and Square150x150Logo at 400% scale wants 600px, so that one tile is upscaled. It is
    fine for development and for sideloading; a Store submission wants the master re-exported at
    600px or larger instead. This is called out in the plan rather than hidden here.

.PARAMETER Source
    The source PNG. Defaults to the 512px icon in a sibling LinuxTSPlayer checkout, which is where
    the shared icon actually lives.

.PARAMETER Destination
    Where to write. Defaults to src/app/Assets.
#>
[CmdletBinding()]
param(
    [string] $Source = (Join-Path $PSScriptRoot '..\..\LinuxTSPlayer\data\icons\hicolor\512x512\apps\co.losno.TabulaSonoraPlayer.png'),
    [string] $Destination = (Join-Path $PSScriptRoot '..\src\app\Assets')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Source)) {
    throw "Source icon not found: $Source`nPass -Source explicitly, or check out LinuxTSPlayer beside this repository."
}

Add-Type -AssemblyName System.Drawing

if (-not (Test-Path $Destination)) {
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
}

# Width, height, and the base name the manifest refers to. The square tiles scale the icon to fill;
# the two wide ones letterbox it centred on transparency, because stretching a square logo to 310x150
# is the one thing that makes a tile look broken rather than merely low-resolution.
$targets = @(
    @{ Name = 'Square44x44Logo';  W = 44;  H = 44  },
    @{ Name = 'Square150x150Logo'; W = 150; H = 150 },
    @{ Name = 'StoreLogo';        W = 50;  H = 50  },
    @{ Name = 'LockScreenLogo';   W = 24;  H = 24  },
    @{ Name = 'Wide310x150Logo';  W = 310; H = 150 },
    @{ Name = 'SplashScreen';     W = 620; H = 300 }
)

$src = [System.Drawing.Image]::FromFile((Resolve-Path $Source))
try {
    foreach ($t in $targets) {
        $bmp = New-Object System.Drawing.Bitmap($t.W, $t.H, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        try {
            $g.Clear([System.Drawing.Color]::Transparent)
            $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $g.PixelOffsetMode   = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $g.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality

            # Fit, never fill: preserve the aspect ratio and centre what is left over.
            $scale = [Math]::Min($t.W / $src.Width, $t.H / $src.Height)
            $w = [int][Math]::Round($src.Width * $scale)
            $h = [int][Math]::Round($src.Height * $scale)
            $x = [int][Math]::Round(($t.W - $w) / 2)
            $y = [int][Math]::Round(($t.H - $h) / 2)

            $g.DrawImage($src, $x, $y, $w, $h)
        }
        finally {
            $g.Dispose()
        }

        $out = Join-Path $Destination "$($t.Name).png"
        $bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
        $bmp.Dispose()
        Write-Host "  $($t.Name).png  $($t.W)x$($t.H)"
    }
}
finally {
    $src.Dispose()
}

Write-Host "Wrote $($targets.Count) assets to $Destination"
