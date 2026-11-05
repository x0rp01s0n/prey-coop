param([string]$ManagerPath)
$ErrorActionPreference = 'Stop'
try {
    if (Get-Process Prey,ChairManager -ErrorAction SilentlyContinue) {
        throw 'Close Prey and ChairManager first, then run this launcher again.'
    }
    if (-not $ManagerPath) {
        Add-Type -AssemblyName System.Windows.Forms
        $dialog = New-Object System.Windows.Forms.OpenFileDialog
        $dialog.Title = 'Select ChairManager.exe'
        $dialog.Filter = 'ChairManager.exe|ChairManager.exe'
        if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { exit 0 }
        $ManagerPath = $dialog.FileName
    }
    if (-not (Test-Path -LiteralPath $ManagerPath -PathType Leaf)) { throw 'ChairManager.exe was not found.' }
    if ([IO.Path]::GetFileName($ManagerPath) -ine 'ChairManager.exe') { throw 'Please select ChairManager.exe.' }
    $root = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'PreyCoop-Diagnostics'
    $session = Join-Path $root ((Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + $PID)
    New-Item -ItemType Directory -Path $session -Force | Out-Null
    $env:COOP_PERF_DIAGNOSTICS = '1'
    $env:COOP_RUNTIME_OBJECT_PROBE_MODE = 'direct'
    $env:COOP_PERF_OUTPUT_DIR = $session
    @(
        'PreyCoop diagnostic session'
        'Protocol=250'
        ('Started=' + (Get-Date -Format o))
        'COOP_PERF_DIAGNOSTICS=1'
        'COOP_RUNTIME_OBJECT_PROBE_MODE=direct'
        ('LogicalProcessors=' + [Environment]::ProcessorCount)
        ('WindowsVersion=' + [Environment]::OSVersion.VersionString)
    ) | Set-Content -LiteralPath (Join-Path $session 'session.txt') -Encoding UTF8
    Start-Process -FilePath $ManagerPath -WorkingDirectory (Split-Path -Parent $ManagerPath)
    Write-Host 'ChairManager started with diagnostics and direct object checks.'
    Write-Host 'Launch Prey normally through ChairManager. Do not enable other tracing.'
    Write-Host "Diagnostic folder: $session"
    Write-Host 'After the test, close Prey and run Collect-Diagnostics.cmd.'
} catch {
    Write-Error $_
    exit 1
}
