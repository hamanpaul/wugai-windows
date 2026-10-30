param([Parameter(Mandatory=$true)][string]$ToolchainBin,[Parameter(Mandatory=$true)][string]$WorkDir)
$ErrorActionPreference='Stop'
$compiler=Join-Path (Resolve-Path -LiteralPath $ToolchainBin).Path 'g++.exe'
$work=[IO.Path]::GetFullPath($WorkDir)
if(Test-Path -LiteralPath $work){throw 'Test working directory already exists.'}
[void][IO.Directory]::CreateDirectory($work)
$exe=Join-Path $work 'retired-files-tests.exe'
& $compiler -std=c++17 -O2 -static -municode (Join-Path $PSScriptRoot 'wix-retired-files-tests.cpp') -lbcrypt -lshlwapi -ladvapi32 -o $exe
if($LASTEXITCODE -ne 0){throw 'Retired file test compilation failed.'}
& $exe (Join-Path $work 'fixture')
if($LASTEXITCODE -ne 0){throw 'Retired file test failed.'}
