# Run from the extracted Windows package (Install-Driver.bat / Uninstall-Driver.bat).
# Never replaces a running driver's DLL.
param(
    [ValidateSet('Install', 'Uninstall', 'Restore', 'Status')][string]$Action = 'Install',
    [string]$SteamVRPath = '',
    [string]$DriverPath = (Join-Path $PSScriptRoot 'driver_lighthouse.dll')
)
$ErrorActionPreference = 'Stop'
if ($Action -eq 'Restore') { $Action = 'Uninstall' }
if (-not $SteamVRPath) {
    $pathsFile = Join-Path $env:LOCALAPPDATA 'openvr\openvrpaths.vrpath'
    if (Test-Path $pathsFile) {
        foreach ($candidate in (Get-Content -Raw $pathsFile | ConvertFrom-Json).runtime) {
            if (Test-Path (Join-Path $candidate 'drivers\lighthouse\bin\win64')) {
                $SteamVRPath = $candidate; break
            }
        }
    }
    if (-not $SteamVRPath) {
        $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam').SteamPath
        $SteamVRPath = Join-Path $steam 'steamapps\common\SteamVR'
    }
}
$directory = Join-Path $SteamVRPath 'drivers\lighthouse\bin\win64'
$target = Join-Path $directory 'driver_lighthouse.dll'
$backup = "$target.stock"
if (-not (Test-Path $target)) { throw "Driver not found at $target. Supply -SteamVRPath." }
if ($Action -eq 'Status') {
    Write-Output "Driver: $target"
    Write-Output "SHA256: $((Get-FileHash $target -Algorithm SHA256).Hash)"
    Write-Output "Stock backup present: $(Test-Path $backup)"
    exit 0
}
# Steam holds SteamVR's driver DLL open even when SteamVR itself is not running: with
# it up, replacing the file fails with "Cannot create a file when that file already
# exists". Check the whole chain, and name what to close.
# The installer offers to close them, and waits until they're gone.
$hostNames = 'steam', 'steamwebhelper', 'vrserver', 'vrmonitor', 'vrcompositor', 'vrdashboard', 'vrwebhelper',
             'vrstartup', 'vrservice', 'monado-service'
function Get-Hosts { @(Get-Process -Name $hostNames -ErrorAction SilentlyContinue) }
$hosts = Get-Hosts
if ($hosts) {
    $names = ($hosts.ProcessName | Sort-Object -Unique) -join ', '
    Write-Host ""
    Write-Host "Steam / SteamVR is running ($names). It has the lighthouse driver open, so it" -ForegroundColor Yellow
    Write-Host "must be closed before the driver can be changed." -ForegroundColor Yellow
    $answer = Read-Host "Close them now? Unsaved work in games is lost. [y/N]"
    if ($answer -notmatch '^[Yy]') { throw "Close Steam and SteamVR completely, then run this again." }
    $hosts | Stop-Process -Force -ErrorAction SilentlyContinue
    for ($i = 0; $i -lt 50 -and (Get-Hosts); $i++) { Start-Sleep -Milliseconds 200 }
    $left = Get-Hosts
    if ($left) { throw "Couldn't close $(($left.ProcessName | Sort-Object -Unique) -join ', '). Close them by hand (Task Manager), then run this again." }
    Start-Sleep -Milliseconds 500  # file handles go a moment after the process
    Write-Host "Closed."
}
# Overwrites, retrying for a few seconds: a file just released by a closed process can
# still be locked briefly (Move-Item then failed with "file already exists").
function Copy-Over([string]$from, [string]$to) {
    for ($i = 0; ; $i++) {
        try { [System.IO.File]::Copy($from, $to, $true); return }
        catch { if ($i -ge 20) { throw "Could not write $to - a process still has it open. Close Steam and SteamVR completely, then run this again. ($($_.Exception.Message))" } }
        Start-Sleep -Milliseconds 250
    }
}
# The update check (openhouse-check.exe): an app SteamVR starts with every session that
# warns when a SteamVR update replaced this driver. Registered in Steam's config:
# appconfig.json lists app manifests, vrappconfig\ holds auto-launch.
$checkKey = 'openhouse.check'
$checkDir = Join-Path $env:LOCALAPPDATA 'openhouse'
$checkManifest = Join-Path $checkDir 'openhouse-check.vrmanifest'
# Without a byte order mark (PowerShell 5's UTF8 writes one).
function Write-Text([string]$path, [string]$text) {
    [System.IO.File]::WriteAllText($path, $text + "`n", (New-Object System.Text.UTF8Encoding($false)))
}
function Get-SteamConfig {
    $pathsFile = Join-Path $env:LOCALAPPDATA 'openvr\openvrpaths.vrpath'
    if (Test-Path $pathsFile) {
        $config = @((Get-Content -Raw $pathsFile | ConvertFrom-Json).config) | Select-Object -First 1
        if ($config) { return $config }
    }
    return Join-Path (Split-Path (Split-Path (Split-Path $SteamVRPath))) 'config'
}
function Set-CheckRegistration([bool]$register) {
    $config = Get-SteamConfig
    $appconfig = Join-Path $config 'appconfig.json'
    $data = [ordered]@{}
    if (Test-Path $appconfig) {
        $parsed = Get-Content -Raw $appconfig | ConvertFrom-Json
        foreach ($p in $parsed.PSObject.Properties) { $data[$p.Name] = $p.Value }
    }
    $paths = @(@($data['manifest_paths']) | Where-Object { $_ -and ($_ -ne $checkManifest) })
    if ($register) { $paths += $checkManifest }
    $data['manifest_paths'] = $paths
    New-Item -ItemType Directory -Force -Path $config | Out-Null
    Write-Text $appconfig (ConvertTo-Json $data -Depth 10)
    $app = Join-Path $config "vrappconfig\$checkKey.vrappconfig"
    if ($register) {
        New-Item -ItemType Directory -Force -Path (Split-Path $app) | Out-Null
        Write-Text $app '{ "autolaunch" : true, "last_launch_time" : "0" }'
    } elseif (Test-Path $app) {
        Remove-Item -LiteralPath $app -Force
    }
}

