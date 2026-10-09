param([Parameter(Mandatory = $true)][string]$ApplicationPath)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$application = (Resolve-Path -LiteralPath $ApplicationPath).Path
$start = [Diagnostics.ProcessStartInfo]::new($application)
$start.ArgumentList.Add('--internal-overlay-configuration-worker')
# This action is rejected before WMI/native/staging initialization. The smoke
# check verifies the real GUI-subsystem executable's redirected worker protocol.
$start.ArgumentList.Add('invalid-action-for-protocol-test')
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$start.StandardOutputEncoding = [Text.UTF8Encoding]::new($false)
$process = [Diagnostics.Process]::new()
$process.StartInfo = $start
try {
    if (-not $process.Start()) { throw 'Overlay worker could not start' }
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(15000)) {
        $process.Kill($true)
        throw 'Invalid-action overlay worker timed out'
    }
    $report = $stdout.GetAwaiter().GetResult().TrimStart([char]0xfeff) | ConvertFrom-Json
    $errors = $stderr.GetAwaiter().GetResult()
    if ($process.ExitCode -ne 1 -or $report.protocol -ne 1 -or $report.exitCode -ne -1 -or
        $report.executionFailed -ne $true -or $report.output -ne 'Invalid overlay worker action.' -or $errors.Length -ne 0) {
        throw 'Overlay worker did not return the expected normal failure protocol'
    }
    Write-Host 'Real overlay-worker protocol smoke check passed; no UWF API was called.'
} finally {
    $process.Dispose()
}
