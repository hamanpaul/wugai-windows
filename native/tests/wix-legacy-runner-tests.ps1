param([Parameter(Mandatory=$true)][string]$ToolchainBin,[Parameter(Mandatory=$true)][string]$WorkDir)
$ErrorActionPreference='Stop';$root=[IO.Path]::GetFullPath($WorkDir);if(Test-Path $root){throw 'Test directory exists.'};$support=Join-Path $root 'support';$versionDir=Join-Path $root 'versions\test-v1';[IO.Directory]::CreateDirectory($support)|Out-Null;[IO.Directory]::CreateDirectory($versionDir)|Out-Null
$names=@('LiuKaiRegister.exe','support/recover-legacy-profile.ps1','support/legacy-prepared-root.ps1','support/legacy-recovery-policy.ps1','support/legacy-recovery-observe.ps1','support/legacy-recovery-transaction.ps1','support/legacy-recovery-adapter.ps1','support/legacy-recovery-backup.ps1','support/legacy-recovery-registry.ps1','support/legacy-owner-marker.ps1','support/legacy-history-probe.exe','support/collect-install-diagnostics.ps1')
$proof=@();foreach($name in $names){$file=Join-Path $root $name.Replace('/','\');if($name -ceq 'support/recover-legacy-profile.ps1'){[IO.File]::WriteAllText($file,"Write-Output 'recovery-fixture-ok'; exit 0" )}else{[IO.File]::WriteAllText($file,'trusted '+$name)};$proof+=($name+'='+(Get-FileHash $file -Algorithm SHA256).Hash.ToLowerInvariant())}
$manifest=Join-Path $versionDir 'package-manifest.json';$helper=Join-Path $root 'LiuKaiInstaller.exe';[IO.File]::WriteAllText($manifest,'manifest');[IO.File]::WriteAllText($helper,'helper')
$manifestHash=(Get-FileHash $manifest -Algorithm SHA256).Hash.ToLowerInvariant();$helperHash=(Get-FileHash $helper -Algorithm SHA256).Hash.ToLowerInvariant();$exe=Join-Path (Split-Path $root -Parent) ('legacy-runner-'+[Guid]::NewGuid().ToString('N')+'.exe');$compiler=Join-Path (Resolve-Path $ToolchainBin).Path 'g++.exe'
& $compiler -std=c++17 -O2 -static -municode (Join-Path $PSScriptRoot 'wix-legacy-runner-tests.cpp') -lole32 -loleaut32 -luuid -ladvapi32 -lbcrypt -lshlwapi -o $exe
if($LASTEXITCODE -ne 0){throw 'Legacy runner test compilation failed.'}
$keyName='LiuKaiProbeTest_'+[Guid]::NewGuid().ToString('N')
if($keyName -cnotmatch '^LiuKaiProbeTest_[0-9a-f]{32}$'){throw 'Invalid test key name.'}
$software=[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software',$true);$createdByTest=$false
try {
 $existing=$software.OpenSubKey($keyName,$false);if($existing){$existing.Dispose();throw 'Test registry key already exists.'}
 $created=$software.CreateSubKey($keyName);$createdByTest=$true;$created.Dispose()
 & $exe $root ($proof -join ';') 'test-v1' $manifestHash $helperHash ('Software\'+$keyName)
 if($LASTEXITCODE -ne 0){throw "Legacy runner test failed: $LASTEXITCODE"}
} finally {if($software){if($createdByTest){$software.DeleteSubKeyTree($keyName,$false)};$software.Dispose()}}
# The production action holds read-only locks to prevent replacement while the
# recovery script and its two helper executables are running.
$script=Join-Path $root 'locked-test.ps1';[IO.File]::WriteAllText($script,"exit 0`r`n");$stream=[IO.File]::Open($script,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
try{& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $script;if($LASTEXITCODE -ne 0){throw 'Locked PowerShell script did not run.'}}finally{$stream.Dispose()}
$fixture=Join-Path (Split-Path $root -Parent) ('locked-exe-'+[Guid]::NewGuid().ToString('N')+'.exe');$code=Join-Path $root 'locked-exe.cpp';[IO.File]::WriteAllText($code,'int main(){return 0;}');& $compiler -std=c++17 -O2 -static $code -o $fixture;if($LASTEXITCODE -ne 0){throw 'Locked executable fixture compilation failed.'}
$stream=[IO.File]::Open($fixture,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
try{& $fixture;if($LASTEXITCODE -ne 0){throw 'Locked helper executable did not run.'}}finally{$stream.Dispose()}