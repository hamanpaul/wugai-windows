# Narrow registry leaf primitives. Callers must hold the product lifecycle locks,
# verify current-user/machine state, retain a durable backup, and pass a handle
# to the exact verified parent. No recursive deletion or parent removal.
function Assert-LegacyRegistryLeafName([string]$Leaf) {
    if([string]::IsNullOrEmpty($Leaf) -or $Leaf -match '[\\/]' -or $Leaf -in @('.','..')){throw 'Invalid registry leaf.'}
}
function Test-LegacyRegistryLeaf {
    param([Parameter(Mandatory=$true)]$Parent,[Parameter(Mandatory=$true)][string]$Leaf,
          [Parameter(Mandatory=$true)][string]$ValueName,[Parameter(Mandatory=$true)][string]$Kind,
          [Parameter(Mandatory=$true)]$Value)
    Assert-LegacyRegistryLeafName $Leaf
    $key=$Parent.OpenSubKey($Leaf,$false)
    if($null -eq $key){return 'absent'}
    try {
        $names=@($key.GetValueNames())
        if($key.SubKeyCount -ne 0 -or $names.Count -ne 1 -or $names[0] -cne $ValueName -or
           $key.GetValueKind($ValueName).ToString() -cne $Kind){return 'different'}
        $actual=$key.GetValue($ValueName,$null,[Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        if($Kind -ceq 'Binary') {
            if($actual -isnot [byte[]] -or $Value -isnot [byte[]]){return 'different'}
            if([Convert]::ToBase64String($actual) -cne [Convert]::ToBase64String($Value)){return 'different'}
        } elseif($Kind -ceq 'DWord') {
            if($actual -isnot [int] -or $actual -ne $Value){return 'different'}
        } else {return 'different'}
        return 'matches'
    } finally {$key.Dispose()}
}
function Remove-LegacyRegistryLeafIfUnchanged {
    param($Parent,[string]$Leaf,[string]$ValueName,[string]$Kind,$Value)
    if((Test-LegacyRegistryLeaf $Parent $Leaf $ValueName $Kind $Value) -cne 'matches'){throw 'Registry leaf changed before deletion.'}
    $Parent.DeleteSubKey($Leaf,$true)
    if((Test-LegacyRegistryLeaf $Parent $Leaf $ValueName $Kind $Value) -cne 'absent'){throw 'Registry leaf deletion not verified.'}
}
function Restore-LegacyRegistryLeafIfUnchanged {
    param($Parent,[string]$Leaf,[string]$ValueName,[string]$Kind,$Value)
    $state=Test-LegacyRegistryLeaf $Parent $Leaf $ValueName $Kind $Value
    if($state -ceq 'matches'){return}
    if($state -cne 'absent'){throw 'Registry leaf changed; refusing to overwrite.'}
    $key=$Parent.CreateSubKey($Leaf)
    if($null -eq $key){throw 'Cannot restore registry leaf.'}
    try {
        if($key.SubKeyCount -ne 0 -or @($key.GetValueNames()).Count -ne 0){throw 'Registry leaf appeared during restoration.'}
        $key.SetValue($ValueName,$Value,[Microsoft.Win32.RegistryValueKind]::$Kind)
        $key.Flush()
    } finally {$key.Dispose()}
    if((Test-LegacyRegistryLeaf $Parent $Leaf $ValueName $Kind $Value) -cne 'matches'){throw 'Registry leaf restoration not verified.'}
}
