# Isolated operating-system probe: no registration, MSI or reboot queue changes.
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class LiuKaiLoadedDllRenameTest {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern IntPtr LoadLibraryEx(string path, IntPtr file, uint flags);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern bool MoveFileEx(string from, string to, uint flags);
    [DllImport("kernel32.dll")]
    public static extern bool FreeLibrary(IntPtr module);
}
'@
$directory = Join-Path ([IO.Path]::GetTempPath()) ('liukai-rename-test-' + [guid]::NewGuid().ToString('N'))
$original = Join-Path $directory 'original.dll'
$retired = Join-Path $directory 'retired.dll'
$module = [IntPtr]::Zero
try {
    [void](New-Item -ItemType Directory -Path $directory)
    Copy-Item -LiteralPath (Join-Path $env:WINDIR 'System32\version.dll') -Destination $original
    # DONT_RESOLVE_DLL_REFERENCES maps an image without invoking its initialization.
    $module = [LiuKaiLoadedDllRenameTest]::LoadLibraryEx($original, [IntPtr]::Zero, 1)
    if ($module -eq [IntPtr]::Zero) { throw "LoadLibraryEx failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())" }
    if (-not [LiuKaiLoadedDllRenameTest]::MoveFileEx($original, $retired, 0)) {
        throw "Rename of loaded image failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    }
    if ((Test-Path -LiteralPath $original) -or -not (Test-Path -LiteralPath $retired)) { throw 'Unexpected state after rename.' }
    if (-not [LiuKaiLoadedDllRenameTest]::MoveFileEx($retired, $original, 0)) {
        throw "Rollback rename failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    }
    if (-not (Test-Path -LiteralPath $original) -or (Test-Path -LiteralPath $retired)) { throw 'Unexpected state after rollback.' }
    Write-Output 'Loaded image rename and rollback passed.'
} finally {
    if ($module -ne [IntPtr]::Zero) { [void][LiuKaiLoadedDllRenameTest]::FreeLibrary($module) }
    Remove-Item -LiteralPath $original, $retired -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $directory -ErrorAction SilentlyContinue
}
