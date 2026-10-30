$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$script = Join-Path $repo 'native\package-install-windows.ps1'
$root = Join-Path $repo ('build\package-install-wrapper-tests-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root -Force | Out-Null
$package = Join-Path $root 'package'
$install = Join-Path $root 'installed'
New-Item -ItemType Directory -Path $package | Out-Null
$utf8 = New-Object System.Text.UTF8Encoding($false)
$version = '0.0.0-dev.wrapper-fixture'
$names = @('BoshiamyTsf.dll','LiuKaiLookup.exe','LiuKaiSettings.exe',
    'dictionary.tsv','readings.tsv','recommended-codes.tsv','recommended-codes-source.json',
    'phonetic-candidates.tsv','phonetic-candidates-source.json','THIRD_PARTY_NOTICES.md')
$paths = @('LiuKaiInstaller.exe','shared/branding.dll')
foreach($name in $names){$paths += "versions/$version/x64/$name"}
$rows = @()
foreach($relative in $paths){
    $file = Join-Path $package ($relative.Replace('/','\'))
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $file) | Out-Null
    [IO.File]::WriteAllText($file, "fixture:$relative`n", $utf8)
    $rows += [ordered]@{path=$relative;sha256=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant();bytes=([IO.FileInfo]$file).Length}
}
$manifest = [ordered]@{format='liukai-windows-package-v1';version=$version;
    installer_cli='LiuKaiInstaller.exe';branding='shared/branding.dll';architectures=@('x64');
    files=$rows;manifest_sha256_scope='All staged files except manifest.json itself.'}
[IO.File]::WriteAllText((Join-Path $package 'manifest.json'),($manifest | ConvertTo-Json -Depth 6),$utf8)
$powershell = (Get-Command powershell.exe).Source
$passed = 0
function Run-Plan([string[]]$Extra) {
    $prior = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & $powershell -NoProfile -ExecutionPolicy Bypass -File $script -PackageRoot $package -InstallRoot $install @Extra 2>&1
        return [pscustomobject]@{exit=$LASTEXITCODE;text=($output | Out-String).Trim()}
    } finally { $ErrorActionPreference = $prior }
}
function Require([bool]$Value,[string]$Message) {if(-not $Value){throw $Message};$script:passed++}

$fresh = Run-Plan @()
Require ($fresh.exit -eq 0) 'fresh plan failed'
$freshJson = $fresh.text | ConvertFrom-Json
Require ($freshJson.mode -eq 'fresh' -and $freshJson.command_arguments[3] -eq '-' -and
    $freshJson.command_arguments[4] -eq '-' -and $freshJson.command_arguments[5] -eq '-') 'fresh old tokens not absent'
Require (-not (Test-Path -LiteralPath $install)) 'planning created install root'
$machineFresh = Run-Plan @('-MachineComX64')
Require ($machineFresh.exit -eq 0) 'machine policy fresh plan failed'
$machineJson = $machineFresh.text | ConvertFrom-Json
Require ($machineJson.machine_com_x64 -eq $true -and $machineJson.command_arguments.Count -eq 8 -and
    $machineJson.command_arguments[6] -ceq '--diagnostic-trace' -and
    $machineJson.command_arguments[7] -ceq '--machine-com-x64') 'machine policy and trace were not passed in CLI order'
Require ($freshJson.command_arguments.Count -eq 7 -and $freshJson.machine_com_x64 -eq $false -and
    $freshJson.command_arguments[6] -ceq '--diagnostic-trace') 'legacy plan must retain trace without opting into machine COM'

# Execute only the production report validator, never the deploying script.
$astTokens = $null; $astErrors = $null
$wrapperAst = [Management.Automation.Language.Parser]::ParseFile($script,[ref]$astTokens,[ref]$astErrors)
Require ($astErrors.Count -eq 0) 'wrapper source parse failed'
$validator = $wrapperAst.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq 'Assert-MachineComCliStatus'},$false)
Require ($null -ne $validator) 'machine CLI report validator missing'
. ([scriptblock]::Create($validator.Extent.Text))
$exitValidator = $wrapperAst.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq 'Assert-MachineComCliExit'},$false)
Require ($null -ne $exitValidator) 'machine CLI exit validator missing'
. ([scriptblock]::Create($exitValidator.Extent.Text))
Assert-MachineComCliExit 0 'status ok'
$failureMessage = ''
try { Assert-MachineComCliExit 1 'TSF managers unavailable' } catch { $failureMessage = $_.Exception.Message }
Require ($failureMessage -match 'exit 1' -and $failureMessage -match 'TSF managers unavailable' -and
    $failureMessage -match 'deployment has not started') 'failed status must preserve CLI exit and diagnostic output'
