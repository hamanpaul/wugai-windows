using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text;
using System.Text.RegularExpressions;
using System.Windows.Forms;

internal static class Setup {
    internal sealed class Options {
        internal bool Uninstall;
        internal bool ExplicitAction;
        internal bool Silent;
        internal string InstallRoot;
        internal string ExpectedSid;
        internal string LogJob;
        internal int? LauncherPid;
    }

    internal static string CanonicalInstallRoot(string path) {
        if (String.IsNullOrEmpty(path) || !Regex.IsMatch(path, @"\A(?:[A-Za-z]:\\|\\\\[^\\]+\\[^\\]+\\)"))
            throw new ArgumentException("Install root must be an absolute Windows path.");
        string full = Path.GetFullPath(path).TrimEnd('\\');
        if (full.Equals(Path.GetPathRoot(full).TrimEnd('\\'), StringComparison.OrdinalIgnoreCase))
            throw new ArgumentException("Install root cannot be a drive or share root.");
        return full;
    }

    internal static Options ParseOptions(string[] args) {
        var options = new Options();
        var seen = new HashSet<string>(StringComparer.Ordinal);
        for (int i = 0; i < args.Length; ++i) {
            string flag = args[i];
            if (!seen.Add(flag)) throw new ArgumentException("Repeated setup argument: " + flag);
            if (flag == "--install" || flag == "--uninstall") {
                if (options.ExplicitAction) throw new ArgumentException("Conflicting setup actions.");
                options.ExplicitAction = true;
                options.Uninstall = flag == "--uninstall";
            } else if (flag == "--silent") {
                options.Silent = true;
            } else if (flag == "--install-root" || flag == "--expected-sid" || flag == "--log-job" || flag == "--launcher-pid") {
                if (++i >= args.Length || String.IsNullOrEmpty(args[i]) || args[i].StartsWith("--", StringComparison.Ordinal))
                    throw new ArgumentException("Missing setup argument value: " + flag);
                if (flag == "--install-root") options.InstallRoot = CanonicalInstallRoot(args[i]);
                else if (flag == "--expected-sid") options.ExpectedSid = new SecurityIdentifier(args[i]).Value;
                else if (flag == "--launcher-pid") {
                    int pid;
                    if (!Regex.IsMatch(args[i], @"\A[1-9][0-9]{0,9}\z") || !Int32.TryParse(args[i], out pid))
                        throw new ArgumentException("Invalid uninstall launcher process.");
                    options.LauncherPid = pid;
                }
                else {
                    if (!IsLogJob(args[i])) throw new ArgumentException("Invalid setup log job identifier.");
                    options.LogJob = args[i];
                }
            } else throw new ArgumentException("Unknown setup argument: " + flag);
        }
        return options;
    }

    internal static Options ResolveEntryOptions(string executable, Options options) {
        string directory = Path.GetDirectoryName(Path.GetFullPath(executable));
        const string suffix = ".maintenance";
        bool installedUninstaller = Path.GetFileName(executable).Equals("Uninstall.exe", StringComparison.OrdinalIgnoreCase) &&
            directory.EndsWith(suffix, StringComparison.OrdinalIgnoreCase);
        if (installedUninstaller) {
            string root = CanonicalInstallRoot(directory.Substring(0, directory.Length - suffix.Length));
            if (options.ExplicitAction && !options.Uninstall)
                throw new ArgumentException("The installed uninstaller cannot install an application.");
            if (options.InstallRoot != null && !options.InstallRoot.Equals(root, StringComparison.OrdinalIgnoreCase))
                throw new ArgumentException("Uninstall root does not match the installed uninstaller location.");
            options.Uninstall = true;
            options.InstallRoot = root;
        }
        if (!options.Uninstall && options.InstallRoot != null)
            throw new ArgumentException("--install-root is supported only for uninstall.");
        if (options.LauncherPid.HasValue) ValidateLauncherWaitScope(executable, options, Path.GetTempPath());
        return options;
    }

    internal static string QuoteArgument(string value) {
        if (value == null || value.IndexOf('\0') >= 0) throw new ArgumentException("Invalid process argument.");
        var result = new StringBuilder("\"");
        int slashes = 0;
        foreach (char c in value) {
            if (c == '\\') { ++slashes; continue; }
            result.Append('\\', c == '"' ? slashes * 2 + 1 : slashes);
            result.Append(c);
            slashes = 0;
        }
        result.Append('\\', slashes * 2);
        return result.Append('"').ToString();
    }

