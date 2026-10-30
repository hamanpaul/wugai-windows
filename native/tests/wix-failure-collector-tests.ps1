param([Parameter(Mandatory=$true)][string]$ToolchainBin,[Parameter(Mandatory=$true)][string]$WorkDir)
$ErrorActionPreference='Stop'
$work=[IO.Path]::GetFullPath($WorkDir);if(Test-Path -LiteralPath $work){throw 'Test directory exists.'}
$root=Join-Path $work 'product folder';$support=Join-Path $root 'support';[IO.Directory]::CreateDirectory($support)|Out-Null
$source=Join-Path $PSScriptRoot '..\collect-install-diagnostics.ps1';$script=Join-Path $support 'collect-install-diagnostics.ps1';Copy-Item -LiteralPath $source -Destination $script
$log=Join-Path $work 'log file.log';[IO.File]::WriteAllText($log,"LiuKai registration: history HRESULT=2147947423`r`n",[Text.Encoding]::Unicode)
$exe=Join-Path $work 'runner.exe';$compiler=Join-Path (Resolve-Path $ToolchainBin).Path 'g++.exe'
& $compiler -std=c++17 -O2 -static -municode (Join-Path $PSScriptRoot 'wix-failure-collector-tests.cpp') -lole32 -loleaut32 -luuid -ladvapi32 -lbcrypt -lshlwapi -o $exe
if($LASTEXITCODE -ne 0){throw 'Failure collector harness compilation failed.'}
$output=Join-Path $work 'diagnostics folder';$hash=(Get-FileHash -LiteralPath $script -Algorithm SHA256).Hash.ToLowerInvariant()
& $exe $root $output $log ('0'*64)
if($LASTEXITCODE -ne 23 -or (Test-Path $output)){throw 'Unverified collector script was not rejected.'}
& $exe $root $output $log $hash
if($LASTEXITCODE -ne 0){throw "Collector execution failed: $LASTEXITCODE"}
$zips=@(Get-ChildItem -LiteralPath $output -Filter '*.zip' -File)
if($zips.Count -ne 1){throw 'Collector did not produce exactly one ZIP.'}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip=[IO.Compression.ZipFile]::OpenRead($zips[0].FullName)
try{if($null -eq $zip.GetEntry('logs\wix-msi.log') -or $null -eq $zip.GetEntry('diagnosis.json')){throw 'Archive missing MSI evidence or summary.'}}finally{$zip.Dispose()}
'wix_failure_collector_test_passed=true'