# The openhouse page in SteamVR's settings: a settings schema in the lighthouse driver's
# resources (SteamVR has none there today; one it ships later is kept as .stock).
$schema = Join-Path (Split-Path (Split-Path $directory)) 'resources\settings\settingsschema.vrsettings'
function Install-Schema {
    $source = Join-Path $PSScriptRoot 'settingsschema.vrsettings'
    if (-not (Test-Path $source)) { return }
    New-Item -ItemType Directory -Force -Path (Split-Path $schema) | Out-Null
    if ((Test-Path $schema) -and -not (Select-String -Quiet -Path $schema -Pattern 'driver_openhouse')) {
        Copy-Item -LiteralPath $schema -Destination "$schema.stock" -Force
    }
    Copy-Item -LiteralPath $source -Destination $schema -Force
}
function Remove-Schema {
    if ((Test-Path $schema) -and (Select-String -Quiet -Path $schema -Pattern 'driver_openhouse')) { Remove-Item -LiteralPath $schema -Force }
    if (Test-Path "$schema.stock") {
        # Still there: a SteamVR update replaced ours with a newer one, so the backup is older.
        if (Test-Path $schema) { Remove-Item -LiteralPath "$schema.stock" -Force }
        else { Copy-Over "$schema.stock" $schema; Remove-Item -LiteralPath "$schema.stock" -Force }
    }
}

if ($Action -eq 'Uninstall') {
    # First, so the check goes even when there's no driver backup.
    Remove-Schema
    Set-CheckRegistration $false
    if (Test-Path $checkDir) { Remove-Item -LiteralPath $checkDir -Recurse -Force }
}
$installed = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($target))
$isOurs = $installed.Contains('liblh session started')
if ($Action -eq 'Install') {
    if (-not (Test-Path $DriverPath)) { throw "Package DLL missing: $DriverPath" }
    # The stock DLL is kept as driver_lighthouse.dll.stock: our driver loads it at run time
    # for headset displays. Back up whatever is installed unless it is ours (a SteamVR
    # update puts a newer stock DLL in place: that one replaces an older backup).
    if (-not $isOurs) {
        Copy-Over $target $backup
        Write-Output "Saved SteamVR's driver as $backup"
    } elseif (-not (Test-Path $backup)) {
        throw "The installed driver is already openhouse's and there is no stock backup ($backup). Verify SteamVR's files in Steam, then install again."
    }
    $source = $DriverPath
} else {
    # A SteamVR update puts its own, newer driver in place of ours. The backup is then the
    # older one: moving it back would undo the update, so it goes instead.
    if (-not $isOurs) {
        if (Test-Path $backup) { Remove-Item -LiteralPath $backup -Force }
        Write-Output "SteamVR's own driver is already in place (a SteamVR update put it back). Your room and settings in %APPDATA%\lh are kept."
        exit 0
    }
    if (-not (Test-Path $backup)) { throw "SteamVR's driver backup isn't there ($backup). Nothing to undo, or verify SteamVR's files in Steam." }
    Copy-Over $backup $target
    Remove-Item -LiteralPath $backup -Force
    Write-Output "SteamVR's own driver is back. Your room and settings in %APPDATA%\lh are kept."
    exit 0
}
Copy-Over $source $target
if ((Get-FileHash $target).Hash -ne (Get-FileHash $source).Hash) { throw 'Installed file hash mismatch' }
Install-Schema
$checkExe = Join-Path $PSScriptRoot 'apps\openhouse-check.exe'
if (Test-Path $checkExe) {
    New-Item -ItemType Directory -Force -Path $checkDir | Out-Null
    Copy-Item -LiteralPath $checkExe -Destination (Join-Path $checkDir 'openhouse-check.exe') -Force
    $manifest = [ordered]@{
        source = 'openhouse'
        applications = @([ordered]@{
            app_key = $checkKey
            launch_type = 'binary'
            binary_path_windows = (Join-Path $checkDir 'openhouse-check.exe')
            arguments = '"' + $directory + '"'
            is_dashboard_overlay = $true
            strings = @{ en_us = @{ name = 'openhouse update check'; description = 'Tells you when a SteamVR update replaced the openhouse driver' } }
        })
    }
    Write-Text $checkManifest (ConvertTo-Json $manifest -Depth 10)
    Set-CheckRegistration $true
}
Write-Output "Installed. Start SteamVR."
