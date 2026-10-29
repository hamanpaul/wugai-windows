param([Parameter(Mandatory=$true)][string]$TestExe,
      [Parameter(Mandatory=$true)][string]$ReleaseRoot)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$build=[IO.Path]::GetFullPath((Join-Path $repo 'build')).TrimEnd('\')
$dist=[IO.Path]::GetFullPath((Join-Path $repo 'dist')).TrimEnd('\')
$source=(Resolve-Path -LiteralPath $TestExe).Path
$release=(Resolve-Path -LiteralPath $ReleaseRoot).Path
if(-not $source.StartsWith($build+'\',[StringComparison]::OrdinalIgnoreCase) -or
   -not $release.StartsWith($dist+'\',[StringComparison]::OrdinalIgnoreCase) -or
   [IO.Path]::GetFileName($source) -cne 'test.exe') {throw 'Fixtures must belong to this workspace.'}
$bundle=Join-Path $release 'bundle'
$manifest=Get-Content -LiteralPath (Join-Path $bundle 'release-manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if($manifest.format -cne 'liukai-release-v1' -or $manifest.version -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$' -or
   $manifest.version.Contains('..')) {throw 'Invalid release identity.'}
$prefix='package/versions/'+$manifest.version+'/x64/'
foreach($name in @('BoshiamyTsf.dll','dictionary.tsv')) {
    $relative=$prefix+$name
    $rows=@($manifest.files | Where-Object {$_.path -ceq $relative})
    if($rows.Count -ne 1) {throw 'Release is missing a unique input-method file.'}
    $file=Join-Path $bundle $relative.Replace('/','\')
    if((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -cne $rows[0].sha256 -or
       (Get-Item -LiteralPath $file).Length -ne $rows[0].bytes) {throw 'Input-method payload hash mismatch.'}
}
$dll=Join-Path $bundle ($prefix+'BoshiamyTsf.dll').Replace('/','\')
$sandbox=Join-Path (Join-Path $build 'tests') ('tsf-binary-fixture-'+[Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($sandbox) | Out-Null
$test=Join-Path $sandbox 'test.exe'
Copy-Item -LiteralPath $source -Destination $test
if((Get-FileHash -LiteralPath $source).Hash -cne (Get-FileHash -LiteralPath $test).Hash) {throw 'Test executable copy mismatch.'}
$stdout=Join-Path $sandbox 'stdout.txt';$stderr=Join-Path $sandbox 'stderr.txt'
$child=Start-Process -FilePath $test -ArgumentList @('"'+$sandbox+'"','"'+$dll+'"') `
    -WindowStyle Hidden -Wait -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
$output=Get-Content -LiteralPath $stdout -Raw
if($child.ExitCode -ne 0 -or $output -notmatch 'packaged TSF DLL committed two Chinese characters') {
    $failure=Get-Content -LiteralPath $stderr -Raw
    throw ('Isolated TSF binary test failed: '+$failure)
}
if(Test-Path -LiteralPath (Join-Path $sandbox 'LiuKai')) {throw 'Test settings were not removed.'}
[pscustomobject]@{version=$manifest.version;test_sandbox=$sandbox;tip_registered=$false;
    actual_search_or_store_tested=$false;result=$output.Trim()} | ConvertTo-Json
