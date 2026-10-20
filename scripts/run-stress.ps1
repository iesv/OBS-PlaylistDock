param(
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [Parameter(Mandatory = $true)][string]$Qt,
    [string]$ObsRoot = 'C:\Program Files\obs-studio',
    [string[]]$Media
)
$ErrorActionPreference = 'Stop'
$executable = Join-Path $BuildDirectory 'Release\playlistdock-stress.exe'
$env:PATH = (Join-Path $ObsRoot 'bin\64bit') + ';' + (Join-Path $Qt 'bin') + ';' + $env:PATH
$env:QT_QPA_PLATFORM = 'minimal'
$env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $Qt 'plugins\platforms'
$env:PLAYLISTDOCK_OBS_DATA = Join-Path $ObsRoot 'data\libobs'
& $executable --fake
if ($LASTEXITCODE -ne 0) { throw 'State/lifecycle stress tests failed.' }
if ($Media -and $Media.Count -gt 0) {
    $vlcModule = Join-Path $ObsRoot 'obs-plugins\64bit\vlc-video.dll'
    $vlcData = Join-Path $ObsRoot 'data\obs-plugins\vlc-video'
    & $executable --vlc $vlcModule $vlcData @Media
    if ($LASTEXITCODE -ne 0) { throw 'Real VLC stress tests failed.' }
}