$goodResource = [pscustomobject]@{name='com-machine-x64';scope='machine';hive='HKLM';registry_view=64;read_result=0;present=$false}
Assert-MachineComCliStatus ([pscustomobject]@{resources=@($goodResource)})
$script:passed++
foreach ($badReport in @(
    [pscustomobject]@{resources=@()},
    [pscustomobject]@{resources=@($goodResource,$goodResource)},
    [pscustomobject]@{resources=@([pscustomobject]@{name='com-machine-x64';scope='current-user';hive='HKLM';registry_view=64;read_result=0})},
    [pscustomobject]@{resources=@([pscustomobject]@{name='com-machine-x64';scope='machine';hive='HKCU';registry_view=64;read_result=0})},
    [pscustomobject]@{resources=@([pscustomobject]@{name='com-machine-x64';scope='machine';hive='HKLM';registry_view=32;read_result=0})},
    [pscustomobject]@{resources=@([pscustomobject]@{name='com-machine-x64';scope='machine';hive='HKLM';registry_view=64;read_result=1})}
)) {
    $rejected = $false
    try { Assert-MachineComCliStatus $badReport } catch { $rejected = $true }
    Require $rejected 'unproven machine CLI capability accepted'
}

New-Item -ItemType Directory -Path (Join-Path $install 'shared') -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $install 'LiuKaiInstaller.exe'),'old-cli',$utf8)
[IO.File]::WriteAllText((Join-Path $install 'shared\branding.dll'),'old-brand',$utf8)
$oldCli = (Get-FileHash (Join-Path $install 'LiuKaiInstaller.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
$oldBrand = (Get-FileHash (Join-Path $install 'shared\branding.dll') -Algorithm SHA256).Hash.ToLowerInvariant()
$state = "liukai-install-state-v1`nowner=LiuKai-TSF-v1`ncurrent=old.version`ncli_sha256=$oldCli`nbranding_sha256=$oldBrand`n"
[IO.File]::WriteAllText((Join-Path $install '.liukai-install-state'),$state,$utf8)
$upgrade = Run-Plan @()
Require ($upgrade.exit -eq 0) 'upgrade plan failed'
$upgradeJson = $upgrade.text | ConvertFrom-Json
Require ($upgradeJson.mode -eq 'upgrade' -and $upgradeJson.command_arguments[3] -eq 'old.version' -and
    $upgradeJson.command_arguments[4] -eq $oldCli -and $upgradeJson.command_arguments[5] -eq $oldBrand) 'upgrade old tokens wrong'

[IO.File]::WriteAllText((Join-Path $install 'LiuKaiInstaller.exe'),'changed-cli',$utf8)
$tamperedOld = Run-Plan @()
Require ($tamperedOld.exit -ne 0) 'tampered installed CLI accepted'
[IO.File]::WriteAllText((Join-Path $install 'LiuKaiInstaller.exe'),'old-cli',$utf8)
[IO.File]::WriteAllText((Join-Path $install '.liukai-install-state'),($state -replace 'owner=LiuKai-TSF-v1','owner=foreign'),$utf8)
$foreign = Run-Plan @()
Require ($foreign.exit -ne 0) 'foreign owner accepted'
[IO.File]::WriteAllText((Join-Path $install '.liukai-install-state'),$state,$utf8)
[IO.File]::WriteAllText((Join-Path $package 'LiuKaiInstaller.exe'),'changed-package-cli',$utf8)
$tamperedPackage = Run-Plan @()
Require ($tamperedPackage.exit -ne 0) 'tampered package CLI accepted'
Require ((Get-Content -Raw (Join-Path $install '.liukai-install-state')) -eq $state) 'failed plan changed installed state'
Write-Output "package_install_wrapper_tests_passed=$passed"
