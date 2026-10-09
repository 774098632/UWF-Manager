param(
    [Parameter(Mandatory = $true)][string]$Msys2Root,
    [string]$BuildDirectory = 'build',
    [string]$OutputDirectory = 'dist'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$prefix = Join-Path $Msys2Root 'clang64'
$bin = Join-Path $prefix 'bin'
$env:PATH = "$bin;$(Join-Path $Msys2Root 'usr/bin');$env:PATH"
$env:LC_ALL = 'C'

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    $result = & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Program failed with exit code $LASTEXITCODE"
    }
    return $result
}

$commit = (Invoke-Checked 'git' @('rev-parse', 'HEAD')).Trim()
$shortCommit = $commit.Substring(0, 12)
$repository = if ($env:GITHUB_REPOSITORY) { $env:GITHUB_REPOSITORY } else { '774098632/UWF-Manager' }
$bundleName = "UWF-Manager-windows-x64-$shortCommit"
$output = [IO.Path]::GetFullPath($OutputDirectory)
$bundle = Join-Path $output $bundleName
if (Test-Path -LiteralPath $bundle) {
    throw "Package directory already exists: $bundle. Use a fresh output directory."
}
New-Item -ItemType Directory -Path $bundle -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $BuildDirectory 'UWF.exe') -Destination $bundle

# windeployqt discovers Qt plugins; the additional scan below includes the
# non-Qt MSYS2 libraries on which those DLLs depend. No application is launched.
Invoke-Checked (Join-Path $bin 'windeployqt6.exe') @(
    '--release', '--compiler-runtime', '--no-translations',
    '--no-system-d3d-compiler', '--no-opengl-sw',
    '--dir', $bundle, (Join-Path $bundle 'UWF.exe')
) | Write-Host

$dllSources = @{}
Get-ChildItem -LiteralPath $bin -Filter '*.dll' -File | ForEach-Object { $dllSources[$_.Name] = $_.FullName }
Get-ChildItem -LiteralPath (Join-Path $prefix 'share/qt6/plugins') -Filter '*.dll' -File -Recurse |
    ForEach-Object { $dllSources[$_.Name] = $_.FullName }
$queue = [Collections.Generic.Queue[string]]::new()
Get-ChildItem -LiteralPath $bundle -Recurse -File |
    Where-Object { $_.Extension -in '.exe', '.dll' } |
    ForEach-Object { $queue.Enqueue($_.FullName) }
$scanned = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
while ($queue.Count -gt 0) {
    $file = $queue.Dequeue()
    if (-not $scanned.Add($file)) { continue }
    $headers = Invoke-Checked (Join-Path $bin 'llvm-objdump.exe') @('--private-headers', $file)
    foreach ($line in $headers) {
        if ($line -cnotmatch '^\s*DLL Name:\s*(\S+)\s*$') { continue }
        $name = $Matches[1]
        $destination = Join-Path $bundle $name
        if (Test-Path -LiteralPath $destination) { continue }
        if ($dllSources.ContainsKey($name)) {
            Copy-Item -LiteralPath $dllSources[$name] -Destination $destination
            $queue.Enqueue($destination)
        } elseif ($name -notmatch '^(api-ms-win-|ext-ms-win-)' -and
                  -not (Test-Path -LiteralPath (Join-Path "$env:SystemRoot/System32" $name))) {
            throw "Unresolved DLL dependency: $name (required by $file)"
        }
    }
}

foreach ($required in @('Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'Qt6Svg.dll', 'Qt6Network.dll', 'platforms/qwindows.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $bundle $required))) {
        throw "Portable bundle is missing $required"
    }
}
# Ensure Qt always resolves its plugins inside this portable directory.
"[Paths]`nPlugins=.`n" | Set-Content -LiteralPath (Join-Path $bundle 'qt.conf') -Encoding utf8
Copy-Item -LiteralPath 'LICENSE', 'README.md', 'README.zh_CN.md', 'MANUAL_RESTORE.zh_CN.md' -Destination $bundle

