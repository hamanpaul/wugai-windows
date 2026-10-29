$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$installerPath = Join-Path $repo 'native\install-release.ps1'
$source = [IO.File]::ReadAllText($installerPath)
$tokens = $null
$parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile(
    $installerPath, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -ne 0) { throw 'install-release.ps1 did not parse cleanly.' }

# Isolate only the actual register-through-fresh-status decision block. The
# surrounding installer performs deployment, elevation, and registry access.
$tryStatements = @($ast.EndBlock.Statements | Where-Object {
    $_ -is [System.Management.Automation.Language.TryStatementAst]
})
if ($tryStatements.Count -ne 1) { throw 'Expected one top-level installer try block.' }
$bodyStatements = @($tryStatements[0].Body.Statements)
$registerAssignment = @($bodyStatements | Where-Object {
    $_ -is [System.Management.Automation.Language.AssignmentStatementAst] -and
    $_.Left -is [System.Management.Automation.Language.VariableExpressionAst] -and
    $_.Left.VariablePath.UserPath -ceq 'registrationText'
})
$freshStatusOutput = @($bodyStatements | Where-Object {
    if ($_ -isnot [System.Management.Automation.Language.PipelineAst] -or
        $_.PipelineElements.Count -ne 1 -or
        $_.PipelineElements[0] -isnot [System.Management.Automation.Language.CommandAst]) {
        return $false
    }
    $command = $_.PipelineElements[0]
    $command.GetCommandName() -ceq 'Write-Output' -and
        $command.CommandElements.Count -eq 2 -and
        $command.CommandElements[1] -is [System.Management.Automation.Language.VariableExpressionAst] -and
        $command.CommandElements[1].VariablePath.UserPath -ceq 'readbackText'
})
$completionOutput = @($bodyStatements | Where-Object {
    if ($_ -isnot [System.Management.Automation.Language.PipelineAst] -or
        $_.PipelineElements.Count -ne 1 -or
        $_.PipelineElements[0] -isnot [System.Management.Automation.Language.CommandAst]) { return $false }
    $command = $_.PipelineElements[0]
    $command.GetCommandName() -ceq 'Write-Output' -and $command.CommandElements.Count -eq 2 -and
        $command.CommandElements[1] -is [System.Management.Automation.Language.StringConstantExpressionAst] -and
        $command.CommandElements[1].Value.StartsWith('Installation complete.',[StringComparison]::Ordinal)
})
if ($registerAssignment.Count -ne 1 -or $freshStatusOutput.Count -ne 1 -or
    $completionOutput.Count -ne 1 -or
    $registerAssignment[0].Extent.StartOffset -ge $freshStatusOutput[0].Extent.EndOffset -or
    $freshStatusOutput[0].Extent.EndOffset -ge $completionOutput[0].Extent.EndOffset) {
    throw 'Could not locate the installer register-to-fresh-status block.'
}
$start = $registerAssignment[0].Extent.StartOffset
$length = $completionOutput[0].Extent.EndOffset - $start
$decision = [scriptblock]::Create($source.Substring($start, $length))

$script:Passed = 0
function Require([bool]$Value, [string]$Message) {
    if (-not $Value) { throw $Message }
    $script:Passed++
}

