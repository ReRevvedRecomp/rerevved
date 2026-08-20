param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$A,

    [Parameter(Mandatory = $true, Position = 1)]
    [string]$B,

    [string]$CropA,
    [string]$CropB,
    [double]$MeanAbsMax,
    [double]$Ssim
)

$ErrorActionPreference = 'Stop'

function Fail([string]$Reason) {
    [Console]::Error.WriteLine("compare-frames: FAIL: $Reason")
    exit 1
}

function Parse-Crop([string]$Value, [int]$Width, [int]$Height, [string]$Name) {
    if ([string]::IsNullOrWhiteSpace($Value)) {
        return [System.Drawing.Rectangle]::FromLTRB(0, 0, $Width, $Height)
    }

    $parts = $Value.Split(',')
    if ($parts.Count -ne 4) {
        throw "$Name must be x,y,w,h"
    }

    $numbers = New-Object int[] 4
    for ($i = 0; $i -lt 4; ++$i) {
        [int]$number = 0
        if (-not [int]::TryParse($parts[$i], [ref]$number)) {
            throw "$Name must contain four integers"
        }
        $numbers[$i] = $number
    }

    $rect = [System.Drawing.Rectangle]::FromLTRB($numbers[0], $numbers[1],
        $numbers[0] + $numbers[2], $numbers[1] + $numbers[3])
    if ($rect.X -lt 0 -or $rect.Y -lt 0 -or $rect.Width -le 0 -or $rect.Height -le 0 -or
        $rect.Right -gt $Width -or $rect.Bottom -gt $Height) {
        throw "$Name is outside the image bounds"
    }
    return $rect
}

function Copy-Crop([System.Drawing.Bitmap]$Source, [System.Drawing.Rectangle]$Rect) {
    $copy = New-Object System.Drawing.Bitmap($Rect.Width, $Rect.Height, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($copy)
    try {
        $graphics.DrawImage($Source, ([System.Drawing.Rectangle]::FromLTRB(0, 0, $Rect.Width, $Rect.Height)),
            $Rect, [System.Drawing.GraphicsUnit]::Pixel)
    }
    finally {
        $graphics.Dispose()
    }
    return $copy
}

function Get-Pixels([System.Drawing.Bitmap]$Bitmap) {
    $rect = [System.Drawing.Rectangle]::FromLTRB(0, 0, $Bitmap.Width, $Bitmap.Height)
    $data = $Bitmap.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    try {
        $bytes = New-Object byte[] ([Math]::Abs($data.Stride) * $Bitmap.Height)
        [Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
        return [PSCustomObject]@{ Bytes = $bytes; Stride = [Math]::Abs($data.Stride) }
    }
    finally {
        $Bitmap.UnlockBits($data)
    }
}

try {
    Add-Type -AssemblyName System.Drawing

    $pathA = (Resolve-Path -LiteralPath $A).Path
    $pathB = (Resolve-Path -LiteralPath $B).Path
    $sourceA = New-Object System.Drawing.Bitmap($pathA)
    $sourceB = New-Object System.Drawing.Bitmap($pathB)
    try {
        $rectA = Parse-Crop $CropA $sourceA.Width $sourceA.Height 'CropA'
        $rectB = Parse-Crop $CropB $sourceB.Width $sourceB.Height 'CropB'
        if ($rectA.Width -ne $rectB.Width -or $rectA.Height -ne $rectB.Height) {
            Fail "comparison regions differ in size ($($rectA.Width)x$($rectA.Height) vs $($rectB.Width)x$($rectB.Height))"
        }

        $bitmapA = Copy-Crop $sourceA $rectA
        $bitmapB = Copy-Crop $sourceB $rectB
        try {
            $pixelsA = Get-Pixels $bitmapA
            $pixelsB = Get-Pixels $bitmapB
            [double]$absSum = 0
            [double]$sumA = 0
            [double]$sumB = 0
            [double]$sumAA = 0
            [double]$sumBB = 0
            [double]$sumAB = 0
            [long]$count = [long]$bitmapA.Width * $bitmapA.Height

            for ($y = 0; $y -lt $bitmapA.Height; ++$y) {
                $rowA = $y * $pixelsA.Stride
                $rowB = $y * $pixelsB.Stride
                for ($x = 0; $x -lt $bitmapA.Width; ++$x) {
                    $offsetA = $rowA + $x * 4
                    $offsetB = $rowB + $x * 4
                    [double]$blueA = $pixelsA.Bytes[$offsetA]
                    [double]$greenA = $pixelsA.Bytes[$offsetA + 1]
                    [double]$redA = $pixelsA.Bytes[$offsetA + 2]
                    [double]$blueB = $pixelsB.Bytes[$offsetB]
                    [double]$greenB = $pixelsB.Bytes[$offsetB + 1]
                    [double]$redB = $pixelsB.Bytes[$offsetB + 2]
                    $absSum += [Math]::Abs($redA - $redB) + [Math]::Abs($greenA - $greenB) + [Math]::Abs($blueA - $blueB)

                    [double]$lumA = 0.2126 * $redA + 0.7152 * $greenA + 0.0722 * $blueA
                    [double]$lumB = 0.2126 * $redB + 0.7152 * $greenB + 0.0722 * $blueB
                    $sumA += $lumA
                    $sumB += $lumB
                    $sumAA += $lumA * $lumA
                    $sumBB += $lumB * $lumB
                    $sumAB += $lumA * $lumB
                }
            }

            [double]$meanAbs = $absSum / ($count * 3.0)
            [double]$meanA = $sumA / $count
            [double]$meanB = $sumB / $count
            [double]$varA = [Math]::Max(0.0, $sumAA / $count - $meanA * $meanA)
            [double]$varB = [Math]::Max(0.0, $sumBB / $count - $meanB * $meanB)
            [double]$covar = $sumAB / $count - $meanA * $meanB
            [double]$c1 = [Math]::Pow(0.01 * 255.0, 2)
            [double]$c2 = [Math]::Pow(0.03 * 255.0, 2)
            [double]$ssimValue = ((2.0 * $meanA * $meanB + $c1) * (2.0 * $covar + $c2)) /
                (($meanA * $meanA + $meanB * $meanB + $c1) * ($varA + $varB + $c2))

            $checkMean = $PSBoundParameters.ContainsKey('MeanAbsMax') -or -not $PSBoundParameters.ContainsKey('Ssim')
            [double]$meanLimit = if ($PSBoundParameters.ContainsKey('MeanAbsMax')) { $MeanAbsMax } else { 0.0 }
            if ($checkMean -and $meanLimit -lt 0.0) {
                Fail 'MeanAbsMax must be non-negative'
            }
            if ($PSBoundParameters.ContainsKey('Ssim') -and ($Ssim -lt -1.0 -or $Ssim -gt 1.0)) {
                Fail 'Ssim must be between -1 and 1'
            }
            if ($checkMean -and $meanAbs -gt $meanLimit) {
                Fail ("mean absolute error {0:F6} exceeds {1:F6}" -f $meanAbs, $meanLimit)
            }
            if ($PSBoundParameters.ContainsKey('Ssim') -and $ssimValue -lt $Ssim) {
                Fail ("SSIM {0:F6} is below {1:F6}" -f $ssimValue, $Ssim)
            }

            [Console]::WriteLine(("compare-frames: PASS: mean_abs={0:F6} ssim={1:F6}" -f $meanAbs, $ssimValue))
        }
        finally {
            $bitmapA.Dispose()
            $bitmapB.Dispose()
        }
    }
    finally {
        $sourceA.Dispose()
        $sourceB.Dispose()
    }
}
catch {
    Fail $_.Exception.Message
}
