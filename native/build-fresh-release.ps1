param(
    [Parameter(Mandatory=$true)][string]$ToolchainBin,
    [Parameter(Mandatory=$true)][string]$Version,
    [Parameter(Mandatory=$true)][string]$OutputDir,
    [Parameter(Mandatory=$true)][string]$SetupName
)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$bin=(Resolve-Path -LiteralPath $ToolchainBin).Path
$compiler=(Resolve-Path -LiteralPath (Join-Path $bin 'g++.exe')).Path
$windres=(Resolve-Path -LiteralPath (Join-Path $bin 'windres.exe')).Path
$output=[IO.Path]::GetFullPath($OutputDir)
$work=$output+'.build'
if((Test-Path -LiteralPath $output) -or (Test-Path -LiteralPath $work)){throw 'Output or build directory already exists.'}
if(-not (Test-Path -LiteralPath (Split-Path -Parent $output) -PathType Container)){throw 'Output parent does not exist.'}
if($Version -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$' -or $Version.Contains('..')){throw 'Invalid package version.'}
if($SetupName -notmatch '^WuGai-[A-Za-z0-9._-]+-x64-Setup\.exe$'){throw 'Invalid setup name.'}
$oldPath=$env:PATH
try {
    $env:PATH=$bin+';'+$oldPath
    $target=(& $compiler -dumpmachine | Out-String).Trim()
    if($LASTEXITCODE -ne 0 -or $target -cne 'x86_64-w64-mingw32'){throw 'A working x64 MinGW toolchain is required.'}
    [IO.Directory]::CreateDirectory($work)|Out-Null
    $x64=Join-Path $work 'x64'; [IO.Directory]::CreateDirectory($x64)|Out-Null
    $resource=Join-Path $work 'resources.o'
    & $windres -I (Join-Path $repo 'assets') (Join-Path $PSScriptRoot 'tsf\resources.rc') -O coff -F pe-x86-64 -o $resource
    if($LASTEXITCODE -ne 0){throw 'TSF resource compilation failed.'}
    $cpp=@('-std=c++17','-O2','-static')
    & $compiler @cpp -shared -municode (Join-Path $PSScriptRoot 'tsf\service.cpp') (Join-Path $PSScriptRoot 'tsf\service.def') $resource -lole32 -loleaut32 -luuid -lgdi32 -o (Join-Path $x64 'BoshiamyTsf.dll')
    if($LASTEXITCODE -ne 0){throw 'TSF DLL compilation failed.'}
    foreach($tool in @('lookup','settings')) {
        $name=if($tool -eq 'lookup'){'LiuKaiLookup.exe'}else{'LiuKaiSettings.exe'}
        & $compiler @cpp -municode -mwindows (Join-Path $PSScriptRoot ('tools\'+$tool+'_main.cpp')) $resource -lcomdlg32 -lcomctl32 -lshell32 -lgdi32 -o (Join-Path $x64 $name)
        if($LASTEXITCODE -ne 0){throw "Compilation failed: $name"}
    }
    $cli=Join-Path $work 'LiuKaiInstaller.exe'; $register=Join-Path $work 'LiuKaiRegister.exe'; $branding=Join-Path $work 'branding.dll'
    & $compiler @cpp -municode (Join-Path $PSScriptRoot 'installer\lifecycle_main.cpp') -lole32 -loleaut32 -luuid -ladvapi32 -lbcrypt -lshlwapi -o $cli
    if($LASTEXITCODE -ne 0){throw 'Installer CLI compilation failed.'}
    & $compiler @cpp -municode (Join-Path $PSScriptRoot 'tools\register_main.cpp') -lole32 -loleaut32 -luuid -ladvapi32 -o $register
    if($LASTEXITCODE -ne 0){throw 'Registration tool compilation failed.'}
    & (Join-Path $PSScriptRoot 'build-branding.ps1') -Architecture x64 -Compiler $compiler -ResourceCompiler $windres -OutputDll $branding
    foreach($name in @('dictionary.tsv','readings.tsv','recommended-codes.tsv','recommended-codes-source.json','phonetic-candidates.tsv','phonetic-candidates-source.json','THIRD_PARTY_NOTICES.md')) {
        Copy-Item -LiteralPath (Join-Path $repo ('assets\'+$name)) -Destination $x64
    }
    $seed=Join-Path $work 'seed'; [IO.Directory]::CreateDirectory($seed)|Out-Null
    & (Join-Path $PSScriptRoot 'package-windows.ps1') -X64Only -X64BuildDir $x64 -CLI64Path $cli -BrandingDLLPath $branding -AssetDir $x64 -Version $Version -OutputDir (Join-Path $seed 'package')
    Copy-Item -LiteralPath $register -Destination $seed
    $rows=@(Get-ChildItem -LiteralPath $seed -File -Recurse | Sort-Object FullName | ForEach-Object {
        [ordered]@{path=$_.FullName.Substring($seed.Length+1).Replace('\','/');sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant();bytes=$_.Length}
    })
    $manifest=[ordered]@{format='liukai-release-v1';architecture='x64';version=$Version;registration_scope='user-and-machine-com-x64-v1';files=$rows}
    [IO.File]::WriteAllText((Join-Path $seed 'release-manifest.json'),($manifest|ConvertTo-Json -Depth 6),(New-Object Text.UTF8Encoding($false)))
    & (Join-Path $PSScriptRoot 'build-release.ps1') -SourceBundle $seed -InstallerCLI $cli -X64BuildDir $x64 -Version $Version -OutputDir $output -SetupName $SetupName
} finally {$env:PATH=$oldPath}
