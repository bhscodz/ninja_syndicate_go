# inspect_assets.ps1
# Scans the Ninja Adventure asset pack, finds character spritesheets and tilesets,
# gets image dimensions, and copies a summary to the clipboard.

$assetRoot = "D:\cloud_computing_game\Ninja Adventure - Asset Pack"

if (-not (Test-Path $assetRoot)) {
    Write-Error "Asset root not found: $assetRoot"
    exit 1
}

function Get-ImageDimensions {
    param([string]$Path)
    try {
        Add-Type -AssemblyName System.Drawing -ErrorAction Stop
        $img = [System.Drawing.Image]::FromFile($Path)
        $w = $img.Width
        $h = $img.Height
        $img.Dispose()
        return "$w x $h"
    } catch {
        return "unknown"
    }
}

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("=== Ninja Adventure Asset Inspection ===")
$lines.Add("Asset root: $assetRoot")
$lines.Add("")

# --- 1. Human / Ninja character spritesheets ---
$lines.Add("--- Human / Ninja Character Spritesheets ---")
$humanSheets = Get-ChildItem -Path $assetRoot -Recurse -File -Filter "*.png" |
    Where-Object {
        $_.FullName -match "\\Actor\\" -and
        $_.Name -match "SpriteSheet" -and
        $_.FullName -match "Ninja|Human|Player|Hero|Warrior|Knight"
    }

if ($humanSheets.Count -eq 0) {
    $lines.Add("No explicit Ninja/Human match. Listing all non-animal actor spritesheets:")
    $fallback = Get-ChildItem -Path $assetRoot -Recurse -File -Filter "*.png" |
        Where-Object {
            $_.FullName -match "\\Actor\\" -and
            $_.Name -match "SpriteSheet" -and
            $_.FullName -notmatch "\\Animal\\"
        }
    if ($fallback.Count -eq 0) {
        $lines.Add("  (none found)")
    } else {
        foreach ($f in $fallback) {
            $dim = Get-ImageDimensions -Path $f.FullName
            $lines.Add("  $($f.FullName)  [$dim]")
        }
    }
} else {
    foreach ($f in $humanSheets) {
        $dim = Get-ImageDimensions -Path $f.FullName
        $lines.Add("  $($f.FullName)  [$dim]")
    }
}

$lines.Add("")

# --- 2. Tilesets / Terrain ---
$lines.Add("--- Tilesets / Terrain ---")
$tilesets = Get-ChildItem -Path $assetRoot -Recurse -File -Filter "*.png" |
    Where-Object {
        $_.Name -match "Tile|Tileset|Terrain|Map|Grass|Floor|Castle" -or
        $_.DirectoryName -match "Tileset|Tiles|Terrain|Map"
    }

if ($tilesets.Count -eq 0) {
    $lines.Add("No explicit tileset match. Listing large PNGs in root and Tileset folders:")
    $candidates = @()
    # Root PNGs larger than 30 KB
    $candidates += Get-ChildItem -Path $assetRoot -File -Filter "*.png" |
        Where-Object { $_.Length -gt 30KB }
    # PNGs inside folders named Tileset, Tiles, Terrain, Map
    $tileDirs = Get-ChildItem -Path $assetRoot -Directory -Recurse |
        Where-Object { $_.Name -match "Tileset|Tiles|Terrain|Map" }
    foreach ($d in $tileDirs) {
        $candidates += Get-ChildItem -Path $d.FullName -File -Filter "*.png"
    }
    $candidates = $candidates | Sort-Object FullName -Unique
    if ($candidates.Count -eq 0) {
        $lines.Add("  (none found)")
    } else {
        foreach ($f in $candidates) {
            $dim = Get-ImageDimensions -Path $f.FullName
            $lines.Add("  $($f.FullName)  [$dim]")
        }
    }
} else {
    foreach ($f in $tilesets) {
        $dim = Get-ImageDimensions -Path $f.FullName
        $lines.Add("  $($f.FullName)  [$dim]")
    }
}

$lines.Add("")

# --- 3. Top-level folders for context ---
$lines.Add("--- Top-level folders ---")
Get-ChildItem -Path $assetRoot -Directory | ForEach-Object {
    $lines.Add("  $($_.Name)")
}

$finalText = $lines -join "`r`n"
Set-Clipboard -Value $finalText
Write-Output $finalText