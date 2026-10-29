param(
    [Parameter(Mandatory=$true)][string]$FixtureExe,
    [Parameter(Mandatory=$true)][string]$FixtureDll
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $repo 'native\maintenance.ps1')
$exe = (Resolve-Path -LiteralPath $FixtureExe).Path
$dll = (Resolve-Path -LiteralPath $FixtureDll).Path
$build = [IO.Path]::GetFullPath((Join-Path $repo 'build')).TrimEnd('\')
Assert-WuGai ($exe.StartsWith($build+'\',[StringComparison]::OrdinalIgnoreCase) -and
    $dll.StartsWith($build+'\',[StringComparison]::OrdinalIgnoreCase) -and
    [IO.Path]::GetFileName($exe) -ceq 'mapped-module-fixture.exe' -and
    [IO.Path]::GetFileName($dll) -ceq 'BoshiamyTsf.dll') 'Fixtures must be inside the build directory.'
Assert-WuGaiNoReparse $exe
Assert-WuGaiNoReparse $dll
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $repo 'native\uninstall-release.ps1'),[ref]$tokens,[ref]$errors)
Assert-WuGai ($errors.Count -eq 0) 'The uninstall script did not parse.'
foreach ($name in @('ConvertFrom-WuGaiModuleProcessList','Get-WuGaiModuleProcessCandidates',
                    'Get-WuGaiProcessModulePaths','Get-WuGaiMappedModuleUsers')) {
    $node=$ast.Find({param($item) $item -is [Management.Automation.Language.FunctionDefinitionAst] -and $item.Name -ceq $name},$false)
    Assert-WuGai ($null -ne $node) "Missing module-check helper: $name"
    . ([scriptblock]::Create($node.Extent.Text))
}
$run = Join-Path $build ('mapped-module-integration-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($run) | Out-Null
$stdout = Join-Path $run 'stdout.txt'; $stderr = Join-Path $run 'stderr.txt'
$readyFile = Join-Path $run 'ready.txt'
$process = $null
try {
    $process=Start-Process -FilePath $exe -ArgumentList @('"'+$dll+'"', '"'+$readyFile+'"') -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $deadline=[DateTime]::UtcNow.AddSeconds(12)
    do {
        if ($process.HasExited) { throw 'The isolated module fixture exited before inspection.' }
        [string]$ready=''
        if (Test-Path -LiteralPath $readyFile) {
            try { $ready=[IO.File]::ReadAllText($readyFile) } catch [IO.IOException] { $ready='' }
        }
        $isReady = [Regex]::IsMatch($ready,('ready pid='+$process.Id+'\b'))
        if (-not $isReady) { Start-Sleep -Milliseconds 100 }
    } while (-not $isReady -and [DateTime]::UtcNow -lt $deadline)
    Assert-WuGai $isReady 'The isolated module fixture did not become ready.'
    $users=@(Get-WuGaiMappedModuleUsers @($dll))
    Assert-WuGai (@($users | Where-Object { $_.Id -eq $process.Id -and $_.Inspection -ceq 'matched' }).Count -eq 1) 'The mapped DLL was not detected.'
    $other=Join-Path $run 'other\BoshiamyTsf.dll'
    $otherUsers=@(Get-WuGaiMappedModuleUsers @($other))
    Assert-WuGai (@($otherUsers | Where-Object { $_.Id -eq $process.Id }).Count -eq 0) 'A different DLL path was incorrectly reported as loaded.'
    Write-Output 'mapped module integration passed; no registration or installer was run'
} finally {
    # Only terminate the test process represented by this returned Process object.
    if ($null -ne $process) {
        try { if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit(5000) | Out-Null } }
        finally { $process.Dispose() }
    }
}
