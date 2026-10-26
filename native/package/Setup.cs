using System;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Security.Principal;
using System.Windows.Forms;

internal static class Setup {
    [STAThread]
    private static int Main(string[] args) {
        bool silent = Array.IndexOf(args, "--silent") >= 0;
        string expectedSid = null;
        for (int i = 0; i + 1 < args.Length; ++i)
            if (args[i] == "--expected-sid") expectedSid = args[i + 1];
        try {
            string sid = WindowsIdentity.GetCurrent().User.Value;
            if (expectedSid != null && expectedSid != sid)
                throw new InvalidOperationException("請使用原本的 Windows 帳號授權安裝。");
            if (!Environment.Is64BitOperatingSystem) throw new InvalidOperationException("僅支援 x64 Windows。");
            bool admin = new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator);
            if (!admin) {
                var elevation = new ProcessStartInfo(Assembly.GetExecutingAssembly().Location,
                    "--expected-sid " + sid + (silent ? " --silent" : ""));
                elevation.Verb = "runas";
                elevation.UseShellExecute = true;
                elevation.WindowStyle = ProcessWindowStyle.Hidden;
                using (var child = Process.Start(elevation)) { child.WaitForExit(); return child.ExitCode; }
            }
            string job = DateTime.UtcNow.ToString("yyyyMMddTHHmmssZ") + "-" + Guid.NewGuid().ToString("N");
            string root = Path.Combine(Path.GetTempPath(), "LiuKai-Setup-" + job);
            Directory.CreateDirectory(root);
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
            string logRoot = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "LiuKai", "installer-logs");
            Directory.CreateDirectory(logRoot);
            string log = Path.Combine(logRoot, job + ".log");
            string script = Path.Combine(root, "install-release.ps1");
            var start = new ProcessStartInfo(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "WindowsPowerShell", "v1.0", "powershell.exe"));
            start.Arguments = "-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" + script +
                "\" -BundleRoot \"" + root + "\" -ExpectedUserSid " + sid + " -Execute";
            start.UseShellExecute = false;
            start.CreateNoWindow = true;
            start.RedirectStandardOutput = true;
            start.RedirectStandardError = true;
            int code;
            using (var child = Process.Start(start)) {
                var stdout = child.StandardOutput.ReadToEndAsync();
                var stderr = child.StandardError.ReadToEndAsync();
                child.WaitForExit();
                File.WriteAllText(log, stdout.Result + Environment.NewLine + stderr.Result);
                code = child.ExitCode;
            }
            if (!silent) MessageBox.Show(code == 0 ? "嘸．改 安裝完成。請重新開啟原本已執行的程式。" :
                "安裝未完成，請保留紀錄，勿連續重裝。\n" + log, "嘸．改", MessageBoxButtons.OK,
                code == 0 ? MessageBoxIcon.Information : MessageBoxIcon.Error);
            return code;
        } catch (Exception error) {
            if (!silent) MessageBox.Show(error.Message, "嘸．改 安裝未完成", MessageBoxButtons.OK, MessageBoxIcon.Error);
            return 1;
        }
    }
}
