param([Parameter(Mandatory=$true)][string]$FixtureRoot,
      [Parameter(Mandatory=$true)][string]$ReadyFile)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$build=[IO.Path]::GetFullPath((Join-Path $repo 'build')).TrimEnd('\')
$root=[IO.Path]::GetFullPath($FixtureRoot)
$ready=[IO.Path]::GetFullPath($ReadyFile)
if (-not $root.StartsWith($build+'\',[StringComparison]::OrdinalIgnoreCase) -or
    -not $ready.StartsWith($build+'\',[StringComparison]::OrdinalIgnoreCase) -or
    [IO.Path]::GetFileName($root) -cne 'Installed WuGai') { throw 'Unsafe fixture path.' }
# Only two fixed test files are removed; no recursive filesystem operation.
[IO.File]::Delete((Join-Path $root 'shared\branding.dll'))
[IO.File]::Delete((Join-Path ($root+'.maintenance') 'Uninstall.exe'))
[IO.File]::WriteAllText($ready,'deletion-started')
Start-Sleep -Seconds 30