# Preserve the package versions, licenses and exact source-package URLs for
# redistributed Qt/compiler/third-party DLLs alongside the application GPL.
$owners = [Collections.Generic.HashSet[string]]::new()
Get-ChildItem -LiteralPath $bundle -Recurse -Filter '*.dll' -File | ForEach-Object {
    if ($dllSources.ContainsKey($_.Name)) {
        $posixPath = Invoke-Checked (Join-Path $Msys2Root 'usr/bin/cygpath.exe') @('-u', $dllSources[$_.Name])
        $owner = Invoke-Checked (Join-Path $Msys2Root 'usr/bin/pacman.exe') @('-Qqo', $posixPath.Trim())
        [void]$owners.Add($owner.Trim())
    }
}
$packages = foreach ($owner in ($owners | Sort-Object)) {
    $versionLine = Invoke-Checked (Join-Path $Msys2Root 'usr/bin/pacman.exe') @('-Q', $owner)
    $version = ($versionLine -split '\s+', 2)[1].Trim()
    $desc = Get-Content -LiteralPath (Join-Path $Msys2Root "var/lib/pacman/local/$owner-$version/desc") -Raw
    if ($desc -notmatch '%BASE%\r?\n([^\r\n]+)') { throw "Missing package base for $owner" }
    $packageBase = $Matches[1]
    $packageFiles = Invoke-Checked (Join-Path $Msys2Root 'usr/bin/pacman.exe') @('-Qql', $owner)
    foreach ($license in ($packageFiles | Where-Object { $_ -match '/share/licenses/' -and -not $_.EndsWith('/') })) {
        $source = Join-Path $Msys2Root $license.TrimStart('/')
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { continue }
        $relative = ($license -split '/share/licenses/', 2)[1]
        $destination = Join-Path $bundle "licenses/$relative"
        New-Item -ItemType Directory -Path (Split-Path $destination) -Force | Out-Null
        Copy-Item -LiteralPath $source -Destination $destination
    }
    [ordered]@{
        package = $owner
        version = $version
        source = "https://mirror.msys2.org/mingw/sources/$packageBase-$version.src.tar.zst"
        recipe = "https://github.com/msys2/MINGW-packages/tree/master/$packageBase"
    }
}
[ordered]@{
    repository = "https://github.com/$repository"
    commit = $commit
    configuration = 'Release, Windows x64, MSYS2 CLANG64, UWF_SANITIZE=OFF, UWF_STATIC_RUNTIME=OFF'
    qtVersion = (Invoke-Checked (Join-Path $bin 'qmake6.exe') @('-query', 'QT_VERSION')).Trim()
    dependencies = @($packages)
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $bundle 'build-info.json') -Encoding utf8
@"
UWF Manager is licensed under GPL-3.0-or-later; see LICENSE.
Application source: https://github.com/$repository/tree/$commit
Matching application source is provided in UWF-Manager-source-$shortCommit.zip in the same build artifact.
Qt is dynamically linked. Third-party license texts are in licenses/;
package versions and corresponding source-package links are in build-info.json.
Extract the entire portable zip before running UWF.exe.
"@ | Set-Content -LiteralPath (Join-Path $bundle 'SOURCE.txt') -Encoding utf8

$sourceArchive = Join-Path $output "UWF-Manager-source-$shortCommit.zip"
Invoke-Checked 'git' @('archive', '--format=zip', "--prefix=UWF-Manager-source-$shortCommit/", "--output=$sourceArchive", $commit)
Compress-Archive -LiteralPath $bundle -DestinationPath (Join-Path $output "$bundleName.zip")
Get-ChildItem -LiteralPath $output -Filter '*.zip' -File | Sort-Object Name | ForEach-Object {
    "$( (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() )  $($_.Name)"
} | Set-Content -LiteralPath (Join-Path $output 'SHA256SUMS.txt') -Encoding ascii
