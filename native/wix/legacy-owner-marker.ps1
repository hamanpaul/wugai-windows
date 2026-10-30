# Decode a complete owner-marker binary without consulting or changing state.
function Read-LegacyOwnerMarker([byte[]]$Bytes) {
    if($null -eq $Bytes){throw 'Marker is missing.'}
    $stream=New-Object IO.MemoryStream(,$Bytes)
    $reader=New-Object IO.BinaryReader($stream,[Text.Encoding]::Unicode)
    try {
        if($reader.ReadUInt32() -ne 1){throw 'Unsupported marker version.'}
        $n=$reader.ReadUInt32();if($n -gt 32760 -or $n*2 -gt ($stream.Length-$stream.Position)){throw 'Invalid owner length.'}
        $owner=[Text.Encoding]::Unicode.GetString($reader.ReadBytes([int]($n*2)))
        $n=$reader.ReadUInt32();if($n -gt 32760 -or $n*2 -gt ($stream.Length-$stream.Position)){throw 'Invalid icon length.'}
        $icon=[Text.Encoding]::Unicode.GetString($reader.ReadBytes([int]($n*2)))
        $enabled=$reader.ReadUInt32();if($enabled -gt 1 -or $stream.Position -ne $stream.Length){throw 'Invalid marker trailing data.'}
        return [pscustomobject]@{Owner=$owner;Icon=$icon;Enabled=$enabled}
    } finally {$reader.Dispose();$stream.Dispose()}
}
