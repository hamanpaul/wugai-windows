# Pure preconditions for the narrowly scoped legacy-profile recovery.
# This file never changes the registry or filesystem.
function Assert-LegacyProfileRecoveryPreconditions {
    param([Parameter(Mandatory=$true)]$State)
    if ($State.IsElevated -ne $true -or [string]::IsNullOrEmpty($State.UserSid) -or
        $State.UserSid -cne $State.ExpectedUserSid) { throw 'The elevated interactive user identity is not verified.' }
    if ($State.HistoryExit -ne 0 -or $State.HistoryInspection -ne 0 -or $State.LegacyHistoryInspection -ne 0) { throw 'The legacy machine transaction history is not verified.' }
    if ($State.PendingFileRemoval -ne $false -or $State.PendingMaintenancePublication -ne $false) { throw 'An interrupted legacy recovery receipt exists.' }
    if ($State.InstallRootExists -ne $false -or $State.MaintenanceRootExists -ne $false) { throw 'Legacy installation files are still present.' }
    if ($State.Status.kind -cne 'windows-installer-lifecycle' -or $State.Status.scope -cne 'user-and-machine-com-x64' -or
        $State.Status.operation -ne 0 -or $State.Status.cleanup -ne 0 -or $State.StatusExit -ne 1 -or $State.Status.readback -ne 2147943706 -or
        $State.Status.profile_registry_absence_result -ne 2147943706 -or
        $State.Status.profile_present_result -ne 0 -or $State.Status.profile_present -ne $false -or
        $State.Status.all_categories_result -ne 0 -or $State.Status.all_categories_count -ne 0) {
        throw 'The TSF and raw-profile observations do not match the verified orphan case.'
    }
    $resources=@($State.Status.resources)
    if ($resources.Count -ne 7) { throw 'Unexpected registration resource count.' }
    foreach ($name in @('com_x64','com_x86','keyboard_category','system_tray_category','ui_element_category','com-machine-x64')) {
        $entry=@($resources | Where-Object { $_.name -ceq $name })
        if ($entry.Count -ne 1 -or $entry[0].read_result -ne 0 -or $entry[0].present -ne $false -or
            $entry[0].lifecycle_owned -ne $false) { throw "Unexpected registration resource: $name" }
    }
    $profile=@($resources | Where-Object { $_.name -ceq 'profile' })
    if ($profile.Count -ne 1 -or $profile[0].read_result -ne 0 -or $profile[0].present -ne $false -or
        $profile[0].lifecycle_owned -ne $true) { throw 'The profile ownership marker is not verified.' }
    if ($State.ProfileSubkeys -ne 0 -or $State.ProfileValueCount -ne 1 -or
        $State.ProfileValueName -cne 'Enable' -or $State.ProfileValueKind -cne 'DWord' -or
        $State.ProfileEnable -notin @(0,1)) { throw 'The user profile leaf contains unexpected data.' }
    if ($State.MarkerSubkeys -ne 0 -or $State.MarkerValueCount -ne 1 -or
        $State.MarkerValueName -cne 'Record' -or $State.MarkerValueKind -cne 'Binary' -or
        $State.MarkerOwner -cne 'LiuKai-TSF-v1' -or $State.MarkerIcon -cne $State.ExpectedIcon -or
        $State.MarkerEnabled -notin @(0,1) -or $State.MarkerEncodingValid -ne $true) {
        throw 'The ownership marker is not the expected product record.'
    }
}

# Used only when restoring an interrupted, verified backup. Registry leaves may
# already be absent, so ownership of the snapshot and exact-value matching are
# independently verified by the receipt reader and conditional restore adapter.
function Assert-LegacyResumePreconditions {
    param([Parameter(Mandatory=$true)]$State)
    if($State.IsElevated -ne $true -or [string]::IsNullOrEmpty($State.UserSid) -or
       $State.UserSid -cne $State.ExpectedUserSid -or $State.HistoryExit -ne 0 -or
       $State.HistoryInspection -ne 0 -or $State.LegacyHistoryInspection -ne 0 -or
       $State.PendingFileRemoval -ne $false -or $State.PendingMaintenancePublication -ne $false -or
       $State.InstallRootExists -ne $false -or $State.MaintenanceRootExists -ne $false -or
       $State.MachineProfileExists -ne $false){throw 'Recovery context is not verified.'}
    $status=$State.Status
    if($status.kind -cne 'windows-installer-lifecycle' -or $status.scope -cne 'user-and-machine-com-x64' -or
       $status.operation -ne 0 -or $status.cleanup -ne 0 -or $status.profile_present_result -ne 0 -or
       $status.profile_present -ne $false -or $status.all_categories_result -ne 0 -or $status.all_categories_count -ne 0){throw 'Unexpected TSF recovery state.'}
    $normal=$State.StatusExit -eq 0 -and $status.readback -eq 0 -and $status.profile_registry_absence_result -eq 0
    $residual=$State.StatusExit -eq 1 -and $status.readback -eq 2147943706 -and $status.profile_registry_absence_result -eq 2147943706
    if(-not ($normal -or $residual)){throw 'The status report is incomplete or unexpected.'}
    $resources=@($status.resources);if($resources.Count -ne 7){throw 'Unexpected registration resource count.'}
    foreach($name in @('com_x64','com_x86','keyboard_category','system_tray_category','ui_element_category','com-machine-x64')) {
        $entry=@($resources | Where-Object {$_.name -ceq $name})
        if($entry.Count -ne 1 -or $entry[0].read_result -ne 0 -or $entry[0].present -ne $false -or
           $entry[0].lifecycle_owned -ne $false){throw "Unexpected registration resource: $name"}
    }
    $entry=@($resources | Where-Object {$_.name -ceq 'profile'})
    if($entry.Count -ne 1 -or $entry[0].read_result -ne 0 -or $entry[0].present -ne $false -or
       $entry[0].lifecycle_owned -isnot [bool]){throw 'Unexpected profile state.'}
}