function Invoke-ReadbackCase(
    [string]$Name,
    [int]$RegisterExit,
    [object]$RegisterState,
    [int]$StatusExit,
    [object]$StatusState,
    [bool]$ExpectedSuccess,
    [string[]]$ExpectedCalls,
    [bool]$PublishFails = $false
) {
    $global:InstallReleaseFixtureCalls = [Collections.Generic.List[string]]::new()
    $global:InstallReleaseRegisterExit = $RegisterExit
    $global:InstallReleaseStatusExit = $StatusExit
    $global:InstallReleaseRegisterJson = $RegisterState | ConvertTo-Json -Compress -Depth 8
    $global:InstallReleaseStatusJson = $StatusState | ConvertTo-Json -Compress -Depth 8
    $global:InstallReleasePublishFails = $PublishFails

    # The production decision block invokes this fixture just as it invokes
    # LiuKaiRegister.exe. It returns structured JSON and supplies each process
    # exit code independently, while recording the command order.
    $tool = {
        param([string]$Operation, [string]$Path)
        $global:InstallReleaseFixtureCalls.Add($Operation)
        if ($Path -cne 'FIXTURE:\BoshiamyTsf.dll') { throw 'Unexpected DLL fixture path.' }
        if ($Operation -ceq '--register') {
            $global:LASTEXITCODE = $global:InstallReleaseRegisterExit
            return $global:InstallReleaseRegisterJson
        }
        if ($Operation -ceq '--status') {
            $global:LASTEXITCODE = $global:InstallReleaseStatusExit
            return $global:InstallReleaseStatusJson
        }
        throw "Unexpected registration operation: $Operation"
    }
    $dll = 'FIXTURE:\BoshiamyTsf.dll'
    $setup = 'FIXTURE:\Setup.exe'
    $root = 'FIXTURE:\LiuKai'
    $sid = 'S-1-5-21-1-2-3-1001'
    $packageManifest = [pscustomobject]@{version='fixture-version'}
    function Publish-WuGaiUninstaller {
        param([string]$SetupPath,[string]$InstallRoot,[string]$Version,[string]$ExpectedUserSid)
        $global:InstallReleaseFixtureCalls.Add('--publish-uninstaller')
        if ($SetupPath -cne 'FIXTURE:\Setup.exe' -or $InstallRoot -cne 'FIXTURE:\LiuKai' -or
            $Version -cne 'fixture-version' -or $ExpectedUserSid -cne 'S-1-5-21-1-2-3-1001') {
            throw 'Uninstaller publication identity mismatch.'
        }
        if ($global:InstallReleasePublishFails) { throw 'fixture publication failure' }
    }
    $accepted = $true
    try { $null = . $decision }
    catch { $accepted = $false }

    Require ($accepted -eq $ExpectedSuccess) "$Name acceptance mismatch"
    $actualCalls = @($global:InstallReleaseFixtureCalls.ToArray())
    Require (($actualCalls -join ',') -ceq ($ExpectedCalls -join ',')) "$Name command sequence mismatch"
}

$registerStale = [ordered]@{
    result = 0; registered = $false; immersive_supported = $false; input_mode_supported = $false
}
$freshGood = [ordered]@{
    registered = $true; immersive_supported = $true; input_mode_supported = $true; machine_com_registered = $true; machine_com = 0
}
Invoke-ReadbackCase 'stale register state, fresh status succeeds' 0 $registerStale 0 $freshGood $true @('--register','--status','--publish-uninstaller')
Invoke-ReadbackCase 'uninstaller publication fails after healthy registration' 0 $registerStale 0 $freshGood $false @('--register','--status','--publish-uninstaller') $true

$freshNotRegistered = [ordered]@{
    registered = $false; immersive_supported = $true; input_mode_supported = $true; machine_com_registered = $true; machine_com = 0
}
Invoke-ReadbackCase 'fresh status not registered' 0 $registerStale 0 $freshNotRegistered $false @('--register','--status')

$freshNotImmersive = [ordered]@{
    registered = $true; immersive_supported = $false; input_mode_supported = $true; machine_com_registered = $true; machine_com = 0
}
Invoke-ReadbackCase 'fresh status lacks immersive support' 0 $registerStale 0 $freshNotImmersive $false @('--register','--status')

Invoke-ReadbackCase 'fresh status command fails' 0 $registerStale 1 $freshGood $false @('--register','--status')

Invoke-ReadbackCase 'register command fails before status' 1 $registerStale 0 $freshGood $false @('--register')

$freshNotInputMode = [ordered]@{
    registered = $true; immersive_supported = $true; input_mode_supported = $false; machine_com_registered = $true; machine_com = 0
}
Invoke-ReadbackCase 'fresh status lacks input mode support' 0 $registerStale 0 $freshNotInputMode $false @('--register','--status')

