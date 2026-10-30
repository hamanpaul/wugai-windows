param([Parameter(Mandatory=$true)][string]$WorkDir)
$ErrorActionPreference='Stop'
$work=[IO.Path]::GetFullPath($WorkDir)
if(Test-Path -LiteralPath $work){throw 'Diagnostic test directory already exists.'}
[IO.Directory]::CreateDirectory($work)|Out-Null
$msi=Join-Path $work 'fixture-msi.log';$bundle=Join-Path $work 'fixture-bundle.log';$output=Join-Path $work 'reports';[IO.Directory]::CreateDirectory($output)|Out-Null
[IO.File]::WriteAllText($msi,"MSI fixture`r`nLiuKai registration: history HRESULT=2147947423`r`nCustomAction FailFixture returned actual error code 1603`r`n",[Text.Encoding]::Unicode)
[IO.File]::WriteAllText($bundle,"Burn fixture`r`n",[Text.Encoding]::UTF8)
$collector=Join-Path $PSScriptRoot '..\collect-install-diagnostics.ps1'
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $collector -OutputDirectory $output -MsiLog $msi -BundleLog $bundle
if($LASTEXITCODE -ne 0){throw 'Collector failed.'}
$archives=@(Get-ChildItem -LiteralPath $output -Filter '*.zip' -File)
if($archives.Count -ne 1){throw 'Expected one diagnostic archive.'}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip=[IO.Compression.ZipFile]::OpenRead($archives[0].FullName)
try {
 foreach($pair in @(@('logs\wix-msi.log',$msi),@('logs\wix-bundle.log',$bundle))){$entry=$zip.GetEntry($pair[0]);if($null -eq $entry){throw "Missing $($pair[0])"};$stream=$entry.Open();$sha=[Security.Cryptography.SHA256]::Create();try{$actual=[BitConverter]::ToString($sha.ComputeHash($stream));$expected=(Get-FileHash -LiteralPath $pair[1] -Algorithm SHA256).Hash;if($actual.Replace('-','') -cne $expected){throw 'Log contents differ.'}}finally{$sha.Dispose();$stream.Dispose()}}
 $entry=$zip.GetEntry('diagnosis.json');if($null -eq $entry){throw 'Missing diagnosis.'};$stream=$entry.Open();$reader=[IO.StreamReader]::new($stream);try{$diagnosis=$reader.ReadToEnd()|ConvertFrom-Json}finally{$reader.Dispose()}
 if(@($diagnosis.wix_registration_failures).Count -ne 1 -or $diagnosis.wix_registration_failures[0].stage -cne 'history' -or $diagnosis.wix_registration_failures[0].hresult -cne '0x8007139F'){throw 'Registration failure summary incorrect.'}
}finally{$zip.Dispose()}
$missing=Join-Path $work 'missing.log';$out2=Join-Path $work 'missing-report';[IO.Directory]::CreateDirectory($out2)|Out-Null
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $collector -OutputDirectory $out2 -MsiLog $missing
if($LASTEXITCODE -ne 0){throw 'Collector failed on missing log.'}
$archive2=Get-ChildItem -LiteralPath $out2 -Filter '*.zip' -File | Select-Object -First 1
$z2=[IO.Compression.ZipFile]::OpenRead($archive2.FullName)
try{$entry=$z2.GetEntry('collection-issues.json');$r=[IO.StreamReader]::new($entry.Open());try{$issues=$r.ReadToEnd()|ConvertFrom-Json}finally{$r.Dispose()};if('Specified MSI log was unavailable.' -cnotin @($issues)){throw 'Missing MSI log was not reported.'}}finally{$z2.Dispose()}
if(@($diagnosis.wix_action_failures).Count -ne 1 -or $diagnosis.wix_action_failures[0].action -cne 'FailFixture' -or $diagnosis.wix_action_failures[0].error_code -ne 1603){throw 'MSI custom action failure summary incorrect.'}
'wix_diagnostics_test_passed=true'