    internal static string BuildSetupArguments(Options options, string sid, string job, int? launcherPid = null) {
        var args = new List<string>();
        args.Add(options.Uninstall ? "--uninstall" : "--install");
        if (options.InstallRoot != null) { args.Add("--install-root"); args.Add(options.InstallRoot); }
        args.Add("--expected-sid"); args.Add(sid);
        args.Add("--log-job"); args.Add(job);
        int? parent = launcherPid ?? options.LauncherPid;
        if (parent.HasValue) { args.Add("--launcher-pid"); args.Add(parent.Value.ToString()); }
        if (options.Silent) args.Add("--silent");
        return String.Join(" ", args.ConvertAll(QuoteArgument).ToArray());
    }

    internal static bool NeedsUninstallHandoff(string executable, Options options) {
        return options.Uninstall && options.InstallRoot != null &&
            Path.GetFileName(executable).Equals("Uninstall.exe", StringComparison.OrdinalIgnoreCase) &&
            Path.GetDirectoryName(Path.GetFullPath(executable)).Equals(options.InstallRoot + ".maintenance", StringComparison.OrdinalIgnoreCase);
    }

    internal static string CopyUninstallerToTemp(string executable, string tempRoot, string job) {
        if (!IsLogJob(job)) throw new ArgumentException("Invalid uninstall worker job.");
        string directory = Path.Combine(Path.GetFullPath(tempRoot), "LiuKai-Uninstall-" + job);
        if (Directory.Exists(directory) || File.Exists(directory)) throw new IOException("Uninstall worker directory already exists.");
        Directory.CreateDirectory(directory);
        string target = Path.Combine(directory, "Uninstall.exe");
        string expectedHash = Sha256(executable);
        File.Copy(executable, target, false);
        if (Sha256(target) != expectedHash) throw new InvalidDataException("Uninstall worker copy hash mismatch.");
        return target;
    }

    internal static void ValidateLauncherWaitScope(string executable, Options options, string tempRoot) {
        if (!options.LauncherPid.HasValue || options.LauncherPid.Value <= 0 || !options.Uninstall ||
            options.InstallRoot == null || !IsLogJob(options.LogJob))
            throw new ArgumentException("Launcher wait requires an identified temporary uninstall worker.");
        string worker = Path.Combine(Path.GetFullPath(tempRoot), "LiuKai-Uninstall-" + options.LogJob, "Uninstall.exe");
        if (!Path.GetFullPath(executable).Equals(worker, StringComparison.OrdinalIgnoreCase))
            throw new ArgumentException("Only the exact temporary uninstall worker may wait for its launcher.");
    }

    internal static bool IsExpectedLauncherImage(string image, string installRoot) {
        return Path.GetFullPath(image).Equals(Path.Combine(installRoot + ".maintenance", "Uninstall.exe"), StringComparison.OrdinalIgnoreCase);
    }

    internal static void WaitForUninstallLauncher(string executable, Options options) {
        ValidateLauncherWaitScope(executable, options, Path.GetTempPath());
        using (var self = Process.GetCurrentProcess()) {
            if (options.LauncherPid.Value == self.Id) throw new ArgumentException("Uninstall worker cannot wait for itself.");
            Process launcher;
            try { launcher = Process.GetProcessById(options.LauncherPid.Value); }
            catch (ArgumentException) { return; } // The launcher has already exited.
            using (launcher) {
                try {
                    if (launcher.HasExited) return;
                    string image = launcher.MainModule.FileName;
                    if (launcher.SessionId != self.SessionId || !IsExpectedLauncherImage(image, options.InstallRoot) || Sha256(image) != Sha256(executable))
                        throw new InvalidOperationException("Uninstall launcher identity does not match this worker.");
                } catch {
                    if (launcher.HasExited) return;
                    throw;
                }
                if (!launcher.WaitForExit(15000)) throw new IOException("The installed uninstaller is still running; no removal was started.");
            }
        }
    }