$freshNoMachine = [ordered]@{
    registered = $true; immersive_supported = $true; input_mode_supported = $true;
    machine_com_registered = $false; machine_com = 1
}
Invoke-ReadbackCase 'fresh status has only current-user COM' 0 $registerStale 0 $freshNoMachine $false @('--register','--status')
$freshMissingMachine = [ordered]@{
    registered = $true; immersive_supported = $true; input_mode_supported = $true
}
Invoke-ReadbackCase 'legacy helper cannot prove machine COM' 0 $registerStale 0 $freshMissingMachine $false @('--register','--status')
$freshMachineConflict = [ordered]@{
    registered = $true; immersive_supported = $true; input_mode_supported = $true;
    machine_com_registered = $true; machine_com = 2147942413
}
Invoke-ReadbackCase 'machine HRESULT conflict cannot be hidden by boolean' 0 $registerStale 0 $freshMachineConflict $false @('--register','--status')

# Exercise the actual recovery loop without invoking deployment, UAC, or the
# registry. Recovery failure must prevent the subsequent deployment step.
$recoveryLoops=@($bodyStatements | Where-Object {
    $_ -is [Management.Automation.Language.ForEachStatementAst] -and
    $_.Variable.VariablePath.UserPath -ceq 'recoveryRoot'
})
if ($recoveryLoops.Count -ne 1 -or $recoveryLoops[0].Extent.StartOffset -ge $registerAssignment[0].Extent.StartOffset) {
    throw 'The interrupted-removal recovery loop is missing or runs too late.'
}
$recoveryBlock=[scriptblock]::Create($recoveryLoops[0].Extent.Text + "`nInvoke-FixtureDeployment")
$root='C:\Fixture\LiuKai';$devRoot='C:\Fixture\LiuKaiDev';$sid='S-1-5-21-1-2-3-1001';$package='C:\Fixture\Package'
$script:recoveryCalls=[Collections.Generic.List[string]]::new()
$script:recoveryMarkers=@{}
$script:recoveryFailure=$false
function Get-WuGaiRemovalReceiptPath([string]$InstallRoot) { return ($InstallRoot+'.receipt') }
function Test-Path([string]$LiteralPath) { return $script:recoveryMarkers.ContainsKey($LiteralPath) }
function Recover-WuGaiMaintenancePublication([string]$InstallRoot,[string]$ExpectedUserSid) {
    $script:recoveryCalls.Add('maintenance:'+ $InstallRoot)
}
function Recover-WuGaiInterruptedFileRemoval([string]$InstallRoot,[string]$ExpectedUserSid,[string]$Cli) {
    $script:recoveryCalls.Add('files:'+ $InstallRoot)
    if($script:recoveryFailure){throw 'Fixture recovery failure.'}
}
function Invoke-FixtureDeployment { $script:recoveryCalls.Add('deploy') }
. $recoveryBlock
Require (($script:recoveryCalls -join ',') -ceq 'deploy') 'Deployment without a receipt attempted recovery.'
$script:recoveryCalls.Clear();$script:recoveryMarkers[$root+'.receipt']=$true;$script:recoveryMarkers[$devRoot+'.receipt']=$true
. $recoveryBlock
Require (($script:recoveryCalls -join ',') -ceq ('maintenance:'+ $root+',files:'+ $root+',maintenance:'+ $devRoot+',files:'+ $devRoot+',deploy')) 'Both root receipts were not recovered before deployment.'
$script:recoveryCalls.Clear();$script:recoveryFailure=$true;$rejected=$false
try {. $recoveryBlock} catch {$rejected=$true}
Require ($rejected -and ($script:recoveryCalls -join ',') -ceq ('maintenance:'+ $root+',files:'+ $root)) 'A failed recovery did not stop deployment.'

Write-Output "install_release_readback_tests_passed=$script:Passed"
