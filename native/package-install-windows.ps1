param(
    [Parameter(Mandatory=$true)][string]$PackageRoot,
    [string]$InstallRoot = (Join-Path $env:ProgramFiles 'LiuKai'),
    [switch]$Execute,
    [string]$ExpectedUserSid = '',
    [switch]$MachineComX64
)
$ErrorActionPreference = 'Stop'

function Assert-Absolute([string]$Path,[string]$Name) {
    if ($Path -notmatch '^(?:[A-Za-z]:\\|\\\\[^\\]+\\[^\\]+\\)') { throw "$Name 必須是絕對路徑。" }
    $full = [IO.Path]::GetFullPath($Path)
    if ($full.Length -le 3) { throw "$Name 不可為磁碟根目錄。" }
    return $full.TrimEnd('\')
}
function Hash([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "缺少檔案：$Path" }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Assert-MachineComCliStatus([object]$Status) {
    $machine = @($Status.resources | Where-Object { $_.name -ceq 'com-machine-x64' })
    if ($machine.Count -ne 1 -or $machine[0].scope -cne 'machine' -or
        $machine[0].hive -cne 'HKLM' -or $machine[0].registry_view -ne 64 -or
        $machine[0].read_result -ne 0) {
        throw 'Packaged lifecycle CLI cannot independently read the explicit HKLM64 COM resource.'
    }
}
function Read-ExactUtf8([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    $utf8 = New-Object System.Text.UTF8Encoding($false,$true)
    return $utf8.GetString($bytes)
}

try {
    $package = Assert-Absolute $PackageRoot '套件目錄'
    $target = Assert-Absolute $InstallRoot '安裝目錄'
    if (-not (Test-Path -LiteralPath $package -PathType Container)) { throw '套件目錄不存在。' }
    $manifestPath = Join-Path $package 'manifest.json'
    $manifest = (Read-ExactUtf8 $manifestPath) | ConvertFrom-Json
    if ($manifest.format -ne 'liukai-windows-package-v1' -or
        $manifest.installer_cli -ne 'LiuKaiInstaller.exe' -or
        $manifest.branding -ne 'shared/branding.dll' -or
        $manifest.architectures.Count -ne 1 -or $manifest.architectures[0] -ne 'x64' -or
        $manifest.files.Count -ne 12 -or
        $manifest.version -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$' -or
        $manifest.version.Contains('..')) { throw '套件 manifest 不是有效的 x64 發佈格式。' }
    $names = @('BoshiamyTsf.dll','LiuKaiLookup.exe','LiuKaiSettings.exe',
        'dictionary.tsv','readings.tsv','recommended-codes.tsv','recommended-codes-source.json',
        'phonetic-candidates.tsv','phonetic-candidates-source.json','THIRD_PARTY_NOTICES.md')
    $expected = @('LiuKaiInstaller.exe','shared/branding.dll')
    foreach ($name in $names) { $expected += "versions/$($manifest.version)/x64/$name" }
    $rows = @{}
    foreach ($row in $manifest.files) {
        $relative = [string]$row.path
        if ($relative -notin $expected -or $rows.ContainsKey($relative) -or
            $row.sha256 -cnotmatch '^[0-9a-f]{64}$' -or
            $row.bytes -lt 0) { throw '套件 manifest 有未知、重複或無效的檔案紀錄。' }
        $rows[$relative] = $row
        $path = Join-Path $package ($relative.Replace('/','\'))
        if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or
            (Hash $path) -cne $row.sha256 -or
            ([IO.FileInfo]$path).Length -ne [long]$row.bytes) {
            throw "套件檔案與 manifest 不符：$relative"
        }
    }
    if ($rows.Count -ne $expected.Count) { throw '套件缺少必要檔案。' }
    $cli = Join-Path $package 'LiuKaiInstaller.exe'
    $oldVersion = '-'; $oldCli = '-'; $oldBrand = '-'; $mode = 'fresh'
    if (Test-Path -LiteralPath $target) {
        if (-not (Test-Path -LiteralPath $target -PathType Container)) { throw '安裝目錄不是資料夾。' }
        $statePath = Join-Path $target '.liukai-install-state'
        if (-not (Test-Path -LiteralPath $statePath -PathType Leaf)) {
            throw '安裝目錄已存在但沒有可辨識的 LiuKai 安裝狀態；拒絕覆寫。'
        }
        $state = Read-ExactUtf8 $statePath
        $match = [regex]::Match($state,
            '\Aliukai-install-state-v1\nowner=LiuKai-TSF-v1\ncurrent=([A-Za-z0-9][A-Za-z0-9._-]{0,63})\ncli_sha256=([0-9a-f]{64})\nbranding_sha256=([0-9a-f]{64})\n\z')
        if (-not $match.Success -or $match.Groups[1].Value.Contains('..')) {
            throw '既有安裝狀態格式或擁有者不符。'
        }
        $oldVersion = $match.Groups[1].Value
        $oldCli = $match.Groups[2].Value
        $oldBrand = $match.Groups[3].Value
        if ((Hash (Join-Path $target 'LiuKaiInstaller.exe')) -cne $oldCli -or
            (Hash (Join-Path $target 'shared\branding.dll')) -cne $oldBrand) {
            throw '既有安裝檔案與其狀態雜湊不符；拒絕升級。'
        }
        $mode = 'upgrade'
    }
    $arguments = @('--deploy-package',$package,$target,$oldVersion,$oldCli,$oldBrand)
    if ($MachineComX64) { $arguments += '--machine-com-x64' }
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $sid = $identity.User.Value
    if ($ExpectedUserSid -and $sid -cne $ExpectedUserSid) {
        throw "目前執行帳號 SID 與預期不符：$sid"
    }
    $plan = [ordered]@{kind='liukai-package-install-plan';mode=$mode;
        package_root=$package;install_root=$target;package_version=$manifest.version;
        manifest_sha256=(Hash $manifestPath);package_cli_sha256=(Hash $cli);
        user_sid=$sid;machine_com_x64=[bool]$MachineComX64;command_arguments=$arguments;executed=$false}
    if (-not $Execute) {
        $plan | ConvertTo-Json -Depth 4
        exit 0
    }
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw '請以同一 Windows 帳號的系統管理員 PowerShell 執行；未開始安裝。'
    }
    if ($MachineComX64) {
        $scopeText = & $cli '--status' '--machine-com-x64' 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) { throw 'Packaged CLI lacks the requested machine COM lifecycle policy; deployment has not started.' }
        Assert-MachineComCliStatus ($scopeText | ConvertFrom-Json)
    }
    $accessText = & $cli '--check-deployment-access' $sid 2>&1 | Out-String
    $accessCode = $LASTEXITCODE
    if ($accessCode -ne 0) { throw "TSF 部署權限檢查未通過（exit $accessCode）：$accessText" }
    $access = $accessText | ConvertFrom-Json
    if ($access.access_ready -ne $true -or $access.expected_user_sid -and
        $access.expected_user_sid -cne $sid) { throw 'TSF 部署權限或帳號不符；未開始安裝。' }
    & $cli @arguments
    $code = $LASTEXITCODE
    if ($code -ne 0) {
        [Console]::Error.WriteLine("LiuKai 部署失敗（installer exit $code）；保留交易紀錄，請勿盲目重試。")
    }
    exit $code
} catch {
    [Console]::Error.WriteLine("LiuKai 安裝入口拒絕執行：$($_.Exception.Message)")
    exit 2
}
