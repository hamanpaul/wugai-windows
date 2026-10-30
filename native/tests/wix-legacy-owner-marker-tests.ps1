$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\wix\legacy-owner-marker.ps1')
$raw=[Convert]::FromBase64String('AQAAAA0AAABMAGkAdQBLAGEAaQAtAFQAUwBGAC0AdgAxACsAAABDADoAXABQAHIAbwBnAHIAYQBtACAARgBpAGwAZQBzAFwATABpAHUASwBhAGkAXABzAGgAYQByAGUAZABcAGIAcgBhAG4AZABpAG4AZwAuAGQAbABsAAEAAAA=')
$m=Read-LegacyOwnerMarker $raw
if($m.Owner -cne 'LiuKai-TSF-v1' -or $m.Icon -cne 'C:\Program Files\LiuKai\shared\branding.dll' -or $m.Enabled -ne 1){throw 'Known marker was decoded incorrectly'}
foreach($bad in @([byte[]]@(),[byte[]](1,0,0,0,255,255,255,255),([byte[]]($raw+0)))) {$reject=$false;try{$null=Read-LegacyOwnerMarker $bad}catch{$reject=$true};if(-not $reject){throw 'Malformed marker accepted'}}
'legacy_owner_marker_tests_passed=4'
