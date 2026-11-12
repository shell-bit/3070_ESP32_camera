param(
    [string]$ArduinoCli = 'arduino-cli',
    [string]$ConfigFile = '',
    [string]$LibraryDirectory = '',
    [switch]$HostTestsOnly
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$layoutTool = Join-Path $root 'tools/project_layout.py'
$paths = & python -B $layoutTool paths | ConvertFrom-Json
if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve viewer project paths' }
$build = $paths.build
New-Item -ItemType Directory -Force $build | Out-Null
& python -B (Join-Path $root 'tools/run_host_tests.py')
if ($LASTEXITCODE -ne 0) { throw 'Host tests failed' }
if ($HostTestsOnly) { exit 0 }
$coreArguments = @('core', 'list', '--format', 'json')
if ($ConfigFile) { $coreArguments += @('--config-file', $ConfigFile) }
$cores = (& $ArduinoCli @coreArguments | ConvertFrom-Json).platforms
if ($LASTEXITCODE -ne 0 -or -not ($cores | Where-Object { $_.id -eq 'esp32:esp32' -and $_.installed_version -eq '3.3.12' })) {
    throw 'This build is pinned to Arduino-ESP32 3.3.12'
}
if (-not $LibraryDirectory) { $LibraryDirectory = $paths.libraries }
$dependency = & python -B $layoutTool verify-dependency --library (Join-Path $LibraryDirectory 'lvgl') | ConvertFrom-Json
if ($LASTEXITCODE -ne 0) { throw 'Run prepare_dependencies.py or supply the pinned LVGL directory' }
# Stage only sketch sources so Arduino does not traverse the SDK/vendor cache.
$sources = & python -B $layoutTool sources | ConvertFrom-Json
if ($LASTEXITCODE -ne 0) { throw 'Invalid viewer source tree' }
$sketch = Join-Path $build 'sketch/lcd_viewer'
$resolvedBuild = [IO.Path]::GetFullPath($build)
$resolvedSketch = [IO.Path]::GetFullPath($sketch)
if (-not $resolvedBuild.StartsWith([IO.Path]::GetFullPath($paths.project_root) + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
    -not $resolvedSketch.StartsWith($resolvedBuild + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Staging must remain inside the project build directory'
}
foreach ($parent in @((Join-Path $paths.project_root 'build'), $build, (Join-Path $build 'sketch'))) {
    if ((Test-Path -LiteralPath $parent) -and ((Get-Item -LiteralPath $parent -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Linked build path is not allowed' }
}
if (Test-Path -LiteralPath $sketch) {
    $entries = @((Get-Item -LiteralPath $sketch -Force)) + @(Get-ChildItem -LiteralPath $sketch -Recurse -Force)
    if ($entries | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }) { throw 'Linked staging path is not allowed' }
    Remove-Item -LiteralPath $resolvedSketch -Recurse -Force
}
New-Item -ItemType Directory -Force $sketch | Out-Null
foreach ($source in $sources.PSObject.Properties.Name) {
    $target = Join-Path $sketch $source
    New-Item -ItemType Directory -Force (Split-Path -Parent $target) | Out-Null
    Copy-Item -LiteralPath (Join-Path $root $source) -Destination $target -Force
}
$fqbn = 'esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,USBMode=hwcdc,CDCOnBoot=cdc'
@{fqbn=$fqbn; library_directory=[IO.Path]::GetFullPath($LibraryDirectory); lvgl=$dependency} | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $build 'build-settings.json')
Remove-Item -LiteralPath (Join-Path $build 'build-manifest.json') -ErrorAction SilentlyContinue
$arguments = @('compile', '--fqbn', $fqbn, '--jobs', '4', '--libraries', $LibraryDirectory, '--build-path', (Join-Path $build 'arduino'), $sketch)
if ($ConfigFile) { $arguments += @('--config-file', $ConfigFile) }
& $ArduinoCli @arguments 2>&1 | Tee-Object -FilePath (Join-Path $build 'compile.log')
if ($LASTEXITCODE -ne 0) { throw "ESP32-S3 firmware compilation failed; see $build/compile.log" }
& python -B (Join-Path $root 'tools/write_build_manifest.py')
if ($LASTEXITCODE -ne 0) { throw 'Firmware built but source/artifact manifest verification failed' }
