param(
    [Parameter(Mandatory=$true)][string]$SourceBundle,
    [Parameter(Mandatory=$true)][string]$InstallerCLI,
    [Parameter(Mandatory=$true)][string]$Version,
    [Parameter(Mandatory=$true)][string]$OutputDir,
    [string]$SetupName = 'WuGai-R60.1-x64-Setup.exe'
)
$ErrorActionPreference = 'Stop'
$bundle = (Resolve-Path -LiteralPath $SourceBundle).Path
$cli = (Resolve-Path -LiteralPath $InstallerCLI).Path
$output = [IO.Path]::GetFullPath($OutputDir)
if (Test-Path -LiteralPath $output) { throw 'Output directory already exists.' }
if ($SetupName -notmatch '^[A-Za-z0-9._-]+\.exe$') { throw 'Invalid setup filename.' }
$parent = Split-Path -Parent $output
if (-not (Test-Path -LiteralPath $parent -PathType Container)) { throw 'Output parent must exist.' }
$release = Get-Content -LiteralPath (Join-Path $bundle 'release-manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if ($release.format -cne 'liukai-release-v1' -or $release.architecture -cne 'x64' -or
    $release.registration_scope -cne 'user-and-machine-com-x64-v1') { throw 'Unsupported source release.' }
foreach ($row in $release.files) {
    if ($row.path -notmatch '^[A-Za-z0-9._/-]+$' -or $row.path.Contains('..') -or $row.path.StartsWith('/')) { throw 'Invalid source path.' }
    $source = Join-Path $bundle $row.path.Replace('/','\')
    if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant() -cne $row.sha256 -or
        (Get-Item -LiteralPath $source).Length -ne $row.bytes) { throw "Source release mismatch: $($row.path)" }
}
$sourcePackage = Join-Path $bundle 'package'
$sourceManifest = Get-Content -LiteralPath (Join-Path $sourcePackage 'manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$sourceBuild = Join-Path $sourcePackage "versions\$($sourceManifest.version)\x64"
New-Item -ItemType Directory -Path $output | Out-Null
$payload = Join-Path $output 'bundle'
New-Item -ItemType Directory -Path $payload | Out-Null
& (Join-Path $PSScriptRoot 'package-windows.ps1') -X64Only -X64BuildDir $sourceBuild `
    -CLI64Path $cli -BrandingDLLPath (Join-Path $sourcePackage 'shared\branding.dll') `
    -AssetDir $sourceBuild -Version $Version -OutputDir (Join-Path $payload 'package')
Copy-Item -LiteralPath (Join-Path $bundle 'LiuKaiRegister.exe') -Destination $payload
foreach ($name in @('install-release.ps1','package-install-windows.ps1')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $payload
}
$files = @(Get-ChildItem -LiteralPath $payload -File -Recurse | Sort-Object FullName | ForEach-Object {
    [ordered]@{path=$_.FullName.Substring($payload.Length+1).Replace('\','/');
        sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(); bytes=$_.Length}
})
$manifest = [ordered]@{format='liukai-release-v1'; architecture='x64'; version=$Version;
    registration_scope='user-and-machine-com-x64-v1'; files=$files}
$utf8 = New-Object Text.UTF8Encoding($false)
[IO.File]::WriteAllText((Join-Path $payload 'release-manifest.json'), ($manifest | ConvertTo-Json -Depth 6), $utf8)
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = Join-Path $output 'payload.zip'
[IO.Compression.ZipFile]::CreateFromDirectory($payload, $zip, [IO.Compression.CompressionLevel]::Optimal, $false)
$framework = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319'
$csc = Join-Path $framework 'csc.exe'
$setup = Join-Path $output $SetupName
& $csc /nologo /target:winexe /platform:x64 /optimize+ ("/out:$setup") ("/resource:$zip,payload.zip") `
    ("/reference:"+(Join-Path $framework 'System.IO.Compression.dll')) `
    ("/reference:"+(Join-Path $framework 'System.IO.Compression.FileSystem.dll')) `
    ("/reference:"+(Join-Path $framework 'System.Windows.Forms.dll')) (Join-Path $PSScriptRoot 'package\Setup.cs')
if ($LASTEXITCODE -ne 0) { throw 'Setup compilation failed.' }
$hash = (Get-FileHash -LiteralPath $setup -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText((Join-Path $output 'SHA256SUMS.txt'), "$hash  $SetupName`n", $utf8)
[pscustomobject]@{setup=$setup; version=$Version; sha256=$hash;
    reused_payload_version=$sourceManifest.version; rebuilt_component='LiuKaiInstaller.exe'} | ConvertTo-Json
