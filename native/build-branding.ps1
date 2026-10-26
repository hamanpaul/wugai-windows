param(
    [Parameter(Mandatory=$true)][string]$OutputDll,
    [ValidateSet('x64','x86')][string]$Architecture = 'x64',
    [string]$Compiler = '',
    [string]$ResourceCompiler = ''
)
$ErrorActionPreference = 'Stop'
if (-not $Compiler) {
    $Compiler = if ($Architecture -eq 'x86') {
        'C:\msys64\mingw32\bin\g++.exe'
    } else { 'C:\msys64\mingw64\bin\g++.exe' }
}
$Compiler = (Resolve-Path -LiteralPath $Compiler).Path
if (-not $ResourceCompiler) { $ResourceCompiler = Join-Path (Split-Path $Compiler -Parent) 'windres.exe' }
$ResourceCompiler = (Resolve-Path -LiteralPath $ResourceCompiler).Path
$Ld = Join-Path (Split-Path $Compiler -Parent) 'ld.exe'
$Ld = (Resolve-Path -LiteralPath $Ld).Path
$OutputDll = [IO.Path]::GetFullPath($OutputDll)
if (Test-Path -LiteralPath $OutputDll) { throw "Output already exists: $OutputDll" }
$parent = Split-Path -Parent $OutputDll
if (-not (Test-Path -LiteralPath $parent -PathType Container)) { throw "Output parent does not exist: $parent" }
$expectedTarget = if ($Architecture -eq 'x86') { 'i686-w64-mingw32' } else { 'x86_64-w64-mingw32' }
$target = (& $Compiler -dumpmachine | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $target -ne $expectedTarget) { throw "Compiler target '$target' does not match $Architecture" }
$format = if ($Architecture -eq 'x86') { 'pe-i386' } else { 'pe-x86-64' }
$outputFormat = if ($Architecture -eq 'x86') { 'pei-i386' } else { 'pei-x86-64' }
$temp = Join-Path ([IO.Path]::GetTempPath()) ('liu-kai-branding-' + [guid]::NewGuid().ToString('N'))
$resourceSources = @($PSCommandPath,
    (Join-Path $PSScriptRoot 'package\branding.rc'),
    (Join-Path $PSScriptRoot 'package\display_name.rc'),
    (Join-Path $PSScriptRoot '..\assets\liukai.ico'))
$sourceHashes = @{}
function Get-BrandingSourceHash([string]$SourcePath) {
    $stream = [IO.File]::OpenRead($SourcePath)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($algorithm.ComputeHash($stream)) }
    finally { $algorithm.Dispose(); $stream.Dispose() }
}
foreach ($source in $resourceSources) {
    $sourceHashes[$source] = Get-BrandingSourceHash $source
}
New-Item -ItemType Directory -Path $temp | Out-Null
$previousPath = $env:PATH
$builtDll = Join-Path $temp 'branding.dll'
try {
    $env:PATH = (Split-Path $Compiler -Parent) + ';' + $previousPath
    $resourceObject = Join-Path $temp 'branding.o'
    $iconDir = Join-Path $PSScriptRoot '..\assets'
    & $ResourceCompiler -I $iconDir (Join-Path $PSScriptRoot 'package\branding.rc') -O coff -F $format -o $resourceObject
    if ($LASTEXITCODE -ne 0) { throw 'windres failed to compile branding icon resource' }
    $linkerScript = Join-Path $temp 'resource-only.ld'
    @"
OUTPUT_FORMAT($outputFormat)
SECTIONS
{
  . = SIZEOF_HEADERS;
  . = ALIGN(__section_alignment__);
  .rsrc __image_base__ + __section_alignment__ : ALIGN(4)
  {
    KEEP (*(.rsrc))
    KEEP (*(.rsrc`$*))
  }
  /DISCARD/ : { *(*) }
}
"@ | Set-Content -LiteralPath $linkerScript -Encoding ascii
    & $Ld -dll --subsystem windows -e 0 -s $resourceObject -o $builtDll -T $linkerScript
    if ($LASTEXITCODE -ne 0) { throw 'GNU ld failed to link resource-only branding DLL' }
    $bytes = [IO.File]::ReadAllBytes($builtDll)
    if ($bytes.Length -lt 64 -or [BitConverter]::ToUInt16($bytes,0) -ne 0x5a4d) { throw 'Branding output has no DOS header' }
    $pe = [BitConverter]::ToUInt32($bytes,0x3c)
    if ($pe -gt ($bytes.Length - 24) -or [BitConverter]::ToUInt32($bytes,[int]$pe) -ne 0x00004550) { throw 'Branding output has no PE signature' }
    $machine = [BitConverter]::ToUInt16($bytes,[int]$pe + 4)
    $expectedMachine = if ($Architecture -eq 'x86') { 0x014c } else { 0x8664 }
    $characteristics = [BitConverter]::ToUInt16($bytes,[int]$pe + 22)
    $sectionCount = [BitConverter]::ToUInt16($bytes,[int]$pe + 6)
    $optionalSize = [BitConverter]::ToUInt16($bytes,[int]$pe + 20)
    $optional = [int]$pe + 24
    $magic = [BitConverter]::ToUInt16($bytes,$optional)
    $expectedMagic = if ($Architecture -eq 'x86') { 0x010b } else { 0x020b }
    $entryPoint = [BitConverter]::ToUInt32($bytes,$optional + 16)
    if ($machine -ne $expectedMachine -or $magic -ne $expectedMagic -or ($characteristics -band 0x2000) -eq 0 -or $entryPoint -ne 0 -or $sectionCount -ne 1) { throw 'Branding output is not a zero-entrypoint, single-section DLL PE for the requested architecture' }
    $section = $optional + $optionalSize
    $sectionName = [Text.Encoding]::ASCII.GetString($bytes,$section,8).Trim([char]0)
    $sectionFlags = [BitConverter]::ToUInt32($bytes,$section + 36)
    if ($sectionName -ne '.rsrc' -or ($sectionFlags -band 0x20000000) -ne 0) { throw 'Branding output contains code or non-resource sections' }
    foreach ($source in $resourceSources) {
        if ((Get-BrandingSourceHash $source) -ne $sourceHashes[$source]) {
            throw "Branding source changed during build: $source"
        }
    }
    [IO.File]::Move($builtDll,$OutputDll)
} catch {
    throw
} finally {
    $env:PATH = $previousPath
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    $resolvedTemp = [IO.Path]::GetFullPath($temp)
    if (-not $resolvedTemp.StartsWith($tempRoot,[StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $resolvedTemp) -notlike 'liu-kai-branding-*') { throw 'Unsafe branding cleanup path' }
    Remove-Item -LiteralPath $resolvedTemp -Recurse -Force
}
