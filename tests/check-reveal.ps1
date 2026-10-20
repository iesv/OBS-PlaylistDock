param([Parameter(Mandatory = $true)][string]$FilePath)
$ErrorActionPreference = 'Stop'
$expected = (Get-Item -LiteralPath $FilePath).FullName
$shell = New-Object -ComObject Shell.Application
$elapsed = [Diagnostics.Stopwatch]::StartNew()
$windowCount = 0
$observed = @()
while ($elapsed.Elapsed.TotalSeconds -lt 15) {
    $windows = @($shell.Windows())
    $windowCount = $windows.Count
    $observed = @()
    foreach ($window in $windows) {
        if ([IO.Path]::GetFileName($window.FullName) -ine 'explorer.exe') { continue }
        try {
            $selection = @($window.Document.SelectedItems())
            $observed += @{ folder = $window.Document.Folder.Self.Path; selected = @($selection | ForEach-Object Path) }
            foreach ($item in $selection) {
                if ([string]::Equals($item.Path, $expected, [StringComparison]::OrdinalIgnoreCase)) {
                    Write-Output 'WINDOWS_REVEAL actual_selected_file=PASS'
                    exit 0
                }
            }
        } catch [System.Management.Automation.MethodInvocationException] {
            # Explorer can replace its view while opening a folder.
        }
    }
    Start-Sleep -Milliseconds 200
}
Write-Output "WINDOWS_REVEAL windows=$windowCount observed=$($observed | ConvertTo-Json -Depth 3 -Compress)"
throw "Explorer did not select the requested file: $expected"
