<#
.SYNOPSIS
    Sends test events to Splunk HEC exactly the way the Satisfactory Splunk
    Exporter mod does (same headers, same batch format), so you can prove the
    Splunk side works BEFORE launching the game.

.EXAMPLE
    .\Test-SplunkHec.ps1 -Url http://192.168.1.50:8088/services/collector/event -Token 1234abcd-...

.EXAMPLE
    # Read URL/token from the mod's config file instead
    .\Test-SplunkHec.ps1 -ConfigPath "C:\Program Files (x86)\Steam\steamapps\common\Satisfactory\FactoryGame\Configs\SatisfactorySplunkMod.ini"
#>
param(
    [string]$Url,
    [string]$Token,
    [string]$Index = "",
    [string]$ConfigPath
)

if ($ConfigPath) {
    if (-not (Test-Path $ConfigPath)) { Write-Error "Config file not found: $ConfigPath"; exit 1 }
    foreach ($line in Get-Content $ConfigPath) {
        $l = $line.Trim()
        if ($l -match '^(SplunkURL|HECToken|Index)\s*=\s*(.*)$') {
            switch ($Matches[1]) {
                'SplunkURL' { if (-not $Url)   { $Url   = $Matches[2].Trim('"') } }
                'HECToken'  { if (-not $Token) { $Token = $Matches[2].Trim('"') } }
                'Index'     { if (-not $Index) { $Index = $Matches[2].Trim('"') } }
            }
        }
    }
}

if (-not $Url -or -not $Token) { Write-Error "Provide -Url and -Token, or -ConfigPath"; exit 1 }
if ($Url -match 'your-splunk' -or $Token -match 'your-hec-token') { Write-Error "URL/token are still the placeholder values"; exit 1 }

$now = [int][double]::Parse((Get-Date -UFormat %s))
function New-Event($sourcetype, $data) {
    $e = [ordered]@{ time = $now; host = $env:COMPUTERNAME; source = "satisfactory-splunk-mod"; sourcetype = $sourcetype; event = $data }
    if ($Index) { $e.index = $Index }
    return ($e | ConvertTo-Json -Compress -Depth 5)
}

# Two events in one request, newline-separated - the same batch format the mod uses.
$body = (New-Event "satisfactory:status" @{ message = "HEC connectivity test from Test-SplunkHec.ps1" }) + "`n" +
        (New-Event "satisfactory:summary" @{ manufacturers = 0; power_produced_mw = 0; test = $true }) + "`n"

Write-Host "POST $Url"
try {
    $resp = Invoke-WebRequest -Uri $Url -Method Post -Body $body -ContentType "application/json" `
        -Headers @{ Authorization = "Splunk $Token" } -UseBasicParsing -TimeoutSec 15
    Write-Host "HTTP $($resp.StatusCode): $($resp.Content)" -ForegroundColor Green
    Write-Host "Success. In Splunk search:  index=* sourcetype=satisfactory:status" -ForegroundColor Green
}
catch {
    $code = $null
    if ($_.Exception.Response) { $code = [int]$_.Exception.Response.StatusCode }
    if ($code) {
        Write-Host "HTTP $code" -ForegroundColor Red
        switch ($code) {
            400 { Write-Host "Bad request - usually the Index isn't allowed for this token." }
            401 { Write-Host "Token rejected - check HECToken." }
            403 { Write-Host "Token disabled or forbidden - check the token in Splunk." }
            404 { Write-Host "Wrong path - URL must end in /services/collector/event" }
            503 { Write-Host "HEC is disabled (Global Settings) or Splunk is busy." }
        }
    } else {
        Write-Host "Could not connect: $($_.Exception.Message)" -ForegroundColor Red
        if ($Url -like 'https*') {
            Write-Host "Using https:// - if Splunk has its default self-signed certificate, the GAME will reject it too."
            Write-Host "Disable SSL in HEC Global Settings and use http://, or install a trusted certificate."
        } else {
            Write-Host "Check the host/port, that HEC is enabled, and the Windows/Splunk firewall."
        }
    }
    exit 1
}