    internal static string BuildScriptArguments(Options options, string script, string bundle, string sid, string executable) {
        var args = new List<string> { "-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
            "-File", script, "-BundleRoot", bundle, "-ExpectedUserSid", sid, "-Execute" };
        if (options.Uninstall) {
            if (options.InstallRoot != null) { args.Add("-InstallRoot"); args.Add(options.InstallRoot); }
        } else { args.Add("-SetupPath"); args.Add(executable); }
        return String.Join(" ", args.ConvertAll(QuoteArgument).ToArray());
    }

    internal static string NewLogJob() {
        return DateTime.UtcNow.ToString("yyyyMMddTHHmmssZ") + "-" + Guid.NewGuid().ToString("N");
    }

    internal static bool IsLogJob(string job) {
        return job != null && Regex.IsMatch(job, @"\A[0-9]{8}T[0-9]{6}Z-[0-9a-f]{32}\z");
    }

    internal static string Sha256(string path) {
        using (var stream = File.OpenRead(path))
        using (var hash = SHA256.Create())
            return BitConverter.ToString(hash.ComputeHash(stream)).Replace("-", "").ToLowerInvariant();
    }

    internal static void AppendLog(string log, string text) {
        File.AppendAllText(log, text + Environment.NewLine, new UTF8Encoding(false));
    }

    internal static void RecordRelease(string log, string root) {
        string manifest = Path.Combine(root, "release-manifest.json");
        // This is diagnostic text only. install-release.ps1 still validates the
        // complete manifest and every payload hash before deployment.
        var version = Regex.Match(File.ReadAllText(manifest),
            "\"version\"\\s*:\\s*\"(?<version>[A-Za-z0-9][A-Za-z0-9._-]{0,63})\"");
        AppendLog(log, "release_version=" + (version.Success ? version.Groups["version"].Value : "unavailable"));
        AppendLog(log, "release_manifest_sha256=" + Sha256(manifest));
    }

    internal static void RecordOutput(string log, string stdout, string stderr, int code) {
        AppendLog(log, "--- stdout ---" + Environment.NewLine + stdout);
        AppendLog(log, "--- stderr ---" + Environment.NewLine + stderr);
        AppendLog(log, "exit_code=" + code + Environment.NewLine + "finished_utc=" + DateTime.UtcNow.ToString("o"));
    }

    internal static void RecordFailure(string log, string stage, Exception error) {
        AppendLog(log, "failed_stage=" + stage + Environment.NewLine + "exception=" + error.ToString());
        AppendLog(log, "exit_code=1" + Environment.NewLine + "finished_utc=" + DateTime.UtcNow.ToString("o"));
    }

    // Only remove the payload directory created for this invocation. Do not
    // follow junctions or symbolic links, even if extraction failed halfway.
    internal static void RemovePayloadDirectory(string tempRoot, string root, string job) {
        if (!IsLogJob(job)) throw new ArgumentException("Invalid payload job.");
        string expected = Path.Combine(Path.GetFullPath(tempRoot), "LiuKai-Setup-" + job);
        if (!Path.GetFullPath(root).Equals(expected, StringComparison.OrdinalIgnoreCase))
            throw new ArgumentException("Unexpected payload directory.");
        if (!Directory.Exists(expected)) return;
        var pending = new Stack<string>();
        pending.Push(expected);
        while (pending.Count != 0) {
            string directory = pending.Pop();
            if ((File.GetAttributes(directory) & FileAttributes.ReparsePoint) != 0)
                throw new IOException("A payload directory is a reparse point.");
            foreach (string entry in Directory.GetFileSystemEntries(directory)) {
                var attributes = File.GetAttributes(entry);
                if ((attributes & FileAttributes.ReparsePoint) != 0)
                    throw new IOException("A payload entry is a reparse point.");
                if ((attributes & FileAttributes.Directory) != 0) pending.Push(entry);
            }
        }
        Directory.Delete(expected, true);
    }

    internal static string PowerShellLiteral(string value) {
        return "'" + value.Replace("'", "''") + "'";
    }

