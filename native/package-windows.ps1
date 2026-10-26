param(
    [Parameter(Mandatory=$true)][string]$X64BuildDir,
    [Parameter(Mandatory=$false)][string]$X86BuildDir,
    [Parameter(Mandatory=$true)][string]$CLI64Path,
    [Parameter(Mandatory=$true)][string]$BrandingDLLPath,
    [Parameter(Mandatory=$true)][string]$Version,
    [Parameter(Mandatory=$true)][string]$OutputDir,
    [Parameter(Mandatory=$false)][string]$AssetDir = (Join-Path $PSScriptRoot '..\assets'),
    [switch]$X64Only
)
$ErrorActionPreference = 'Stop'

function Get-CanonicalExistingFile([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Required input file is missing: $Path" }
    return (Resolve-Path -LiteralPath $Path).Path
}
function Get-CanonicalExistingDirectory([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) { throw "Required input directory is missing: $Path" }
    return (Resolve-Path -LiteralPath $Path).Path
}
function Test-PathOverlap([string]$A, [string]$B) {
    $aFull = [IO.Path]::GetFullPath($A).TrimEnd('\')
    $bFull = [IO.Path]::GetFullPath($B).TrimEnd('\')
    return $aFull.Equals($bFull, [StringComparison]::OrdinalIgnoreCase) -or
        $aFull.StartsWith($bFull + '\', [StringComparison]::OrdinalIgnoreCase) -or
        $bFull.StartsWith($aFull + '\', [StringComparison]::OrdinalIgnoreCase)
}
function Get-PeInfo([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 64 -or [BitConverter]::ToUInt16($bytes, 0) -ne 0x5a4d) { throw "Not a PE image: $Path" }
    $pe = [BitConverter]::ToUInt32($bytes, 0x3c)
    if ($pe -gt ($bytes.Length - 24) -or [BitConverter]::ToUInt32($bytes, [int]$pe) -ne 0x00004550) { throw "Invalid PE signature: $Path" }
    $machine = [BitConverter]::ToUInt16($bytes, [int]$pe + 4)
    $sections = [BitConverter]::ToUInt16($bytes, [int]$pe + 6)
    $optionalSize = [BitConverter]::ToUInt16($bytes, [int]$pe + 20)
    $characteristics = [BitConverter]::ToUInt16($bytes, [int]$pe + 22)
    $optional = [int]$pe + 24
    if ($optionalSize -lt 2 -or $optional + $optionalSize -gt $bytes.Length) { throw "Invalid PE optional header: $Path" }
    $magic = [BitConverter]::ToUInt16($bytes, $optional)
    $entryPoint = if ($optionalSize -ge 20) { [BitConverter]::ToUInt32($bytes, $optional + 16) } else { [uint32]::MaxValue }
    $sectionNames = @()
    $sectionOffset = $optional + $optionalSize
    if ($sectionOffset + ([int]$sections * 40) -gt $bytes.Length) { throw "Invalid PE section table: $Path" }
    for ($i = 0; $i -lt $sections; $i++) {
        $offset = $sectionOffset + ($i * 40)
        $name = [Text.Encoding]::ASCII.GetString($bytes, $offset, 8).Trim([char]0)
        $flags = [BitConverter]::ToUInt32($bytes, $offset + 36)
        $sectionNames += [PSCustomObject]@{ name=$name; characteristics=$flags }
    }
    return [PSCustomObject]@{
        machine=$machine; magic=$magic; characteristics=$characteristics
        entry_point=$entryPoint; sections=$sectionNames
    }
}
function Assert-PeArchitecture([string]$Path, [string]$Architecture, [bool]$RequireDll) {
    $info = Get-PeInfo $Path
    $machine = if ($Architecture -eq 'x64') { 0x8664 } else { 0x014c }
    $magic = if ($Architecture -eq 'x64') { 0x020b } else { 0x010b }
    if ($info.machine -ne $machine -or $info.magic -ne $magic) {
        throw "$Architecture PE architecture mismatch for $Path (machine=$('{0:x4}' -f $info.machine), optional=0x$('{0:x4}' -f $info.magic))"
    }
    $isDll = ($info.characteristics -band 0x2000) -ne 0
    if ($isDll -ne $RequireDll) { throw "PE DLL/executable type mismatch for $Path" }
    return $info
}

if ($Version -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$' -or $Version -eq '.' -or $Version -eq '..' -or $Version.Contains('..')) {
    throw "Unsafe package version: $Version"
}
$x64Root = Get-CanonicalExistingDirectory $X64BuildDir
$assetRoot = Get-CanonicalExistingDirectory $AssetDir
$architectures = [string[]]@('x64','x86')
if ($X64Only) { $architectures = [string[]]@('x64') }
$x86Root = $null
if (-not $X64Only) { $x86Root = Get-CanonicalExistingDirectory $X86BuildDir }
$cli = Get-CanonicalExistingFile $CLI64Path
$branding = Get-CanonicalExistingFile $BrandingDLLPath
$output = [IO.Path]::GetFullPath($OutputDir)
if (-not [IO.Path]::IsPathFullyQualified($OutputDir)) { throw 'OutputDir must be an absolute path' }
if (Test-Path -LiteralPath $output) { throw "Output already exists; refusing to overwrite: $output" }
$parent = Split-Path -Parent $output
if (-not (Test-Path -LiteralPath $parent -PathType Container)) { throw "Output parent directory must already exist: $parent" }
foreach ($sourceRoot in (@($x64Root, $x86Root) | Where-Object { $_ })) {
    if (Test-PathOverlap $output $sourceRoot) { throw "Output must not overlap an input directory: $sourceRoot" }
}
Assert-PeArchitecture $cli 'x64' $false | Out-Null
$brandInfo = Get-PeInfo $branding
$brandArchitectureMatches = ($brandInfo.machine -eq 0x014c -and $brandInfo.magic -eq 0x010b) -or ($brandInfo.machine -eq 0x8664 -and $brandInfo.magic -eq 0x020b)
if ($brandInfo.entry_point -ne 0 -or $brandInfo.sections.Count -ne 1 -or
    -not $brandArchitectureMatches -or ($brandInfo.characteristics -band 0x2000) -eq 0 -or
    $brandInfo.sections[0].name -ne '.rsrc' -or ($brandInfo.sections[0].characteristics -band 0x20000000) -ne 0) {
    throw 'Branding DLL must be resource-only: zero entry point, exactly one non-executable .rsrc section'
}

$requiredFiles = @('BoshiamyTsf.dll','LiuKaiLookup.exe','LiuKaiSettings.exe','dictionary.tsv','readings.tsv','recommended-codes.tsv','recommended-codes-source.json','phonetic-candidates.tsv','phonetic-candidates-source.json','THIRD_PARTY_NOTICES.md')
$assetFiles = @('phonetic-candidates.tsv','phonetic-candidates-source.json')
$resolved = @{}
foreach ($architecture in $architectures) {
    $root = if ($architecture -eq 'x64') { $x64Root } else { $x86Root }
    foreach ($name in $requiredFiles) {
        $sourceRoot = if ($assetFiles -contains $name) { $assetRoot } else { $root }
        $resolved["$architecture/$name"] = Get-CanonicalExistingFile (Join-Path $sourceRoot $name)
    }
    Assert-PeArchitecture $resolved["$architecture/BoshiamyTsf.dll"] $architecture $true | Out-Null
    Assert-PeArchitecture $resolved["$architecture/LiuKaiLookup.exe"] $architecture $false | Out-Null
    Assert-PeArchitecture $resolved["$architecture/LiuKaiSettings.exe"] $architecture $false | Out-Null
}
foreach ($name in @('dictionary.tsv','readings.tsv','recommended-codes.tsv','recommended-codes-source.json','phonetic-candidates.tsv','phonetic-candidates-source.json','THIRD_PARTY_NOTICES.md')) {
    $x64Hash = (Get-FileHash -LiteralPath $resolved["x64/$name"] -Algorithm SHA256).Hash
    if (-not $X64Only) {
        $x86Hash = (Get-FileHash -LiteralPath $resolved["x86/$name"] -Algorithm SHA256).Hash
        if ($x64Hash -ne $x86Hash) { throw "Architecture data mismatch for $name (x64=$x64Hash, x86=$x86Hash)" }
    }
}

$inputHashes = @{}
foreach ($file in @($resolved.Values) + @($cli, $branding)) {
    $inputHashes[$file] = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Copy-VerifiedInput([string]$Source, [string]$Destination) {
    Copy-Item -LiteralPath $Source -Destination $Destination
    if ((Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash.ToLowerInvariant() -ne $inputHashes[$Source]) {
        throw "Input changed while staging: $Source"
    }
}

$stage = Join-Path $parent ('.' + (Split-Path -Leaf $output) + '.stage-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null
try {
    foreach ($architecture in $architectures) {
        $destination = Join-Path $stage ("versions\$Version\$architecture")
        New-Item -ItemType Directory -Path $destination -Force | Out-Null
        foreach ($name in $requiredFiles) { Copy-VerifiedInput $resolved["$architecture/$name"] (Join-Path $destination $name) }
    }
    $shared = Join-Path $stage 'shared'
    New-Item -ItemType Directory -Path $shared | Out-Null
    Copy-VerifiedInput $branding (Join-Path $shared 'branding.dll')
    Copy-VerifiedInput $cli (Join-Path $stage 'LiuKaiInstaller.exe')
    $fileRows = @()
    Get-ChildItem -LiteralPath $stage -File -Recurse | Sort-Object FullName | ForEach-Object {
        $relative = $_.FullName.Substring($stage.Length).TrimStart('\').Replace('\','/')
        $fileRows += [ordered]@{ path=$relative; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(); bytes=$_.Length }
    }
    $manifest = [ordered]@{
        format='liukai-windows-package-v1'
        version=$Version
        installer_cli='LiuKaiInstaller.exe'
        branding='shared/branding.dll'
        architectures=$architectures
        files=$fileRows
        manifest_sha256_scope='All staged files except manifest.json itself.'
    }
    $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $stage 'manifest.json') -Encoding utf8
    if (Test-Path -LiteralPath $output) { throw "Output appeared during staging; refusing to overwrite: $output" }
    [IO.Directory]::Move($stage, $output)
} catch {
    $resolvedStage = [IO.Path]::GetFullPath($stage)
    $expectedParent = [IO.Path]::GetFullPath($parent).TrimEnd('\') + '\'
    if (-not $resolvedStage.StartsWith($expectedParent,[StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Parent $resolvedStage) -ne $parent -or
        (Split-Path -Leaf $resolvedStage) -notlike ('.' + (Split-Path -Leaf $output) + '.stage-*')) { throw 'Unsafe package staging cleanup path' }
    if (Test-Path -LiteralPath $resolvedStage) { Remove-Item -LiteralPath $resolvedStage -Recurse -Force }
    throw
}
Write-Output "Staged version $Version to $output"
