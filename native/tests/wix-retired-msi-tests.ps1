param([Parameter(Mandatory=$true)][string]$ToolchainBin,[Parameter(Mandatory=$true)][string]$WixExe,[Parameter(Mandatory=$true)][string]$WorkDir)
$ErrorActionPreference='Stop'
$work=[IO.Path]::GetFullPath($WorkDir)
if(Test-Path -LiteralPath $work){throw 'Test working directory already exists.'}
[void][IO.Directory]::CreateDirectory($work)
$compiler=Join-Path (Resolve-Path -LiteralPath $ToolchainBin).Path 'g++.exe'
$action=Join-Path $work 'action.dll';$msi=Join-Path $work 'fixture.msi';$root=Join-Path $work 'installed'
& $compiler -std=c++17 -O2 -static -shared (Join-Path $PSScriptRoot 'wix-retired-fixture.cpp') -lmsi -lbcrypt -lshlwapi -ladvapi32 -o $action
if($LASTEXITCODE -ne 0){throw 'Fixture action compilation failed.'}
& $WixExe build -arch x64 -d "ActionDll=$action" -d "FixtureDll=$env:WINDIR\System32\version.dll" -o $msi (Join-Path $PSScriptRoot 'wix-retired-fixture.wxs')
if($LASTEXITCODE -ne 0){throw 'Fixture MSI compilation failed.'}
$holderScript=Join-Path $work 'holder.ps1';$sentinel=Join-Path $work 'hold';$ready=Join-Path $work 'ready'
@'
Add-Type -TypeDefinition 'using System; using System.Runtime.InteropServices; public static class HoldDll { [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] public static extern IntPtr LoadLibraryEx(string p,IntPtr f,uint flags); [DllImport("kernel32.dll")] public static extern bool FreeLibrary(IntPtr h); }'
$h=[HoldDll]::LoadLibraryEx($env:LIUKAI_FIXTURE_DLL,[IntPtr]::Zero,1)
if($h -eq [IntPtr]::Zero){exit 1}
try { [IO.File]::WriteAllText($env:LIUKAI_FIXTURE_READY,'ready'); while(Test-Path -LiteralPath $env:LIUKAI_FIXTURE_HOLD){Start-Sleep -Milliseconds 200} }
finally { [void][HoldDll]::FreeLibrary($h) }
'@ | Set-Content -LiteralPath $holderScript -Encoding UTF8
function Invoke-Msi([string[]]$arguments,[string]$log) {
    $p=Start-Process -FilePath "$env:WINDIR\System32\msiexec.exe" -ArgumentList ($arguments+@('/qn','/norestart','/l*v',('"'+$log+'"'))) -Wait -PassThru -WindowStyle Hidden
    return $p.ExitCode
}
$holder=$null;$installed=$false
try {
    $code=Invoke-Msi @('/i',('"'+$msi+'"'),('INSTALLDIR="'+$root+'"')) (Join-Path $work 'install.log')
    if($code -ne 0){throw "Fixture installation failed: $code"};$installed=$true
    [IO.File]::WriteAllText($sentinel,'hold')
    $env:LIUKAI_FIXTURE_DLL=Join-Path $root 'fixture.dll';$env:LIUKAI_FIXTURE_HOLD=$sentinel;$env:LIUKAI_FIXTURE_READY=$ready
    $holder=Start-Process -FilePath "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+$holderScript+'"')) -PassThru -WindowStyle Hidden
    $deadline=[DateTime]::UtcNow.AddSeconds(15)
    while(-not (Test-Path -LiteralPath $ready) -and -not $holder.HasExited -and [DateTime]::UtcNow -lt $deadline){Start-Sleep -Milliseconds 100;$holder.Refresh()}
    if(-not (Test-Path -LiteralPath $ready)){throw 'Fixture holder failed to load DLL.'}
    $code=Invoke-Msi @('/x',('"'+$msi+'"'),('INSTALLDIR="'+$root+'"'),'FAIL_RETIRE=1') (Join-Path $work 'rollback.log')
    if($code -eq 0){throw 'Expected the injected MSI failure.'}
    $holder.Refresh();if($holder.HasExited){throw 'Holder was closed during the rollback test.'}
    if(-not (Test-Path -LiteralPath (Join-Path $root 'fixture.dll')) -or (Test-Path -LiteralPath (Join-Path $root 'retired.dll'))){throw 'MSI failed to restore loaded DLL.'}
    $log=Get-Content -LiteralPath (Join-Path $work 'rollback.log') -Raw
    if($log -notmatch 'CustomActionRollback\(Action=Undo' -or $log -notmatch 'CustomAction ForceFailure returned actual error code 1603'){throw 'Missing MSI action evidence.'}
    $code=Invoke-Msi @('/x',('"'+$msi+'"'),('INSTALLDIR="'+$root+'"'),'FAIL_COMMIT=1') (Join-Path $work 'commit-rollback.log')
    if($code -eq 0 -or -not (Test-Path -LiteralPath (Join-Path $root 'fixture.dll')) -or (Test-Path -LiteralPath (Join-Path $root 'retired.dll'))){throw 'Commit failure did not restore loaded DLL.'}
    $holder.Refresh();if($holder.HasExited){throw 'Holder was closed during the commit rollback test.'}
    $log=Get-Content -LiteralPath (Join-Path $work 'commit-rollback.log') -Raw
    if($log -notmatch 'CustomAction CommitRetire returned actual error code 1603' -or $log -notmatch 'CustomActionRollback\(Action=Undo'){throw 'Missing commit rollback evidence.'}
    $code=Invoke-Msi @('/x',('"'+$msi+'"'),('INSTALLDIR="'+$root+'"'),'FAIL_COMMIT=0') (Join-Path $work 'uninstall.log')
    if($code -ne 3010){throw "Fixture uninstall did not request reboot: $code"};$installed=$false
    $holder.Refresh();if($holder.HasExited){throw 'Holder was closed during successful uninstall.'}
    if((Test-Path -LiteralPath (Join-Path $root 'fixture.dll')) -or -not (Test-Path -LiteralPath (Join-Path $root 'retired.dll'))){throw 'Unexpected successful removal state.'}
    Write-Output 'MSI deferred and commit rollback, loaded DLL uninstall and reboot request passed.'
} finally {
    Remove-Item -LiteralPath $sentinel -ErrorAction SilentlyContinue
    if($holder){$holder.WaitForExit(10000)|Out-Null}
    if($installed){
        $cleanup=Invoke-Msi @('/x',('"'+$msi+'"'),('INSTALLDIR="'+$root+'"'),'SKIP_REBOOT_MODE=1','FAIL_COMMIT=0') (Join-Path $work 'cleanup.log')
        if($cleanup -in @(0,3010)){$installed=$false}else{Write-Warning "Fixture remains installed (exit $cleanup); inspect logs at $work"}
    }
    if(-not $installed){Remove-Item -LiteralPath (Join-Path $root 'retired.dll') -ErrorAction SilentlyContinue;Remove-Item -LiteralPath $root -ErrorAction SilentlyContinue}
    Remove-Item Env:LIUKAI_FIXTURE_DLL,Env:LIUKAI_FIXTURE_HOLD,Env:LIUKAI_FIXTURE_READY -ErrorAction SilentlyContinue
}
