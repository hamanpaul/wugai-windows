param([Parameter(Mandatory=$true)][string]$WorkDir)
$ErrorActionPreference='Stop';. (Join-Path $PSScriptRoot '..\wix\legacy-prepared-root.ps1')
$work=[IO.Path]::GetFullPath($WorkDir);if(Test-Path -LiteralPath $work){throw 'Test directory exists.'};[IO.Directory]::CreateDirectory($work)|Out-Null
function New-Fixture([string]$name) {
 $root=Join-Path $work $name;$payload=Join-Path $root 'versions\test-v1\x64';$support=Join-Path $root 'support';[IO.Directory]::CreateDirectory($payload)|Out-Null;[IO.Directory]::CreateDirectory($support)|Out-Null
 $file=Join-Path $payload 'BoshiamyTsf.dll';$extra=Join-Path $support 'helper.ps1';[IO.File]::WriteAllText($file,'verified payload');[IO.File]::WriteAllText($extra,'verified support')
 $hash=(Get-FileHash $file -Algorithm SHA256).Hash.ToLowerInvariant();$extraHash=(Get-FileHash $extra -Algorithm SHA256).Hash.ToLowerInvariant()
 $doc=[ordered]@{format='liukai-windows-package-v1';version='test-v1';architectures=@('x64');files=@(@{path='versions/test-v1/x64/BoshiamyTsf.dll';sha256=$hash;bytes=16})}
 $manifest=Join-Path $root 'versions\test-v1\package-manifest.json';[IO.File]::WriteAllText($manifest,($doc|ConvertTo-Json -Depth 5),[Text.UTF8Encoding]::new($false));$manifestHash=(Get-FileHash $manifest -Algorithm SHA256).Hash.ToLowerInvariant()
 return @{Root=$root;Version='test-v1';ManifestSha256=$manifestHash;ExtraHashes=@{'support/helper.ps1'=$extraHash}}
}
function MustFail($case){$failed=$false;try{Assert-PreparedMsiRoot @case | Out-Null}catch{$failed=$true};if(-not $failed){throw 'Invalid prepared root was accepted.'}}
$valid=New-Fixture 'valid';if(-not (Assert-PreparedMsiRoot @valid)){throw 'Valid root rejected.'}
$handles=New-Object 'System.Collections.Generic.List[System.IO.FileStream]'
try {
 foreach($file in @(Get-ChildItem -LiteralPath $valid.Root -Recurse -File)){$handles.Add([IO.File]::Open($file.FullName,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read))}
 if(-not (Assert-PreparedMsiRoot @valid)){throw 'Read-locked MSI files could not be verified.'}
} finally {foreach($handle in $handles){$handle.Dispose()}}
$case=New-Fixture 'extra-file';[IO.File]::WriteAllText((Join-Path $case.Root 'unexpected.txt'),'x');MustFail $case
$case=New-Fixture 'extra-dir';[IO.Directory]::CreateDirectory((Join-Path $case.Root 'foreign'))|Out-Null;MustFail $case
$case=New-Fixture 'tampered';[IO.File]::WriteAllText((Join-Path $case.Root 'support\helper.ps1'),'tampered');MustFail $case
$case=New-Fixture 'missing';Remove-Item -LiteralPath (Join-Path $case.Root 'support\helper.ps1');MustFail $case
$case=New-Fixture 'bad-manifest';$case.ManifestSha256='0'*64;MustFail $case
$case=New-Fixture 'bad-identity';$case.Version='..';MustFail $case
$names=@('LiuKaiRegister.exe','support/recover-legacy-profile.ps1','support/legacy-prepared-root.ps1','support/legacy-recovery-policy.ps1','support/legacy-recovery-observe.ps1','support/legacy-recovery-transaction.ps1','support/legacy-recovery-adapter.ps1','support/legacy-recovery-backup.ps1','support/legacy-recovery-registry.ps1','support/legacy-owner-marker.ps1','support/legacy-history-probe.exe','support/collect-install-diagnostics.ps1')
$proof=($names|ForEach-Object {$_+'='+('a'*64)}) -join ';'
if((ConvertFrom-PreparedMsiExtraHashes $proof).Count -ne 12){throw 'Complete support proof rejected.'}
foreach($invalid in @($proof+';support/foreign.ps1='+('a'*64),($names[0]+'='+('a'*64)),($proof+';'+$names[0]+'='+('a'*64)))){
 $rejected=$false;try{ConvertFrom-PreparedMsiExtraHashes $invalid|Out-Null}catch{$rejected=$true};if(-not $rejected){throw 'Invalid support proof accepted.'}
}
'wix_prepared_root_tests_passed=true'