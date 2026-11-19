# Traegt den Geometry-Dash-MCP-Server in die Claude-Desktop-Konfiguration ein.
# Muss laufen, waehrend Claude Desktop GESCHLOSSEN ist (sonst ueberschreibt die App die Aenderung).

param([string]$TestConfig = "")

$ErrorActionPreference = "Stop"
$configPath = "$env:LOCALAPPDATA\Claude-Data\claude_pzs8sxrjxfjjc\claude_desktop_config.json"
$isTest = $TestConfig -ne ""
if ($isTest) { $configPath = $TestConfig }
$serverScript = Join-Path $PSScriptRoot "index.js"
$nodeExe = "C:\Program Files\nodejs\node.exe"

Write-Host ""
Write-Host "=== Geometry Dash -> Claude Desktop einrichten ===" -ForegroundColor Cyan
Write-Host ""

# 1. Warten, bis Claude komplett beendet ist
if (-not $isTest -and (Get-Process -Name "Claude" -ErrorAction SilentlyContinue)) {
    Write-Host "Claude Desktop laeuft noch." -ForegroundColor Yellow
    Write-Host "Bitte beenden: Rechtsklick auf das Claude-Symbol unten rechts in der Taskleiste -> Beenden."
    Write-Host "Ich warte ..."
    while (Get-Process -Name "Claude" -ErrorAction SilentlyContinue) { Start-Sleep -Seconds 1 }
    Start-Sleep -Seconds 2
}
Write-Host "Claude ist beendet." -ForegroundColor Green

# 2. Pruefen
if (-not (Test-Path $configPath)) { throw "Konfigurationsdatei nicht gefunden: $configPath" }
if (-not (Test-Path $nodeExe)) { throw "Node.js nicht gefunden: $nodeExe" }
if (-not (Test-Path $serverScript)) { throw "index.js nicht gefunden: $serverScript" }

$text = [IO.File]::ReadAllText($configPath)

if ($text -match '"geometry-dash"') {
    Write-Host "Der Eintrag ist schon vorhanden - nichts zu tun." -ForegroundColor Green
}
else {
    # 3. Sicherung
    Copy-Item $configPath "$configPath.backup-$(Get-Date -Format yyyyMMdd-HHmmss)"

    $esc = { param($s) $s.Replace('\', '\\') }
    $entry = '"geometry-dash": { "command": "' + (& $esc $nodeExe) + '", "args": ["' + (& $esc $serverScript) + '"] }'

    if ($text -match '"mcpServers"\s*:\s*\{') {
        # vorhandenen mcpServers-Block ergaenzen
        $text = [regex]::Replace($text, '"mcpServers"\s*:\s*\{', { param($m) $m.Value + "`n    " + $entry + "," }, 1)
        $text = [regex]::Replace($text, ',(\s*)\}', '$1}')  # falls der Block vorher leer war
    }
    else {
        $idx = $text.IndexOf('{')
        $text = $text.Substring(0, $idx + 1) + "`n  `"mcpServers`": {`n    " + $entry + "`n  }," + $text.Substring($idx + 1)
    }

    # Gueltigkeit pruefen, bevor geschrieben wird
    $null = $text | ConvertFrom-Json
    [IO.File]::WriteAllText($configPath, $text, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "Eintrag 'geometry-dash' wurde hinzugefuegt." -ForegroundColor Green
}

# 4. Claude wieder starten
if ($isTest) { return }
Write-Host "Starte Claude Desktop ..."
Start-Process "shell:AppsFolder\Claude_pzs8sxrjxfjjc!Claude"
Write-Host ""
Write-Host "Fertig! Oeffne in Claude einen NEUEN Chat und teste mit gd_ping." -ForegroundColor Cyan
Start-Sleep -Seconds 5
