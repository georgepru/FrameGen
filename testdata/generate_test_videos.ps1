# generate_test_videos.ps1
# Generates synthetic 10-second benchmark videos using FFmpeg.
# Requires ffmpeg.exe to be on PATH or in the same directory.
#
# Output files (created in this script's directory):
#   720p_test.mp4   — 1280x720  @ 30 fps, 10 seconds
#   1080p_test.mp4  — 1920x1080 @ 30 fps, 10 seconds
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File generate_test_videos.ps1

$dir = $PSScriptRoot

function Find-FFmpeg {
    $local = Join-Path $dir "ffmpeg.exe"
    if (Test-Path $local) { return $local }
    $found = Get-Command ffmpeg -ErrorAction SilentlyContinue
    if ($found) { return $found.Source }
    return $null
}

$ffmpeg = Find-FFmpeg
if (-not $ffmpeg) {
    Write-Host "ERROR: ffmpeg.exe not found." -ForegroundColor Red
    Write-Host "Download from https://ffmpeg.org/download.html and place ffmpeg.exe"
    Write-Host "in this folder or add it to your PATH, then re-run this script."
    exit 1
}

Write-Host "Using ffmpeg: $ffmpeg" -ForegroundColor Cyan

$videos = @(
    @{ Name = "720p_test.mp4";  Size = "1280x720" },
    @{ Name = "1080p_test.mp4"; Size = "1920x1080" }
)

foreach ($v in $videos) {
    $out = Join-Path $dir $v.Name
    if (Test-Path $out) {
        Write-Host "Already exists, skipping: $($v.Name)" -ForegroundColor Yellow
        continue
    }
    Write-Host "Generating $($v.Name) ($($v.Size)) ..." -ForegroundColor Green
    # testsrc2 gives a colourful moving pattern — varied content exercises RIFE
    & $ffmpeg -y `
        -f lavfi -i "testsrc2=size=$($v.Size):rate=30" `
        -t 10 `
        -c:v libx264 -preset ultrafast -crf 18 `
        -pix_fmt yuv420p `
        $out
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR generating $($v.Name)" -ForegroundColor Red
        exit 1
    }
    Write-Host "  -> $out" -ForegroundColor Green
}

Write-Host ""
Write-Host "Done. Test videos are ready in: $dir" -ForegroundColor Cyan
