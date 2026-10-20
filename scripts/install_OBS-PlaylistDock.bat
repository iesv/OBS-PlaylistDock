@echo off
setlocal DisableDelayedExpansion
chcp 65001 >nul
title OBS-PlaylistDock
set "PLAYLISTDOCK_SETUP_FILE=%~f0"
set "PLAYLISTDOCK_OBS_ROOT=%~1"
powershell.exe -NoLogo -NoProfile -Command "$parts = (Get-Content -LiteralPath $env:PLAYLISTDOCK_SETUP_FILE -Raw -Encoding UTF8) -split '(?m)^# POWERSHELL_BEGIN\r?$', 2; & ([ScriptBlock]::Create($parts[1]))"
exit /b %errorlevel%
# POWERSHELL_BEGIN
$ErrorActionPreference = 'Stop'
try {
    $installerPath = $env:PLAYLISTDOCK_SETUP_FILE
    $packageRoot = Split-Path -Parent $installerPath
    $obsRoot = $env:PLAYLISTDOCK_OBS_ROOT
    if ([string]::IsNullOrWhiteSpace($obsRoot)) {
        $obsRoot = Join-Path $env:ProgramFiles 'obs-studio'
    }
    if (Get-Process -Name obs64 -ErrorAction SilentlyContinue) {
        throw '请先关闭 OBS，再运行安装脚本。'
    }
    $obsExe = Join-Path $obsRoot 'bin\64bit\obs64.exe'
    $qtDll = Join-Path $obsRoot 'bin\64bit\Qt6Core.dll'
    if (-not (Test-Path -LiteralPath $obsExe) -or -not (Test-Path -LiteralPath $qtDll)) {
        throw '找不到 OBS。可将 OBS 安装目录作为本 BAT 的第一个参数。'
    }
    if ((Get-Item -LiteralPath $obsExe).VersionInfo.FileVersion -notmatch '^32\.0\.4(?:\D|$)') {
        throw '本插件需要 OBS Studio 32.0.4。'
    }
    if ((Get-Item -LiteralPath $qtDll).VersionInfo.FileVersion -notmatch '^6\.8\.3(?:\D|$)') {
        throw '本插件需要 OBS 随附的 Qt 6.8.3。'
    }
    $source = Join-Path $packageRoot 'obs-playlistdock'
    if (-not (Test-Path -LiteralPath (Join-Path $source 'bin\64bit\OBS-PlaylistDock.dll'))) {
        throw '请先完整解压 ZIP，再运行其中的 BAT 安装脚本。'
    }
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        $arguments = '/d /c ""{0}" "{1}""' -f $installerPath, $obsRoot
        $elevated = Start-Process -FilePath $env:ComSpec -ArgumentList $arguments -WorkingDirectory $packageRoot -Verb RunAs -Wait -PassThru
        exit $elevated.ExitCode
    }
    $destination = Join-Path $env:ProgramData 'obs-studio\plugins\obs-playlistdock'
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    foreach ($entry in Get-ChildItem -LiteralPath $source -Force) {
        Copy-Item -LiteralPath $entry.FullName -Destination $destination -Recurse -Force
    }
    Write-Host "已安装到：$destination"
    Write-Host '启动 OBS 后，在“停靠窗口”菜单中打开“播放列表”（Playlist／播放清單）。'
    if (-not [Console]::IsInputRedirected) {
        for ($remaining = 5; $remaining -gt 0; --$remaining) {
            Write-Host -NoNewline ("`r安装完成，{0} 秒后退出；按任意键立即退出。   " -f $remaining)
            for ($tick = 0; $tick -lt 10; ++$tick) {
                if ([Console]::KeyAvailable) {
                    [Console]::ReadKey($true) | Out-Null
                    Write-Host
                    exit 0
                }
                Start-Sleep -Milliseconds 100
            }
        }
        Write-Host "`r安装完成，正在退出。                                      "
    }
    exit 0
} catch {
    Write-Host ("安装失败：" + $_.Exception.Message) -ForegroundColor Red
    if (-not [Console]::IsInputRedirected) {
        Write-Host '按任意键退出。'
        [Console]::ReadKey($true) | Out-Null
    }
    exit 1
}
