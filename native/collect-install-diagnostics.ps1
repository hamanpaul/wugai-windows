# Read-only collection: never runs the installer or changes registration.
# Run from the Windows account that attempted installation.
param(
    [string]$OutputDirectory = (Get-Location).Path,
    [string]$InstallRoot,
    [string]$InstallerPath
)
$ErrorActionPreference = 'Stop'
$issues = New-Object 'System.Collections.Generic.List[string]'
$inventory = New-Object 'System.Collections.Generic.List[object]'
$utf8 = New-Object Text.UTF8Encoding($false)
if (-not $InstallRoot) {
    $programs = $env:ProgramW6432
    if (-not $programs) { $programs = $env:ProgramFiles }
    $InstallRoot = Join-Path $programs 'LiuKai'
}
$InstallRoot = [IO.Path]::GetFullPath($InstallRoot)
$output = (Resolve-Path -LiteralPath $OutputDirectory).Path
$name = 'WuGai-diagnostics-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8)
$report = Join-Path $output $name
[IO.Directory]::CreateDirectory($report) | Out-Null

function Write-Json($Value, [string]$RelativePath) {
    [IO.File]::WriteAllText((Join-Path $report $RelativePath), (ConvertTo-Json -InputObject $Value -Depth 16), $utf8)
}
function Copy-Evidence([string]$Source, [string]$RelativePath) {
    try {
        if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) { return }
        $item = Get-Item -LiteralPath $Source
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or $item.Length -gt 8MB) {
            throw 'Skipped reparse point or file larger than 8 MB.'
        }
        $destination = Join-Path $report $RelativePath
        [IO.Directory]::CreateDirectory((Split-Path -Parent $destination)) | Out-Null
        Copy-Item -LiteralPath $Source -Destination $destination
        $inventory.Add([pscustomobject]@{source=$Source; copy=$RelativePath; bytes=$item.Length;
            sha256=(Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()})
    } catch { $issues.Add("$Source : $($_.Exception.Message)") }
}
function File-Identity([string]$Path) {
    try {
        if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return }
        $item = Get-Item -LiteralPath $Path
        $signature = Get-AuthenticodeSignature -LiteralPath $Path
        [pscustomobject]@{path=$Path; bytes=$item.Length; modified_utc=$item.LastWriteTimeUtc.ToString('o');
            sha256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant();
            file_version=$item.VersionInfo.FileVersion; product_version=$item.VersionInfo.ProductVersion;
            signature_status=$signature.Status.ToString();
            signer=$(if ($signature.SignerCertificate) { $signature.SignerCertificate.Subject } else { $null })}
    } catch { $issues.Add("$Path : $($_.Exception.Message)") }
}

