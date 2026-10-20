param(
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [Parameter(Mandatory = $true)][string]$Destination,
    [string]$ObsRoot = 'C:\Program Files\obs-studio',
    [int]$Runs = 3,
    [ValidateSet('en-US','zh-CN','zh-TW')][string]$Locale = 'en-US',
    [switch]$Baseline
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
python (Join-Path $projectRoot 'tests\prepare-smoke.py') --obs-root $ObsRoot --build $BuildDirectory --out $Destination --locale $Locale
if ($LASTEXITCODE -ne 0) { throw 'Cannot prepare portable OBS.' }
if ($Baseline) {
    $testPlugin = Join-Path $Destination 'obs-plugins\64bit\OBS-PlaylistDock.dll'
    Remove-Item -LiteralPath $testPlugin
}
for ($run = 1; $run -le $Runs; $run++) {
    $process = Start-Process -FilePath (Join-Path $Destination 'bin\64bit\obs64.exe') `
        -ArgumentList @('--portable','--multi','--disable-updater','--disable-shutdown-check','--minimize-to-tray') `
        -WorkingDirectory (Join-Path $Destination 'bin\64bit') -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit(30000)) {
        Stop-Process -Id $process.Id -ErrorAction SilentlyContinue
        throw "Smoke run $run did not close within 30 seconds."
    }
    $latestLog = Get-ChildItem -LiteralPath (Join-Path $Destination 'config\obs-studio\logs') -File | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $text = Get-Content -LiteralPath $latestLog.FullName -Raw
    if ($process.ExitCode -ne 0) { throw "Smoke run $run exited with $($process.ExitCode)." }
    if (-not $Baseline -and $text -notmatch '\[OBS-PlaylistDock\].*loaded; Qt 6.8.3; dock registered\.') {
        throw "Plugin did not load: $($latestLog.FullName)"
    }
    if ($env:PLAYLISTDOCK_SMOKE_MEDIA -and $text -notmatch 'OBS_UI_CHECK all=PASS') {
        throw "UI checks did not pass: $($latestLog.FullName)"
    }
    Write-Output "OBS_SMOKE run=$run baseline=$($Baseline.IsPresent) exit=$($process.ExitCode)"
    Select-String -LiteralPath $latestLog.FullName -Pattern '\[OBS-PlaylistDock\]|OBS_UI_CHECK|Number of memory leaks' | ForEach-Object { $_.Line }
}
