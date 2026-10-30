param([Parameter(Mandatory=$true)][string]$ToolchainBin,[Parameter(Mandatory=$true)][string]$WorkDir,[Parameter(Mandatory=$true)][string]$Msi,[Parameter(Mandatory=$true)][string]$ExpectedScript)
$ErrorActionPreference='Stop';$work=[IO.Path]::GetFullPath($WorkDir);if(Test-Path $work){throw 'Test directory exists.'};[IO.Directory]::CreateDirectory($work)|Out-Null
$root=Join-Path $work 'mock install';$output=Join-Path $work 'diagnostic output';$log=Join-Path $work 'sample.log';[IO.File]::WriteAllText($log,"Log data not flushed yet`r`n",[Text.Encoding]::Unicode)
$exe=Join-Path $work 'runner.exe';$compiler=Join-Path (Resolve-Path $ToolchainBin).Path 'g++.exe'
& $compiler -std=c++17 -O2 -static -municode (Join-Path $PSScriptRoot 'wix-early-collector-tests.cpp') -lmsi -lole32 -loleaut32 -luuid -ladvapi32 -lbcrypt -lshlwapi -o $exe
if($LASTEXITCODE -ne 0){throw 'Early collector compilation failed.'}
$msiPath=(Resolve-Path $Msi).Path;$hash=(Get-FileHash -LiteralPath $ExpectedScript -Algorithm SHA256).Hash.ToLowerInvariant()
& $exe $msiPath $work $root $output $log ('0'*64)
if($LASTEXITCODE -ne 23 -or (Test-Path $output)){throw 'Hash mismatch was not safely rejected.'}
if(@(Get-ChildItem $work -Directory -Filter '.LiuKai-diag-*' -Force).Count -ne 0){throw 'Staging directory was not cleaned on failure.'}
& $exe $msiPath $work $root $output $log $hash
if($LASTEXITCODE -ne 0){throw "Early collector failed: $LASTEXITCODE"}
if(@(Get-ChildItem $work -Directory -Filter '.LiuKai-diag-*' -Force).Count -ne 0){throw 'Staging directory was not cleaned after execution.'}
if(@(Get-ChildItem $output -Filter '*.zip' -File).Count -ne 1){throw 'Diagnostic ZIP missing.'}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive=Get-ChildItem -LiteralPath $output -Filter '*.zip' -File | Select-Object -First 1
$zip=[IO.Compression.ZipFile]::OpenRead($archive.FullName)
try{$entry=$zip.GetEntry('diagnosis.json');$reader=[IO.StreamReader]::new($entry.Open());try{$summary=$reader.ReadToEnd()|ConvertFrom-Json}finally{$reader.Dispose()};if(@($summary.wix_registration_failures).Count -ne 1 -or $summary.wix_registration_failures[0].stage -cne 'prepare-root' -or $summary.wix_registration_failures[0].hresult -cne '0x80070057'){throw 'Explicit early failure was not recorded.'}}finally{$zip.Dispose()}
'wix_early_collector_test_passed=true'