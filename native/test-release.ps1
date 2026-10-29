param([Parameter(Mandatory=$true)][string]$ToolchainBin,
      [Parameter(Mandatory=$true)][string]$WorkDir)
$ErrorActionPreference='Stop'
$bin=(Resolve-Path -LiteralPath $ToolchainBin).Path
$compiler=(Resolve-Path -LiteralPath (Join-Path $bin 'g++.exe')).Path
$work=[IO.Path]::GetFullPath($WorkDir)
if(Test-Path -LiteralPath $work){throw 'Test directory already exists.'}
[IO.Directory]::CreateDirectory($work)|Out-Null
$oldPath=$env:PATH
try {
    $env:PATH=$bin+';'+$oldPath
    foreach($name in @('engine_long_english_tests','candidate_ui_contract_tests','installer_durable_compensation_projection_tests')) {
        $source=Join-Path $PSScriptRoot ("tests\$name.cpp"); $exe=Join-Path $work ($name+'.exe')
        & $compiler -std=c++17 -O2 -static $source -lole32 -loleaut32 -luuid -lgdi32 -ladvapi32 -lbcrypt -lshlwapi -o $exe
        if($LASTEXITCODE -ne 0){throw "Compilation failed: $name"}
        & $exe
        if($LASTEXITCODE -ne 0){throw "Test failed: $name"}
    }
    $framework=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319'
    $setupTest=Join-Path $work 'setup-tests.exe'
    & (Join-Path $framework 'csc.exe') /nologo /target:exe /platform:x64 /main:SetupLogTests ("/out:$setupTest") `
        ("/reference:"+(Join-Path $framework 'System.IO.Compression.dll')) `
        ("/reference:"+(Join-Path $framework 'System.IO.Compression.FileSystem.dll')) `
        ("/reference:"+(Join-Path $framework 'System.Windows.Forms.dll')) `
        (Join-Path $PSScriptRoot 'package\Setup.cs') (Join-Path $PSScriptRoot 'tests\setup_log_tests.cs')
    if($LASTEXITCODE -ne 0){throw 'Setup test compilation failed.'}
    & $setupTest (Join-Path $work 'setup-fixture')
    if($LASTEXITCODE -ne 0){throw 'Setup tests failed.'}
    foreach($name in @('install-release-readback-tests.ps1','package-install-windows-tests.ps1','uninstall-release-tests.ps1','uninstall-file-recovery-tests.ps1')) {
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot ("tests\$name"))
        if($LASTEXITCODE -ne 0){throw "Test failed: $name"}
    }
    Write-Output 'release_preflight_passed=true'
} finally {$env:PATH=$oldPath}
