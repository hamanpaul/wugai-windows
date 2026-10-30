# Recovery transaction coordinator. The supplied store owns durable snapshots,
# conditional registry operations and locks. This coordinator must not be used
# with a store that cannot prove those properties.
function Invoke-LegacyRecoveryTransaction {
    param([Parameter(Mandatory=$true)]$Store)
    $before = & $Store.ReadState
    Assert-LegacyProfileRecoveryPreconditions $before
    $fingerprint = & $Store.Fingerprint $before
    if ($fingerprint -cnotmatch '^[0-9a-f]{64}$') { throw 'Invalid prestate fingerprint.' }
    $receipt = & $Store.SaveDurableBackup $before $fingerprint
    if (-not $receipt -or -not (& $Store.VerifyBackup $receipt $fingerprint)) { throw 'Recovery backup could not be verified; no changes made.' }
    $current = & $Store.ReadState
    Assert-LegacyProfileRecoveryPreconditions $current
    if ((& $Store.Fingerprint $current) -cne $fingerprint) { throw 'Recovery prestate changed after backup; no changes made.' }
    $started = $false
    try {
        $started = $true
        & $Store.RemoveProfileIfUnchanged $before
        & $Store.RemoveMarkerIfUnchanged $before
        if (-not (& $Store.VerifyAbsent $before)) { throw 'Recovery readback did not prove absence.' }
        & $Store.MarkComplete $receipt
        return $receipt
    } catch {
        $failure = $_.Exception.Message
        if ($started) {
            try {
                & $Store.RestoreIfUnchanged $receipt
                if (-not (& $Store.VerifyRestored $receipt)) { throw 'Restore readback failed.' }
                & $Store.MarkRestored $receipt
            } catch {
                throw "Recovery failed: $failure; restore could not be verified: $($_.Exception.Message). Preserve backup $receipt."
            }
        }
        throw "Recovery failed and the prior state was restored: $failure. Backup: $receipt"
    }
}

# Resume a previously durable receipt after an interrupted operation. A real
# adapter must take both lifecycle locks and validate the current user, product,
# paths, hashes and each live value before the conditional restore.
function Resume-LegacyRecoveryTransaction {
    param([Parameter(Mandatory=$true)]$Store,[Parameter(Mandatory=$true)]$Receipt)
    $document = & $Store.ReadReceipt $Receipt
    if (-not $document -or $document.State -cnotin @('prepared','complete','restored') -or
        $document.Fingerprint -cnotmatch '^[0-9a-f]{64}$' -or
        -not (& $Store.VerifyBackup $Receipt $document.Fingerprint) -or
        -not (& $Store.VerifyRecoveryScope $document)) { throw 'Recovery receipt or scope cannot be verified.' }
    if ($document.State -in @('complete','restored')) { return $document.State }
    & $Store.RestoreIfUnchanged $Receipt
    if (-not (& $Store.VerifyRestored $Receipt)) { throw "Interrupted recovery restore could not be verified; preserve backup $Receipt." }
    & $Store.MarkRestored $Receipt
    return 'restored'
}
