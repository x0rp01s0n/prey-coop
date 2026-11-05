$ErrorActionPreference = 'Stop'
try {
    $root = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'PreyCoop-Diagnostics'
    if (-not (Test-Path -LiteralPath $root -PathType Container)) { throw 'No diagnostic folder found. Run Run-Diagnostics.cmd first.' }
    $session = Get-ChildItem -LiteralPath $root -Directory | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $session) { throw 'No diagnostic session found. Run Run-Diagnostics.cmd first.' }
    $logs = @(Get-ChildItem -LiteralPath $session.FullName -File | Where-Object { $_.Name -like 'coop_perf*' })
    if ($logs.Count -eq 0) { throw 'No performance log found. Check that the diagnostic DLL is enabled and Prey was launched from the diagnostic ChairManager session.' }
    if (-not (Test-Path -LiteralPath (Join-Path $session.FullName 'session.txt'))) { throw 'The latest diagnostic folder has no launcher metadata. Select the intended session manually before sharing its files.' }
    $files = @($logs.FullName) + @(Join-Path $session.FullName 'session.txt')
    $archive = Join-Path $root ($session.Name + '.zip')
    Compress-Archive -LiteralPath $files -DestinationPath $archive -Force
    Write-Host "Created: $archive"
    Write-Host 'Review these files before sharing. Nothing has been uploaded automatically.'
    Start-Process explorer.exe -ArgumentList ('/select,"' + $archive + '"')
} catch {
    Write-Error $_
    exit 1
}