$osKey = Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
Write-Json ([ordered]@{format='wugai-install-diagnostics-v1'; collected_utc=[DateTime]::UtcNow.ToString('o');
    install_root=$InstallRoot; os_build=$osKey.CurrentBuildNumber; os_revision=$osKey.UBR;
    os_display_version=$osKey.DisplayVersion; os_64bit=[Environment]::Is64BitOperatingSystem;
    process_64bit=[Environment]::Is64BitProcess; powershell=$PSVersionTable.PSVersion.ToString();
    elevated=(New-Object Security.Principal.WindowsPrincipal($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator);
    note='Only WuGai installer evidence was read. No installer, repair, or registry write was executed.'}) 'environment.json'
$identity.Dispose()

$logs = Join-Path $env:LOCALAPPDATA 'LiuKai\installer-logs'
if (Test-Path -LiteralPath $logs -PathType Container) {
    Get-ChildItem -LiteralPath $logs -File -Filter '*.log' | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 10 | ForEach-Object {
        Copy-Evidence $_.FullName ('logs\' + $_.Name)
    }
}
Copy-Evidence (Join-Path $InstallRoot '.liukai-install-state') 'installed\install-state.txt'
Copy-Evidence (Join-Path $InstallRoot 'manifest.json') 'installed\manifest.json'
$identities = @(File-Identity (Join-Path $InstallRoot 'LiuKaiInstaller.exe'); File-Identity (Join-Path $InstallRoot 'shared\branding.dll'))
if ($InstallerPath) {
    $identities += @(File-Identity ([IO.Path]::GetFullPath($InstallerPath)))
} else {
    $downloads = Join-Path $env:USERPROFILE 'Downloads'
    if (Test-Path -LiteralPath $downloads) {
        Get-ChildItem -LiteralPath $downloads -File -Filter 'WuGai-*-Setup*.exe' | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 5 | ForEach-Object {
            $identities += @(File-Identity $_.FullName)
        }
    }
}
$setupRoots = @(Get-ChildItem -LiteralPath ([IO.Path]::GetTempPath()) -Directory -Filter 'LiuKai-Setup-*' |
    Where-Object { ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0 } |
    Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 5)
foreach ($setupRoot in $setupRoots) {
    Copy-Evidence (Join-Path $setupRoot.FullName 'release-manifest.json') ('bundles\' + $setupRoot.Name + '\release-manifest.json')
    Copy-Evidence (Join-Path $setupRoot.FullName 'package\manifest.json') ('bundles\' + $setupRoot.Name + '\package-manifest.json')
    $identities += @(File-Identity (Join-Path $setupRoot.FullName 'package\LiuKaiInstaller.exe'))
}
Write-Json @($identities) 'file-identities.json'

# Only this product's machine-COM transaction directories, never the whole Program Files tree.
$ledger = Join-Path (Split-Path -Parent $InstallRoot) '.liukai-installer-ledger-machine-com-x64\LiuKai-TSF-v1'
if (Test-Path -LiteralPath $ledger -PathType Container) {
    $transactions = @(Get-ChildItem -LiteralPath $ledger -Directory |
        Where-Object { $_.Name -match '^[0-9a-fA-F]{32}$' -and ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0 } |
        Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 32)
    foreach ($transaction in $transactions) {
        foreach ($file in @('transaction.journal','transaction.snapshot','terminal-compensation.journal','terminal-registration-compensation.journal')) {
            Copy-Evidence (Join-Path $transaction.FullName $file) ('transactions\' + $transaction.Name + '\' + $file)
        }
    }
}

# Selected product keys only; preserve the value types and raw binary marker bytes.
$clsid = '{5D7C4291-2B34-4CE8-8A11-39627105C42E}'
$keys = @("Software\Classes\CLSID\$clsid", "Software\Classes\CLSID\$clsid\InprocServer32",
    "Software\Microsoft\CTF\TIP\$clsid\LanguageProfile\0x00000404\{7254A21F-6EB1-4217-A42C-C27301175D90}",
    "Software\LiuKai\Installer\$clsid\Profile", "Software\LiuKai\Installer\$clsid\KeyboardCategory",
    "Software\LiuKai\Installer\$clsid\SystemTrayCategory", "Software\LiuKai\Installer\$clsid\UIElementCategory")
$registry = @()
foreach ($hive in @([Microsoft.Win32.RegistryHive]::CurrentUser, [Microsoft.Win32.RegistryHive]::LocalMachine)) {
    foreach ($view in @([Microsoft.Win32.RegistryView]::Registry64, [Microsoft.Win32.RegistryView]::Registry32)) {
        $base = $null
        try {
            $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey($hive, $view)
            foreach ($keyPath in $keys) {
                $key = $null
                try {
                    $key = $base.OpenSubKey($keyPath, $false)
                    $values = @()
                    if ($key) {
                        foreach ($valueName in $key.GetValueNames()) {
                            $value = $key.GetValue($valueName, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
                            if ($value -is [byte[]]) { $value = [Convert]::ToBase64String($value) }
                            $values += [pscustomobject]@{name=$valueName; kind=$key.GetValueKind($valueName).ToString(); value=$value}
                        }
                    }
                    $registry += [pscustomobject]@{hive=$hive.ToString(); view=$view.ToString(); path=$keyPath; present=($null -ne $key); values=$values}
                } catch { $issues.Add("Registry $hive/$view/$keyPath : $($_.Exception.Message)") }
                finally { if ($key) { $key.Dispose() } }
            }
        } catch { $issues.Add("Registry $hive/$view : $($_.Exception.Message)") }
        finally { if ($base) { $base.Dispose() } }
    }
}
Write-Json @($registry) 'registry.json'

# Relevant Defender detection/action events only. Lack of events is not a malware verdict.
try {
    $eventErrors = @()
    $events = @(Get-WinEvent -FilterHashtable @{LogName='Microsoft-Windows-Windows Defender/Operational';
        Id=1116,1117; StartTime=(Get-Date).AddDays(-2)} -MaxEvents 200 -ErrorAction SilentlyContinue -ErrorVariable eventErrors |
        Where-Object { $_.Message -match '(?i)WuGai|LiuKai|BoshiamyTsf' } |
        ForEach-Object { [pscustomobject]@{time_utc=$_.TimeCreated.ToUniversalTime().ToString('o'); id=$_.Id; message=$_.Message} })
    foreach ($eventError in $eventErrors) {
        if ($eventError.FullyQualifiedErrorId -notmatch '^NoMatchingEventsFound') { $issues.Add("Defender events: $eventError") }
    }
    Write-Json @($events) 'defender-events.json'
} catch { $issues.Add("Defender events: $($_.Exception.Message)") }
Write-Json ($inventory.ToArray()) 'evidence-files.json'
Write-Json ($issues.ToArray()) 'collection-issues.json'
# Summarize observed lifecycle results without claiming an unproven root cause.
function Format-HResult($Value) {
    if ($null -eq $Value) { return $null }
    try { return ('0x{0:X8}' -f [uint32]([int64]$Value -band [int64]4294967295)) } catch { return $null }
}
$observations = New-Object 'System.Collections.Generic.List[object]'
$copiedLogs = Join-Path $report 'logs'
if (Test-Path -LiteralPath $copiedLogs -PathType Container) {
    foreach ($file in @(Get-ChildItem -LiteralPath $copiedLogs -File -Filter '*.log' | Sort-Object Name)) {
        foreach ($line in @(Get-Content -LiteralPath $file.FullName -Encoding UTF8 -ErrorAction Stop)) {
            if (-not $line.TrimStart().StartsWith('{')) { continue }
            try { $entry = $line | ConvertFrom-Json -ErrorAction Stop } catch { continue }
            if ($entry.kind -cne 'windows-installer-lifecycle') { continue }
            $observations.Add([pscustomobject]@{ log=$file.Name; scope=$entry.scope;
                operation=$entry.operation; cleanup=$entry.cleanup; readback=$entry.readback;
                operation_hex=(Format-HResult $entry.operation); cleanup_hex=(Format-HResult $entry.cleanup) })
        }
    }
}
$transactionCopies = @($inventory | Where-Object { $_.copy -match '^transactions\\' }).Count
Write-Json ([ordered]@{format='wugai-diagnostics-summary-v1'; lifecycle_observations=$observations.ToArray();
    transaction_evidence_files=$transactionCopies; collection_issue_count=$issues.Count;
    note='Observed results only; absent evidence or an error code does not establish a root cause.'}) 'diagnosis.json'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = $report + '.zip'
[IO.Compression.ZipFile]::CreateFromDirectory($report, $archive, [IO.Compression.CompressionLevel]::Optimal, $false)
Write-Output "Diagnostic archive: $archive"
Write-Output 'Upload the ZIP from the computer where installation failed. Keep the original transaction files in place.'
if ($issues.Count -gt 0) { Write-Output "Collection notes: $($issues.Count). See collection-issues.json in the ZIP." }