    internal static void AddWindowsPowerShellModules(ProcessStartInfo start) {
        // A PowerShell 7 parent can supply a module path that omits the
        // Windows PowerShell 5 built-in modules, including Get-FileHash.
        string modules = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System),
            "WindowsPowerShell", "v1.0", "Modules");
        string current = start.EnvironmentVariables["PSModulePath"] ?? "";
        start.EnvironmentVariables["PSModulePath"] = modules + (current.Length == 0 ? "" : ";" + current);
    }

    // The collector is a separate resource so it remains available if payload
    // extraction fails. It only reads evidence and writes a local archive.
    internal static string CollectDiagnostics(Stream collector, string outputRoot, string executable, string installRoot) {
        if (collector == null) throw new InvalidOperationException("Embedded diagnostics collector is unavailable.");
        string run = Path.Combine(Path.GetFullPath(outputRoot), "run-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(run);
        string script = Path.Combine(run, "collector.ps1");
        try {
            using (var destination = new FileStream(script, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                collector.CopyTo(destination);
            var args = new List<string> { "-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", script,
                "-OutputDirectory", run, "-InstallerPath", executable };
            if (!String.IsNullOrEmpty(installRoot)) { args.Add("-InstallRoot"); args.Add(installRoot); }
            var start = new ProcessStartInfo(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System),
                "WindowsPowerShell", "v1.0", "powershell.exe"), String.Join(" ", args.ConvertAll(QuoteArgument).ToArray()));
            start.UseShellExecute = false;
            start.CreateNoWindow = true;
            start.RedirectStandardOutput = true;
            start.RedirectStandardError = true;
            AddWindowsPowerShellModules(start);
            using (var child = Process.Start(start)) {
                var stdout = child.StandardOutput.ReadToEndAsync();
                var stderr = child.StandardError.ReadToEndAsync();
                if (!child.WaitForExit(60000)) {
                    try { child.Kill(); child.WaitForExit(); } catch { }
                    throw new TimeoutException("Diagnostics collection timed out.");
                }
                if (child.ExitCode != 0) throw new IOException("Diagnostics collector failed: " + stderr.Result + stdout.Result);
            }
            string[] archives = Directory.GetFiles(run, "WuGai-diagnostics-*.zip", SearchOption.TopDirectoryOnly);
            if (archives.Length != 1) throw new IOException("Diagnostics collector did not produce exactly one archive.");
            using (var zip = ZipFile.OpenRead(archives[0])) {
                if (zip.GetEntry("environment.json") == null || zip.GetEntry("collection-issues.json") == null || zip.GetEntry("diagnosis.json") == null)
                    throw new InvalidDataException("Diagnostics archive is incomplete.");
            }
            return archives[0];
        } finally {
            if (File.Exists(script)) File.Delete(script);
        }
    }

    internal static string TryCollectDiagnostics(string log, string executable, string installRoot) {
        try {
            string outputRoot = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "LiuKai", "diagnostics");
            using (Stream collector = Assembly.GetExecutingAssembly().GetManifestResourceStream("diagnostics.ps1")) {
                string archive = CollectDiagnostics(collector, outputRoot, executable, installRoot);
                if (log != null) try { AppendLog(log, "diagnostics_archive=" + archive); } catch { }
                return archive;
            }
        } catch (Exception error) {
            if (log != null) try { AppendLog(log, "diagnostics_collection_failed=" + error.ToString()); } catch { }
            return null;
        }
    }

    // A running EXE cannot remove itself. A short-lived system PowerShell waits
    // for this worker to exit, then removes only the verified worker copy.
    internal static void ScheduleWorkerCleanup(string executable, string tempRoot, string job, string log) {
        if (!IsLogJob(job)) throw new ArgumentException("Invalid worker cleanup job.");
        string directory = Path.Combine(Path.GetFullPath(tempRoot), "LiuKai-Uninstall-" + job);
        string worker = Path.Combine(directory, "Uninstall.exe");
        if (!Path.GetFullPath(executable).Equals(worker, StringComparison.OrdinalIgnoreCase))
            throw new ArgumentException("Unexpected uninstall worker path.");
        string hash = Sha256(worker);
        int pid;
        using (var self = Process.GetCurrentProcess()) pid = self.Id;
        string script = "$ErrorActionPreference='Stop';$d=" + PowerShellLiteral(directory) + ";$f=" + PowerShellLiteral(worker) +
            ";$log=" + PowerShellLiteral(log) + ";$expected=" + PowerShellLiteral(hash) + ";try {" +
            "$p=Get-Process -Id " + pid + " -ErrorAction SilentlyContinue;if($p){if(-not $p.WaitForExit(30000)){throw 'Worker did not exit'}};" +
            "for($i=0;$i -lt 40;$i++){try {" +
            "if(-not (Test-Path -LiteralPath $d)){break};" +
            "$di=Get-Item -LiteralPath $d -Force;if(($di.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0){throw 'Worker directory is a reparse point'};" +
            "$items=@(Get-ChildItem -LiteralPath $d -Force);if($items.Count -ne 1 -or $items[0].FullName -ine $f -or " +
            "($items[0].Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0){throw 'Unexpected worker directory contents'};" +
            "if((Get-FileHash -LiteralPath $f -Algorithm SHA256).Hash -ine $expected){throw 'Worker hash changed'};" +
            "Remove-Item -LiteralPath $f -Force;Remove-Item -LiteralPath $d -Force;break" +
            "}catch {if($i -eq 39){throw};Start-Sleep -Milliseconds 250}};" +
            "Add-Content -LiteralPath $log -Value 'worker_cleanup=complete' -Encoding UTF8" +
            "}catch {try {Add-Content -LiteralPath $log -Value ('worker_cleanup_failed='+$_.Exception.Message) -Encoding UTF8}catch {}}";
        string encoded = Convert.ToBase64String(Encoding.Unicode.GetBytes(script));
        var start = new ProcessStartInfo(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "WindowsPowerShell", "v1.0", "powershell.exe"),
            "-NoLogo -NoProfile -NonInteractive -EncodedCommand " + encoded);
        start.UseShellExecute = false;
        start.CreateNoWindow = true;
        AddWindowsPowerShellModules(start);
        using (var child = Process.Start(start)) { }
    }

    [STAThread]
    private static int Main(string[] args) {
        bool silent = Array.IndexOf(args, "--silent") >= 0;
        bool uninstall = false;
        string log = null;
        bool logCreated = false;
        string stage = "initialize";
        string payloadRoot = null;
        string payloadJob = null;
        string workerCleanupPath = null;
        string workerCleanupJob = null;
        string executable = Assembly.GetExecutingAssembly().Location;
        string installRoot = null;
        try {
            Options options = ResolveEntryOptions(executable, ParseOptions(args));
            installRoot = options.InstallRoot;
            silent = options.Silent;
            uninstall = options.Uninstall;
            string job = options.LogJob ?? NewLogJob();
            string logRoot = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "LiuKai", "installer-logs");
            Directory.CreateDirectory(logRoot);
            log = Path.Combine(logRoot, job + ".log");
            using (new FileStream(log, FileMode.CreateNew, FileAccess.Write, FileShare.Read)) { }
            logCreated = true;
            AppendLog(log, "setup_log_format=liukai-setup-log-v2" + Environment.NewLine + "started_utc=" + DateTime.UtcNow.ToString("o"));
            AppendLog(log, "setup_path=" + executable + Environment.NewLine + "setup_sha256=" + Sha256(executable));
            AppendLog(log, "action=" + (options.Uninstall ? "uninstall" : "install"));
            AppendLog(log, "os_version_reported=" + Environment.OSVersion.Version + Environment.NewLine +
                "os_64bit=" + Environment.Is64BitOperatingSystem + Environment.NewLine + "process_64bit=" + Environment.Is64BitProcess);
            try {
                using (var os = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\Windows NT\CurrentVersion"))
                    if (os != null) AppendLog(log, "os_build=" + os.GetValue("CurrentBuildNumber") + "." + os.GetValue("UBR"));
            } catch (Exception osError) { AppendLog(log, "os_build_unavailable=" + osError.Message); }
            stage = "identity";
            AppendLog(log, "stage=" + stage);
            string sid = WindowsIdentity.GetCurrent().User.Value;
            if (options.ExpectedSid != null && options.ExpectedSid != sid)
                throw new InvalidOperationException("請使用原本的 Windows 帳號授權安裝。");
            if (!Environment.Is64BitOperatingSystem) throw new InvalidOperationException("僅支援 x64 Windows。");
            if (options.LauncherPid.HasValue) {
                stage = "wait_for_uninstall_launcher";
                AppendLog(log, "stage=" + stage);
                workerCleanupPath = executable;
                workerCleanupJob = options.LogJob;
                WaitForUninstallLauncher(executable, options);
                options.LauncherPid = null;
            }
            // The installed copy must exit before the worker removes its
            // maintenance directory. The temporary copy owns the result UI/log.
            if (NeedsUninstallHandoff(executable, options)) {
                stage = "uninstall_worker_handoff";
                AppendLog(log, "stage=" + stage);
                string workerJob = NewLogJob();
                string worker = CopyUninstallerToTemp(executable, Path.GetTempPath(), workerJob);
                AppendLog(log, "log_role=uninstall_launcher" + Environment.NewLine +
                    "worker_log=" + Path.Combine(logRoot, workerJob + ".log"));
                int launcherPid;
                using (var self = Process.GetCurrentProcess()) { launcherPid = self.Id; }
                var handoff = new ProcessStartInfo(worker, BuildSetupArguments(options, sid, workerJob, launcherPid));
                handoff.UseShellExecute = false;
                handoff.WorkingDirectory = Environment.GetFolderPath(Environment.SpecialFolder.System);
                handoff.CreateNoWindow = true;
                handoff.WindowStyle = ProcessWindowStyle.Hidden;
                using (var child = Process.Start(handoff)) {
                    AppendLog(log, "worker_process_id=" + child.Id + Environment.NewLine + "handoff_started_utc=" + DateTime.UtcNow.ToString("o"));
                }
                return 0;
            }
            bool admin = new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator);
            if (!admin) {
                stage = "elevation";
                AppendLog(log, "stage=" + stage);
                string elevatedJob = NewLogJob();
                AppendLog(log, "log_role=elevation_launcher" + Environment.NewLine +
                    "elevated_log=" + Path.Combine(logRoot, elevatedJob + ".log"));
                var elevation = new ProcessStartInfo(Assembly.GetExecutingAssembly().Location,
                    BuildSetupArguments(options, sid, elevatedJob));
                elevation.Verb = "runas";
                elevation.UseShellExecute = true;
                elevation.WorkingDirectory = Environment.GetFolderPath(Environment.SpecialFolder.System);
                elevation.WindowStyle = ProcessWindowStyle.Hidden;
                using (var child = Process.Start(elevation)) {
                    child.WaitForExit();
                    AppendLog(log, "elevated_exit_code=" + child.ExitCode + Environment.NewLine + "finished_utc=" + DateTime.UtcNow.ToString("o"));
                    return child.ExitCode;
                }
            }
            if (!options.Uninstall && !silent) {
                stage = "install_confirmation";
                AppendLog(log, "stage=" + stage);
                var choice = MessageBox.Show(
                    "即將安裝嘸．改輸入法。安裝程式會在此電腦安裝程式檔，並為目前的 Windows 帳戶註冊輸入法及必要的系統元件。\n\n" +
                    "安裝需要系統管理員權限。安裝後可在 Windows 的「已安裝的應用程式」中移除；移除時會保留使用者設定及診斷紀錄。\n\n" +
                    "安裝或移除失敗時會在本機收集診斷資料，不會自動上傳。\n\n要繼續安裝嗎？",
                    "嘸．改 安裝說明", MessageBoxButtons.OKCancel, MessageBoxIcon.Information);
                if (choice != DialogResult.OK) {
                    AppendLog(log, "installation_cancelled_by_user=true" + Environment.NewLine + "finished_utc=" + DateTime.UtcNow.ToString("o"));
                    return 1223;
                }
            }
            stage = "extract_payload";
            AppendLog(log, "stage=" + stage);
            string root = Path.Combine(Path.GetTempPath(), "LiuKai-Setup-" + job);
            if (Directory.Exists(root) || File.Exists(root)) throw new IOException("Payload directory already exists.");
            Directory.CreateDirectory(root);
            payloadRoot = root;
            payloadJob = job;
            using (Stream data = Assembly.GetExecutingAssembly().GetManifestResourceStream("payload.zip"))
            using (var zip = new ZipArchive(data, ZipArchiveMode.Read)) {
                foreach (var entry in zip.Entries) {
                    string file = Path.GetFullPath(Path.Combine(root, entry.FullName));
                    if (!file.StartsWith(root + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
                        throw new InvalidDataException("Invalid installer payload path.");
                    Directory.CreateDirectory(Path.GetDirectoryName(file));
                    entry.ExtractToFile(file);
                }
            }
            stage = "read_release_metadata";
            AppendLog(log, "stage=" + stage + Environment.NewLine + "payload_root=" + root);
            RecordRelease(log, root);
            string script = Path.Combine(root, options.Uninstall ? "uninstall-release.ps1" : "install-release.ps1");
            var start = new ProcessStartInfo(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "WindowsPowerShell", "v1.0", "powershell.exe"));
            start.Arguments = BuildScriptArguments(options, script, root, sid, executable);
            start.UseShellExecute = false;
            start.WorkingDirectory = Environment.GetFolderPath(Environment.SpecialFolder.System);
            start.CreateNoWindow = true;
            AddWindowsPowerShellModules(start);
            start.RedirectStandardOutput = true;
            start.RedirectStandardError = true;
            int code;
            stage = options.Uninstall ? "run_uninstall_release" : "run_install_release";
            AppendLog(log, "stage=" + stage);
            using (var child = Process.Start(start)) {
                var stdout = child.StandardOutput.ReadToEndAsync();
                var stderr = child.StandardError.ReadToEndAsync();
                child.WaitForExit();
                code = child.ExitCode;
                RecordOutput(log, stdout.Result, stderr.Result, code);
            }
            string diagnostics = code == 0 ? null : TryCollectDiagnostics(log, executable, installRoot);
            RemovePayloadDirectory(Path.GetTempPath(), payloadRoot, payloadJob);
            payloadRoot = null;
            AppendLog(log, "payload_cleanup=complete");
            if (!silent) MessageBox.Show(code == 0 ? (options.Uninstall ? "嘸．改 移除完成。使用者設定與診斷紀錄已保留。" : "嘸．改 安裝完成。請重新開啟原本已執行的程式。") :
                (options.Uninstall ? "移除未完成，請保留紀錄。\n" : "安裝未完成，請保留紀錄，勿連續重裝。\n") + log +
                (diagnostics != null ? "\n診斷資料已儲存在本機：\n" + diagnostics : "\n無法自動收集完整診斷資料，請保留上述紀錄。"), "嘸．改", MessageBoxButtons.OK,
                code == 0 ? MessageBoxIcon.Information : MessageBoxIcon.Error);
            return code;
        } catch (Exception error) {
            // Argument and initialization errors can occur before the normal
            // job log exists. Preserve the error before collecting evidence.
            if (!logCreated) {
                try {
                    string fallbackRoot = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "LiuKai", "installer-logs");
                    Directory.CreateDirectory(fallbackRoot);
                    log = Path.Combine(fallbackRoot, NewLogJob() + ".log");
                    using (new FileStream(log, FileMode.CreateNew, FileAccess.Write, FileShare.Read)) { }
                    logCreated = true;
                    AppendLog(log, "setup_log_format=liukai-setup-log-v2" + Environment.NewLine + "started_utc=" + DateTime.UtcNow.ToString("o") +
                        Environment.NewLine + "setup_path=" + executable);
                } catch { logCreated = false; }
            }
            if (logCreated) {
                try { RecordFailure(log, stage, error); } catch { /* Preserve the original failure if the log is unwritable. */ }
            }
            string diagnostics = TryCollectDiagnostics(logCreated ? log : null, executable, installRoot);
            if (!silent) MessageBox.Show(error.Message + (logCreated ? "\n" + log : "") +
                (diagnostics != null ? "\n診斷資料已儲存在本機：\n" + diagnostics : "\n無法自動收集完整診斷資料。"),
                uninstall ? "嘸．改 移除未完成" : "嘸．改 安裝未完成", MessageBoxButtons.OK, MessageBoxIcon.Error);
            return 1;
        } finally {
            if (payloadRoot != null) {
                try {
                    RemovePayloadDirectory(Path.GetTempPath(), payloadRoot, payloadJob);
                    if (logCreated) AppendLog(log, "payload_cleanup=complete");
                } catch (Exception cleanupError) {
                    if (logCreated) try { AppendLog(log, "payload_cleanup_failed=" + cleanupError.ToString()); } catch { }
                }
            }
            if (workerCleanupPath != null) {
                try { ScheduleWorkerCleanup(workerCleanupPath, Path.GetTempPath(), workerCleanupJob, log); }
                catch (Exception cleanupError) {
                    if (logCreated) try { AppendLog(log, "worker_cleanup_schedule_failed=" + cleanupError.ToString()); } catch { }
                }
            }
        }
    }
}
