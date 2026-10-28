# Offline descriptor regression. Never accesses Bluetooth.
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$bridge = Get-Content (Join-Path $root 'src/bridge.c') -Raw
$snapshot = Get-Content (Join-Path $PSScriptRoot 'reports.h') -Raw

function Read-Bytes([string]$text, [string]$name) {
    $match = [regex]::Match($text, "\b$name\[\]\s*=\s*\{([\s\S]*?)\};")
    if (-not $match.Success) { throw "Array not found: $name" }
    $body = [regex]::Replace($match.Groups[1].Value, '/\*[\s\S]*?\*/', '')
    return @([regex]::Matches($body, '0x[0-9a-fA-F]+') | ForEach-Object {
        [Convert]::ToInt32($_.Value.Substring(2), 16)
    })
}

function Assert-Layout($bytes, [int]$expectedInput, [int]$expectedOutput) {
    $size = 0
    $count = 0
    $inputBits = 0
    $outputBits = 0
    $collections = 0
    for ($i = 0; $i -lt $bytes.Count;) {
        $tag = $bytes[$i]
        $i++
        $length = $tag -band 3
        if ($length -eq 3) { $length = 4 }
        if ($i + $length -gt $bytes.Count) { throw 'Truncated HID item' }
        $value = 0
        for ($j = 0; $j -lt $length; $j++) {
            $value = $value -bor ($bytes[$i + $j] -shl (8 * $j))
        }
        switch ($tag -band 0xfc) {
            0x74 { $size = $value }
            0x94 { $count = $value }
            0x80 { $inputBits += $size * $count }
            0x90 { $outputBits += $size * $count }
            0xa0 { $collections++ }
            0xc0 { $collections-- }
        }
        if ($collections -lt 0) { throw 'Unbalanced HID collection' }
        $i += $length
    }
    if ($collections -ne 0 -or $inputBits -ne $expectedInput -or $outputBits -ne $expectedOutput) {
        throw "Invalid layout: collections=$collections inputBits=$inputBits outputBits=$outputBits"
    }
}

foreach ($pair in @(
    @('KEYBOARD_REPORT_DESC_FULL', 'keyboard_desc', 64, 8),
    @('ABSOLUTE_MOUSE_REPORT_DESC', 'pointer_desc', 64, 0)
)) {
    $original = Read-Bytes $bridge $pair[0]
    $copy = Read-Bytes $snapshot $pair[1]
    if (($original -join ',') -cne ($copy -join ',')) {
        throw "Descriptor drift: $($pair[1]) differs from src/bridge.c"
    }
    Assert-Layout $copy $pair[2] $pair[3]
    Write-Output "PASS: $($pair[1]) matches bridge.c; HID input/output bit lengths and collections valid"
}
