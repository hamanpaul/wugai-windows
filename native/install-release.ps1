param(
    [Parameter(Mandatory=$true)][string]$BundleRoot,
    [switch]$Execute,
    [string]$ExpectedUserSid = '',
    [string]$SetupPath = ''
)
$ErrorActionPreference = 'Stop'
$releaseMutex = $null
$releaseLockHeld = $false
$identity = $null
try {
    $bundle = (Resolve-Path -LiteralPath $BundleRoot).Path
    $release = Get-Content -LiteralPath (Join-Path $bundle 'release-manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($release.format -cne 'liukai-release-v1' -or $release.architecture -cne 'x64' -or $release.registration_scope -cne 'user-and-machine-com-x64-v1') { throw 'Invalid x64 release.' }
    foreach ($row in $release.files) {
        if ($row.path -notmatch '^[A-Za-z0-9._/-]+$' -or $row.path.Contains('..') -or $row.path.StartsWith('/')) { throw 'Invalid release path.' }
        $file = Join-Path $bundle $row.path.Replace('/','\')
        if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -cne $row.sha256) { throw "Release hash mismatch: $($row.path)" }
    }
    $releaseMutex = New-Object Threading.Mutex($false,'Global\WuGai-ReleaseLifecycle-v1')
    try { $releaseLockHeld = $releaseMutex.WaitOne(0) }
    catch [Threading.AbandonedMutexException] { $releaseLockHeld = $true }
    if (-not $releaseLockHeld) { throw 'Another WuGai installation or removal is in progress.' }
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $sid = $identity.User.Value
    if ($ExpectedUserSid -and $sid -cne $ExpectedUserSid) { throw 'Windows account changed during elevation.' }
    $root = Join-Path $env:ProgramFiles 'LiuKai'
    $devRoot = Join-Path $env:ProgramFiles "LiuKaiDev-$sid\Managed"
    $comKey = 'Registry::HKEY_CURRENT_USER\SOFTWARE\Classes\CLSID\{5D7C4291-2B34-4CE8-8A11-39627105C42E}\InprocServer32'
    if (Test-Path -LiteralPath $comKey) {
        $current = [string](Get-Item -LiteralPath $comKey).GetValue('')
        if ($current.StartsWith($devRoot+'\versions\',[StringComparison]::OrdinalIgnoreCase)) { $root = $devRoot }
        elseif (-not $current.StartsWith($root+'\versions\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Existing development registration needs an explicit migration; it will not be overwritten.' }
    }
    $package = Join-Path $bundle 'package'
    $packageManifest = Get-Content -LiteralPath (Join-Path $package 'manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $dll = Join-Path $root "versions\$($packageManifest.version)\x64\BoshiamyTsf.dll"
    $tool = Join-Path $bundle 'LiuKaiRegister.exe'
    $wrapper = Join-Path $bundle 'package-install-windows.ps1'
    if (-not $Execute) {
        & $wrapper -PackageRoot $package -InstallRoot $root -ExpectedUserSid $sid -MachineComX64
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        [pscustomobject]@{phase='modern-host-registration';registration_scope=$release.registration_scope;tool=$tool;dll=$dll;executed=$false} | ConvertTo-Json
        exit 0
    }
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run the installer as administrator using the same Windows account.' }
    if (-not $SetupPath -or -not (Test-Path -LiteralPath $SetupPath -PathType Leaf)) { throw 'SetupPath must identify the installer executable before deployment.' }
    $setup = (Resolve-Path -LiteralPath $SetupPath).Path
    . (Join-Path $bundle 'maintenance.ps1')
    . (Join-Path $bundle 'uninstall-file-recovery.ps1')
    if (-not (Get-Command Publish-WuGaiUninstaller -CommandType Function -ErrorAction SilentlyContinue)) { throw 'The release does not provide an uninstaller publisher.' }
    # An interrupted older removal may have deleted some files or its Apps
    # entry. Recover its verified backup before any new deployment begins.
    foreach ($recoveryRoot in @($root,$devRoot) | Select-Object -Unique) {
        if (Test-Path -LiteralPath (Get-WuGaiRemovalReceiptPath $recoveryRoot)) {
            Recover-WuGaiMaintenancePublication -InstallRoot $recoveryRoot -ExpectedUserSid $sid
            Recover-WuGaiInterruptedFileRemoval -InstallRoot $recoveryRoot -ExpectedUserSid $sid `
                -Cli (Join-Path $package 'LiuKaiInstaller.exe')
        }
    }
    # Keep the existing durable deployment and its immutable history. The new
    # capability is an explicit, independently read-back registration step.
    & $wrapper -PackageRoot $package -InstallRoot $root -ExpectedUserSid $sid -MachineComX64 -Execute
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $registrationText = & $tool --register $dll | Out-String
    $registrationCode = $LASTEXITCODE
    Write-Output $registrationText
    if ($registrationCode -ne 0) { throw 'Package was deployed, but modern-host registration failed. Preserve the log; do not reinstall blindly.' }
    # Registration operation success is independent of the registering manager's
    # cached view. Verify all postconditions below in a new helper process.
    $readbackText = & $tool --status $dll | Out-String
    if ($LASTEXITCODE -ne 0) { throw 'Independent registration readback failed.' }
    $readback = $readbackText | ConvertFrom-Json
    if ($readback.immersive_supported -ne $true -or $readback.input_mode_supported -ne $true -or $readback.registered -ne $true -or $readback.machine_com_registered -ne $true -or $readback.machine_com -ne 0) { throw 'Independent modern-host readback mismatch.' }
    Write-Output $readbackText
    try {
        Publish-WuGaiUninstaller -SetupPath $setup -InstallRoot $root -Version $packageManifest.version -ExpectedUserSid $sid
    } catch {
        throw "Application deployment passed, but publishing the Windows uninstall entry failed: $($_.Exception.Message)"
    }
    Write-Output 'Installation complete. Reopen applications that were already running.'
    exit 0
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
} finally {
    if ($null -ne $identity) { $identity.Dispose() }
    if ($releaseLockHeld) { $releaseMutex.ReleaseMutex() }
    if ($null -ne $releaseMutex) { $releaseMutex.Dispose() }
}
