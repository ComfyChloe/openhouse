# Run from the extracted Windows package. Never replaces a running driver's DLL.
param(
    [ValidateSet('Install', 'Restore', 'Status')][string]$Action = 'Install',
    [string]$SteamVRPath = '',
    [string]$DriverPath = (Join-Path $PSScriptRoot 'driver_lighthouse.dll')
)
$ErrorActionPreference = 'Stop'
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
if (Get-Process -Name vrserver,monado-service -ErrorAction SilentlyContinue) {
    throw 'Stop SteamVR / Monado before changing the driver.'
}
if ($Action -eq 'Install') {
    if (-not (Test-Path $DriverPath)) { throw "Package DLL missing: $DriverPath" }
    # The stock DLL is kept as driver_lighthouse.dll.stock: our driver loads it at run time
    # for headset displays. Back up whatever is installed unless it is ours (a SteamVR
    # update puts a newer stock DLL in place: that one replaces an older backup).
    $installed = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($target))
    $isOurs = $installed.Contains('liblh session started')
    if (-not $isOurs) {
        Copy-Item -LiteralPath $target -Destination $backup -Force
        Write-Output "Saved SteamVR's driver as $backup"
    } elseif (-not (Test-Path $backup)) {
        throw "The installed driver is already lhdrvrw's and there is no stock backup ($backup). Verify SteamVR's files in Steam, then install again."
    }
    $source = $DriverPath
} else {
    if (-not (Test-Path $backup)) { throw "Stock backup missing: $backup" }
    $source = $backup
}
Copy-Item -LiteralPath $source -Destination "$target.new" -Force
Move-Item -LiteralPath "$target.new" -Destination $target -Force
if ((Get-FileHash $target).Hash -ne (Get-FileHash $source).Hash) { throw 'Installed file hash mismatch' }
Write-Output "$Action complete: $target (stock backup retained at $backup)"
