using System;
using System.IO;
using System.Diagnostics;
using System.Reflection;
using System.Threading;
using System.Runtime.InteropServices;
using System.Text;

internal static class SetupLogTests {
    private static int passed;
    [DllImport("shell32.dll", SetLastError = true)]
    private static extern IntPtr CommandLineToArgvW([MarshalAs(UnmanagedType.LPWStr)] string commandLine, out int argc);
    [DllImport("kernel32.dll")]
    private static extern IntPtr LocalFree(IntPtr memory);

    private static void Require(bool value, string message) {
        if (!value) throw new Exception(message);
        ++passed;
    }

    private static string[] SplitArguments(string arguments) {
        int count;
        IntPtr vector = CommandLineToArgvW("fixture.exe " + arguments, out count);
        if (vector == IntPtr.Zero) throw new Exception("Could not parse fixture command line.");
        try {
            var result = new string[count - 1];
            for (int i = 1; i < count; ++i)
                result[i - 1] = Marshal.PtrToStringUni(Marshal.ReadIntPtr(vector, i * IntPtr.Size));
            return result;
        } finally { LocalFree(vector); }
    }

    private static void RequireRejected(Action action, string message) {
        bool rejected = false;
        try { action(); } catch (ArgumentException) { rejected = true; }
        Require(rejected, message);
    }

