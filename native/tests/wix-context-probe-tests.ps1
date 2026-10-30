param([Parameter(Mandatory=$true)][string]$Msi)
$ErrorActionPreference='Stop'
$db=(New-Object -ComObject WindowsInstaller.Installer).OpenDatabase((Resolve-Path -LiteralPath $Msi).Path,0)
$v=$db.OpenView('SELECT * FROM `CustomAction`');$v.Execute();$seen=@{}
while($row=$v.Fetch()){$seen[$row.StringData(1)]=[pscustomobject]@{type=$row.IntegerData(2);source=$row.StringData(3);target=$row.StringData(4)}}
foreach($case in @(@('ProbeImmediate',1),@('ProbeUser',17409),@('ProbeSystem',3073))){$name=$case[0];if(-not $seen.ContainsKey($name) -or $seen[$name].type -ne $case[1] -or $seen[$name].source -cne 'ContextProbe' -or $seen[$name].target -cne 'ProbeContext'){throw "Incorrect MSI action: $name"}}
$v=$db.OpenView('SELECT `Condition` FROM `LaunchCondition`');$v.Execute();$blocked=$false;while($row=$v.Fetch()){if($row.StringData(1) -ceq '0'){$blocked=$true}};if(-not $blocked){throw 'Probe MSI is not blocked from installation.'}
$v=$db.OpenView('SELECT * FROM `InstallExecuteSequence`');$v.Execute();$seq=@{};$conditions=@{}
while($row=$v.Fetch()){$name=$row.StringData(1);$seq[$name]=$row.IntegerData(3);$conditions[$name]=$row.StringData(2)}
if(-not ($seq['InstallInitialize'] -lt $seq['ProbeImmediate'] -and $seq['ProbeImmediate'] -lt $seq['ProbeUser'] -and $seq['ProbeUser'] -lt $seq['ProbeSystem'] -and $seq['ProbeSystem'] -lt $seq['InstallFinalize'])){throw 'Incorrect probe action order.'}
foreach($name in @('ProbeImmediate','ProbeUser','ProbeSystem')){if($conditions[$name] -cne 'NOT Installed'){throw "Unexpected probe condition: $name"}}
'msi_context_action_tables_verified=3;sequence_verified=true;installation_blocked=true'
