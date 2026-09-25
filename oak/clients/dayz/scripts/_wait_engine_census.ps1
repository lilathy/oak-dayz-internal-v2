$log = Join-Path $env:LOCALAPPDATA "DayZ\oak_imgui.log"
Write-Host "log=$log"
$deadline = (Get-Date).AddSeconds(90)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 5
    if (-not (Test-Path $log)) { continue }
    $text = Get-Content $log -Raw -ErrorAction SilentlyContinue
    if ($text -match "engine\[census\] end") {
        Write-Host "FOUND census end"
        break
    }
    if ($text -match "engine: init") {
        Write-Host "engine init seen; waiting for world/local..."
    }
}
Write-Host "=== engine lines ==="
Get-Content $log -Tail 5000 | Where-Object { $_ -like "engine*" } | Select-Object -Last 50
Write-Host "alive=$(([bool](Get-Process DayZ_x64 -EA SilentlyContinue)))"