    private static int Main(string[] args) {
        try {
            if (args.Length == 4 && args[0] == "--diagnostics-integration-fixture") {
                using (var collector = File.OpenRead(args[1]))
                    Console.WriteLine(Setup.CollectDiagnostics(collector, args[2], args[3], null));
                return 0;
            }
            if (args.Length == 4 && args[0] == "--cleanup-worker-fixture") {
                Setup.ScheduleWorkerCleanup(Assembly.GetExecutingAssembly().Location, args[1], args[2], args[3]);
                return 0;
            }
            if (args.Length != 1) throw new ArgumentException("Expected an isolated fixture directory.");
            string root = Path.GetFullPath(args[0]);
            if (Directory.Exists(root)) throw new ArgumentException("Fixture directory already exists.");
            Directory.CreateDirectory(root);
            string log = Path.Combine(root, "setup.log");
            string manifest = Path.Combine(root, "release-manifest.json");
            File.WriteAllText(manifest, "{\"format\":\"liukai-release-v1\",\"version\":\"0.0.0-dev.log-fixture\"}", new UTF8Encoding(false));
            Setup.AppendLog(log, "started_utc=fixture");
            Setup.RecordRelease(log, root);
            Setup.RecordOutput(log, "{\"operation\":2147942413,\"diagnostic_trace\":{\"events\":[]}}\n", "fixture stderr\n", 1);
            string text = File.ReadAllText(log);
            Require(text.StartsWith("started_utc=fixture"), "Recording child output replaced the diagnostic header.");
            Require(text.Contains("release_version=0.0.0-dev.log-fixture"), "Release version was not recorded.");
            Require(text.Contains("release_manifest_sha256=" + Setup.Sha256(manifest)), "Release manifest identity was not recorded.");
            Require(text.Contains("{\"operation\":2147942413,\"diagnostic_trace\":{\"events\":[]}}\n"), "Child JSON output was lost.");
            Require(text.Contains("fixture stderr\n") && text.Contains("exit_code=1"), "Failure output or exit code was lost.");
            string earlyLog = Path.Combine(root, "early-failure.log");
            Setup.AppendLog(earlyLog, "started_utc=fixture");
            Setup.RecordFailure(earlyLog, "extract_payload", new InvalidDataException("fixture damaged ZIP"));
            string early = File.ReadAllText(earlyLog);
            Require(early.Contains("failed_stage=extract_payload") && early.Contains("System.IO.InvalidDataException: fixture damaged ZIP") &&
                early.Contains("exit_code=1") && early.Contains("finished_utc="), "Early failure evidence was incomplete.");
            string abc = Path.Combine(root, "abc");
            File.WriteAllText(abc, "abc", new UTF8Encoding(false));
            Require(Setup.Sha256(abc) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "File SHA-256 was incorrect.");
            Require(Setup.IsLogJob(Setup.NewLogJob()), "Generated child log job was invalid.");
            foreach (string invalid in new [] { "", "..\\existing.log", "C:\\existing.log", "20261026T000000Z-00000000000000000000000000000000.log", "20261026T000000Z-00000000000000000000000000000000\n" })
                Require(!Setup.IsLogJob(invalid), "Unsafe child log job was accepted.");

            const string installRoot = @"C:\Program Files\LiuKai";
            const string installedUninstaller = @"C:\Program Files\LiuKai.maintenance\Uninstall.exe";
            const string sid = "S-1-5-21-1-2-3-1001";
            var defaults = Setup.ResolveEntryOptions(@"C:\Downloads\Setup.exe", Setup.ParseOptions(new string[0]));
            Require(!defaults.Uninstall && defaults.InstallRoot == null, "External Setup no longer defaults to installation.");
            var removal = Setup.ResolveEntryOptions(installedUninstaller, Setup.ParseOptions(new string[0]));
            Require(removal.Uninstall && removal.InstallRoot == installRoot, "Double-clicking installed Uninstall.exe did not select its own root.");
            RequireRejected(delegate { Setup.ResolveEntryOptions(installedUninstaller, Setup.ParseOptions(new [] { "--install" })); }, "Installed Uninstall.exe accepted installation.");
            RequireRejected(delegate { Setup.ResolveEntryOptions(installedUninstaller, Setup.ParseOptions(new [] { "--uninstall", "--install-root", @"C:\Other" })); }, "Installed Uninstall.exe accepted a different removal root.");
            foreach (string[] invalid in new [] {
                new [] { "--unknown" }, new [] { "--uninstall", "--install" }, new [] { "--uninstall", "--uninstall" },
                new [] { "--silent", "--silent" }, new [] { "--install-root" }, new [] { "--expected-sid", "invalid" },
                new [] { "--log-job", "--silent" }, new [] { "--uninstall", "--install-root", "relative" },
                new [] { "--uninstall", "--install-root", @"C:\" }, new [] { "--install-root", installRoot }
            }) RequireRejected(delegate { Setup.ResolveEntryOptions(@"C:\Downloads\Setup.exe", Setup.ParseOptions(invalid)); }, "Unsafe or ambiguous setup arguments were accepted.");

            var explicitRemoval = Setup.ParseOptions(new [] { "--uninstall", "--silent", "--install-root", installRoot });
            string childJob = Setup.NewLogJob();
            var relayed = Setup.ParseOptions(SplitArguments(Setup.BuildSetupArguments(explicitRemoval, sid, childJob)));
            Require(relayed.Uninstall && relayed.Silent && relayed.InstallRoot == installRoot && relayed.ExpectedSid == sid &&
                relayed.LogJob == childJob, "Uninstall action or identity was lost across handoff/elevation.");
            string[] scriptArgs = SplitArguments(Setup.BuildScriptArguments(explicitRemoval, @"C:\Temp\uninstall-release.ps1", @"C:\Temp\bundle", sid, installedUninstaller));
            int rootAt = Array.IndexOf(scriptArgs, "-InstallRoot");
            Require(rootAt >= 0 && scriptArgs[rootAt + 1] == installRoot && Array.IndexOf(scriptArgs, "-SetupPath") < 0,
                "Uninstall script arguments were routed as installation.");
            string[] installArgs = SplitArguments(Setup.BuildScriptArguments(defaults, @"C:\Temp\install-release.ps1", @"C:\Temp\bundle", sid, @"C:\Downloads\Setup.exe"));
            int setupAt = Array.IndexOf(installArgs, "-SetupPath");
            Require(setupAt >= 0 && installArgs[setupAt + 1] == @"C:\Downloads\Setup.exe" && Array.IndexOf(installArgs, "-InstallRoot") < 0,
                "Installation did not identify its own Setup executable.");
            foreach (string argument in new [] { "", "contains spaces", "quote \" text", @"C:\trailing\", "嘸改" })
                Require(SplitArguments(Setup.QuoteArgument(argument))[0] == argument, "Process argument quoting did not round-trip.");
            Require(Setup.NeedsUninstallHandoff(installedUninstaller, removal), "Installed uninstaller must leave the removal tree.");
            Require(!Setup.NeedsUninstallHandoff(@"C:\Temp\Uninstall.exe", removal), "Temporary worker recursively requested handoff.");
            Require(!Setup.NeedsUninstallHandoff(@"C:\Program Files\Other.maintenance\Uninstall.exe", removal), "Handoff accepted another maintenance location.");
            string workerRoot = Path.Combine(root, "workers");
            Directory.CreateDirectory(workerRoot);
            string workerJob = Setup.NewLogJob();
            string worker = Setup.CopyUninstallerToTemp(abc, workerRoot, workerJob);
            Require(worker.StartsWith(workerRoot + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) &&
                Path.GetFileName(worker) == "Uninstall.exe" && Setup.Sha256(worker) == Setup.Sha256(abc), "Worker copy was outside its job or differed from its source.");
            bool collisionRejected = false;
            try { Setup.CopyUninstallerToTemp(abc, workerRoot, workerJob); } catch (IOException) { collisionRejected = true; }
            Require(collisionRejected, "Worker copy overwrote an existing job.");
            RequireRejected(delegate { Setup.CopyUninstallerToTemp(abc, workerRoot, "..\\elsewhere"); }, "Worker copy accepted path traversal.");
            var workerOptions = Setup.ParseOptions(SplitArguments(Setup.BuildSetupArguments(explicitRemoval, sid, workerJob, 42)));
            string workerExecutable = Path.Combine(Path.GetTempPath(), "LiuKai-Uninstall-" + workerJob, "Uninstall.exe");
            Setup.ResolveEntryOptions(workerExecutable, workerOptions);
            Require(workerOptions.LauncherPid == 42 && workerOptions.Uninstall && workerOptions.InstallRoot == installRoot,
                "Launcher identity or uninstall target was lost in worker handoff.");
            RequireRejected(delegate { Setup.ValidateLauncherWaitScope(@"C:\Other\Uninstall.exe", workerOptions, Path.GetTempPath()); }, "An arbitrary executable could wait for a process.");
            RequireRejected(delegate { Setup.ResolveEntryOptions(installedUninstaller, workerOptions); }, "The installed launcher accepted a worker-only wait argument.");
            Require(Setup.IsExpectedLauncherImage(installedUninstaller, installRoot), "Original launcher path was rejected.");
            Require(!Setup.IsExpectedLauncherImage(@"C:\Other.maintenance\Uninstall.exe", installRoot), "Another installation's launcher was accepted.");
            Require(!Setup.IsExpectedLauncherImage(@"C:\Program Files\LiuKai.maintenance\Other.exe", installRoot), "An unrelated maintenance executable was accepted.");
            foreach (string invalidPid in new [] { "0", "-1", "2147483648", "other" })
                RequireRejected(delegate { Setup.ParseOptions(new [] { "--uninstall", "--launcher-pid", invalidPid }); }, "An invalid launcher process identifier was accepted.");
            string payloadJob = Setup.NewLogJob();
            string payload = Path.Combine(root, "LiuKai-Setup-" + payloadJob);
            Directory.CreateDirectory(Path.Combine(payload, "nested"));
            File.WriteAllText(Path.Combine(payload, "nested", "payload.bin"), "fixture");
            RequireRejected(delegate { Setup.RemovePayloadDirectory(root, workerRoot, payloadJob); }, "Payload cleanup accepted an unrelated directory.");
            Setup.RemovePayloadDirectory(root, payload, payloadJob);
            Require(!Directory.Exists(payload) && File.Exists(abc), "Payload cleanup failed or removed an unrelated file.");
            var moduleStart = new ProcessStartInfo();
            moduleStart.UseShellExecute = false;
            moduleStart.EnvironmentVariables["PSModulePath"] = @"C:\fixture\modules";
            Setup.AddWindowsPowerShellModules(moduleStart);
            Require(moduleStart.EnvironmentVariables["PSModulePath"].Contains(@"WindowsPowerShell\v1.0\Modules") &&
                moduleStart.EnvironmentVariables["PSModulePath"].Contains(@"C:\fixture\modules"), "Windows PowerShell module path was not restored.");
            string collectorFixture = "param($OutputDirectory,$InstallerPath,$InstallRoot)\n" +
                "Add-Type -AssemblyName System.IO.Compression.FileSystem\n" +
                "$d=Join-Path $OutputDirectory 'data';[IO.Directory]::CreateDirectory($d)|Out-Null\n" +
                "[IO.File]::WriteAllText((Join-Path $d 'environment.json'),$InstallerPath)\n" +
                "[IO.File]::WriteAllText((Join-Path $d 'collection-issues.json'),$InstallRoot)\n" +
                "[IO.File]::WriteAllText((Join-Path $d 'diagnosis.json'),'{}')\n" +
                "[IO.Compression.ZipFile]::CreateFromDirectory($d,(Join-Path $OutputDirectory 'WuGai-diagnostics-fixture.zip'))\n";
            string diagnosticsRoot = Path.Combine(root, "diagnostics with spaces");
            string archive;
            using (var collector = new MemoryStream(Encoding.UTF8.GetBytes(collectorFixture)))
                archive = Setup.CollectDiagnostics(collector, diagnosticsRoot, abc, installRoot);
            Require(File.Exists(archive), "Diagnostics fixture did not produce an archive.");
            using (var zip = System.IO.Compression.ZipFile.OpenRead(archive)) {
                using (var reader = new StreamReader(zip.GetEntry("environment.json").Open()))
                    Require(reader.ReadToEnd() == abc, "Diagnostics installer path was not preserved.");
                using (var reader = new StreamReader(zip.GetEntry("collection-issues.json").Open()))
                    Require(reader.ReadToEnd() == installRoot, "Diagnostics custom install root was not preserved.");
            }
            Require(!File.Exists(Path.Combine(Path.GetDirectoryName(archive), "collector.ps1")), "Temporary collector was not removed.");
            bool collectorFailure = false;
            try { using (var collector = new MemoryStream(Encoding.UTF8.GetBytes("exit 7")))
                    Setup.CollectDiagnostics(collector, diagnosticsRoot, abc, null); }
            catch (IOException) { collectorFailure = true; }
            Require(collectorFailure, "Collector failure was not reported.");
            string cleanupJob = Setup.NewLogJob();
            string cleanupDir = Path.Combine(root, "LiuKai-Uninstall-" + cleanupJob);
            Directory.CreateDirectory(cleanupDir);
            string cleanupExe = Path.Combine(cleanupDir, "Uninstall.exe");
            File.Copy(Assembly.GetExecutingAssembly().Location, cleanupExe);
            string cleanupLog = Path.Combine(root, "worker-cleanup.log");
            File.WriteAllText(cleanupLog, "fixture\n");
            var start = new ProcessStartInfo(cleanupExe, "--cleanup-worker-fixture " + Setup.QuoteArgument(root) + " " +
                Setup.QuoteArgument(cleanupJob) + " " + Setup.QuoteArgument(cleanupLog));
            start.UseShellExecute = false;
            using (var child = Process.Start(start)) { child.WaitForExit(); Require(child.ExitCode == 0, "Worker cleanup fixture failed."); }
            for (int i = 0; i < 100 && (Directory.Exists(cleanupDir) || !File.ReadAllText(cleanupLog).Contains("worker_cleanup=complete")); ++i)
                Thread.Sleep(100);
            Require(!Directory.Exists(cleanupDir) && File.ReadAllText(cleanupLog).Contains("worker_cleanup=complete"), "Worker copy was not removed after exit.");
            Console.WriteLine("setup_log_tests_passed=" + passed);
            return 0;
        } catch (Exception error) {
            Console.Error.WriteLine(error);
            return 1;
        }
    }
}
