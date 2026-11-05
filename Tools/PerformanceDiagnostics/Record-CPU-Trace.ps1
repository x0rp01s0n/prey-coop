$ErrorActionPreference = 'Stop'
$started = $false
try {
    $wpr = Join-Path $env:WINDIR 'System32\wpr.exe'
    if (-not (Test-Path -LiteralPath $wpr)) { throw 'Windows Performance Recorder is unavailable. Skip this optional trace.' }
    $root = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'PreyCoop-Diagnostics'
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    $trace = Join-Path $root ('cpu-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.etl')
    Write-Host 'This optional 60-second trace records CPU activity system-wide.'
    Write-Host 'It can contain process names, paths and other private metadata.'
    Write-Host 'Only record while reproducing the lag; share the ETL privately, not on GitHub.'
    Read-Host 'Press Enter to start, then return to Prey and reproduce the slowdown' | Out-Null
    & $wpr -start CPU -filemode
    if ($LASTEXITCODE -ne 0) { throw 'Could not start recording. An existing WPR recording will not be cancelled.' }
    $started = $true
    Write-Host 'Recording for 60 seconds. Return to Prey now.'
    Start-Sleep -Seconds 60
    & $wpr -stop $trace
    if ($LASTEXITCODE -ne 0) { throw 'Could not save the CPU trace. WPR may need manual attention; no other recording was cancelled.' }
    $started = $false
    Write-Host "Saved: $trace"
    Write-Host 'This ETL is not included by Collect-Diagnostics.cmd. Share it privately if requested.'
} catch {
    Write-Host $_ -ForegroundColor Red
    if ($started) {
        Write-Host 'Attempting to finish this script-owned recording.'
        & $wpr -stop $trace
    }
} finally {
    Read-Host 'Press Enter to close' | Out-Null
}
