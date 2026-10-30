param([Parameter(Mandatory=$true)][string]$ToolchainBin,[Parameter(Mandatory=$true)][string]$WorkDir)
$ErrorActionPreference='Stop'
$compiler=Join-Path (Resolve-Path -LiteralPath $ToolchainBin).Path 'g++.exe'
$work=[IO.Path]::GetFullPath($WorkDir)
if(Test-Path -LiteralPath $work){throw 'TLS test directory already exists.'}
[IO.Directory]::CreateDirectory($work)|Out-Null
$dll=Join-Path $work 'fixture.dll';$exe=Join-Path $work 'runner.exe'
& $compiler -std=c++17 -O2 -static -shared (Join-Path $PSScriptRoot 'wix-tls-detach-fixture.cpp') -o $dll
if($LASTEXITCODE -ne 0){throw 'TLS fixture build failed.'}
& $compiler -std=c++17 -O2 -static -municode (Join-Path $PSScriptRoot 'wix-tls-detach-runner.cpp') -o $exe
if($LASTEXITCODE -ne 0){throw 'TLS runner build failed.'}
$p=Start-Process -FilePath $exe -ArgumentList ('"'+$dll+'"') -WindowStyle Hidden -PassThru
if(-not $p.WaitForExit(10000)){$p.Kill();$p.WaitForExit();throw 'DLL thread cleanup timed out.'}
if($p.ExitCode -ne 0){throw "DLL thread cleanup failed: $($p.ExitCode)"}
'wix_tls_detach_test=passed'
