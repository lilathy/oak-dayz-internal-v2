using System.Diagnostics;
using System.IO;
using System.Text.RegularExpressions;
using Microsoft.Win32;

namespace OakLauncher;

/// <summary>
/// Starts Steam (if needed), then launches the real DayZ client through
/// BattlEye (<c>DayZ_BE.exe</c>) — not DayZLauncher / the Steam library page.
/// DayZ Steam AppID: 221100. Target process: DayZ_x64.
/// </summary>
public static class GameLauncher
{
    public const int DayZAppId = 221100;
    public const string SteamProcess = "steam";
    public const string DayZProcess = Injector.GameProcess;
    public const string BattlEyeExe = "DayZ_BE.exe";
    public const string ClientExe = "DayZ_x64.exe";

    public static bool IsSteamRunning() =>
        Process.GetProcessesByName(SteamProcess).Length > 0;

    public static bool IsDayZRunning() => Injector.IsGameRunning();

    /// <summary>
    /// Ensures Steam is running, then starts DayZ via BattlEye. If DayZ_x64 is
    /// already up, this is a no-op success.
    /// </summary>
    public static async Task EnsureDayZRunningAsync(
        IProgress<string>? progress = null,
        CancellationToken ct = default)
    {
        if (IsDayZRunning())
        {
            progress?.Report("DayZ is already running.");
            return;
        }

        if (!IsSteamRunning())
        {
            progress?.Report("Starting Steam…");
            if (!TryStartSteam())
                throw new InvalidOperationException(
                    "Could not find Steam. Install Steam or start it manually, then try Launch again.");

            var steamReady = await WaitForProcessAsync(SteamProcess, TimeSpan.FromSeconds(45), progress, "Waiting for Steam…", ct);
            if (!steamReady)
                throw new InvalidOperationException("Steam did not start in time.");
            await Task.Delay(2500, ct);
        }

        progress?.Report("Starting DayZ (BattlEye)…");
        if (!TryStartDayZ(progress))
        {
            throw new InvalidOperationException(
                "Could not start DayZ_BE.exe. Open DayZ from Steam once to finish install, then try Launch again.");
        }

        var gameReady = await Injector.WaitForGameAsync(TimeSpan.FromMinutes(3), progress, ct);
        if (!gameReady)
        {
            throw new InvalidOperationException(
                "DayZ_x64 did not start in time. If BattlEye asked for elevation, accept it and try again.");
        }
    }

    public static bool TryStartSteam()
    {
        var steamExe = ResolveSteamExe();
        if (steamExe == null) return false;
        try
        {
            Process.Start(new ProcessStartInfo
            {
                FileName = steamExe,
                UseShellExecute = true,
            });
            return true;
        }
        catch
        {
            return false;
        }
    }

    /// <summary>
    /// Starts DayZ through BattlEye via <c>DayZ_BE.exe</c>. Prefer that over
    /// <c>steam://</c> — Steam often only opens DayZLauncher and never starts
    /// the client. Never uses <c>-noBattlEye</c> (that path is for offset dumps only).
    /// </summary>
    public static bool TryStartDayZ(IProgress<string>? progress = null)
    {
        // NVIDIA Freestyle/Overlay hooks crash DayZ at D3D create. Kill the hook
        // processes only — do not stop/restore NVIDIA services.
        KillNvidiaOverlayHooks(progress);

        var installDir = ResolveDayZInstallDir();
        if (installDir == null)
        {
            progress?.Report("DayZ install not found in Steam libraries.");
            return false;
        }

        var bePath = Path.Combine(installDir, BattlEyeExe);
        if (!File.Exists(bePath))
        {
            progress?.Report("DayZ_BE.exe missing.");
            return false;
        }

        progress?.Report($"Launching {BattlEyeExe} (BattlEye)…");
        if (StartProcess(bePath, workingDirectory: installDir))
            return true;

        // Last resort: Steam URL still goes through BattlEye (never -noBattlEye).
        if (IsSteamRunning())
        {
            progress?.Report($"DayZ_BE failed — trying Steam AppID {DayZAppId}…");
            try
            {
                Process.Start(new ProcessStartInfo
                {
                    FileName = $"steam://rungameid/{DayZAppId}",
                    UseShellExecute = true,
                });
                return true;
            }
            catch { /* fall through */ }
        }

        return false;
    }

    /// <summary>
    /// Opens the Bohemia <c>DayZLauncher.exe</c> UI only — does not start
    /// DayZ_x64 / DayZ_BE. Used when the user wants to pick mods/servers themselves
    /// after Oak has mapped the driver.
    /// </summary>
    public static bool TryStartDayZLauncher(IProgress<string>? progress = null)
    {
        if (Process.GetProcessesByName("DayZLauncher").Length > 0)
        {
            progress?.Report("DayZ Launcher is already open.");
            return true;
        }

        var installDir = ResolveDayZInstallDir();
        if (installDir == null)
        {
            progress?.Report("DayZ install not found in Steam libraries.");
            return false;
        }

        var launcherPath = Path.Combine(installDir, "DayZLauncher.exe");
        if (!File.Exists(launcherPath))
        {
            progress?.Report("DayZLauncher.exe missing.");
            return false;
        }

        progress?.Report("Opening DayZ Launcher…");
        return StartProcess(launcherPath, workingDirectory: installDir);
    }

    /// <summary>
    /// Ends NVIDIA Overlay / nvcontainer that inject Freestyle into DayZ's D3D
    /// device create. No service stop/start and no restore — Overlay respawns
    /// itself when the user opens the NVIDIA App.
    /// </summary>
    public static void KillNvidiaOverlayHooks(IProgress<string>? progress = null)
    {
        var killed = 0;
        foreach (var name in new[] { "NVIDIA Overlay", "nvcontainer" })
        {
            try
            {
                foreach (var p in Process.GetProcessesByName(name))
                {
                    try
                    {
                        p.Kill(entireProcessTree: true);
                        killed++;
                    }
                    catch { /* access / already exiting */ }
                    finally { p.Dispose(); }
                }
            }
            catch { /* ignore */ }
        }
        if (killed > 0)
            progress?.Report($"Cleared {killed} NVIDIA overlay hook process(es)…");
    }

    /// <summary>Local dev / LAN — BattlEye blocks usermode inject.</summary>
    public static bool TryStartDayZNoBattlEye(IProgress<string>? progress = null, string? connectHost = null, int connectPort = 0)
    {
        var installDir = ResolveDayZInstallDir();
        if (installDir == null)
        {
            progress?.Report("DayZ install not found in Steam libraries.");
            return false;
        }

        var clientPath = Path.Combine(installDir, ClientExe);
        if (!File.Exists(clientPath))
        {
            progress?.Report($"{ClientExe} not found.");
            return false;
        }

        var args = "-noBattlEye";
        if (!string.IsNullOrWhiteSpace(connectHost) && connectPort > 0)
            args += $" -connect={connectHost} -port={connectPort}";

        progress?.Report($"Launching {ClientExe} ({args})…");
        try
        {
            Process.Start(new ProcessStartInfo
            {
                FileName = clientPath,
                Arguments = args,
                WorkingDirectory = installDir,
                UseShellExecute = true,
            });
            return true;
        }
        catch
        {
            return false;
        }
    }

    public const int Cs2AppId = 730;
    public const string Cs2Process = "cs2";
    public const string Cs2ClientExe = "cs2.exe";

    public static bool IsCs2Running() => Injector.IsProcessRunning(Cs2Process);

    /// <summary>Starts CS2 via Steam AppID 730 (VAC path — same as a normal Steam launch).</summary>
    public static bool TryStartCs2(IProgress<string>? progress = null)
    {
        KillNvidiaOverlayHooks(progress);

        if (IsCs2Running())
        {
            progress?.Report("CS2 is already running.");
            return true;
        }

        if (!IsSteamRunning())
        {
            progress?.Report("Steam is not running.");
            return false;
        }

        progress?.Report($"Launching CS2 (Steam AppID {Cs2AppId})…");
        try
        {
            Process.Start(new ProcessStartInfo
            {
                FileName = $"steam://rungameid/{Cs2AppId}",
                UseShellExecute = true,
            });
            return true;
        }
        catch
        {
            return false;
        }
    }

    public static string? ResolveSteamExe()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam");
            var path = key?.GetValue("SteamExe") as string;
            if (!string.IsNullOrWhiteSpace(path) && File.Exists(path))
                return path;
            path = key?.GetValue("SteamPath") as string;
            if (!string.IsNullOrWhiteSpace(path))
            {
                var candidate = Path.Combine(path.Replace('/', '\\'), "steam.exe");
                if (File.Exists(candidate)) return candidate;
            }
        }
        catch { /* ignore registry issues */ }

        foreach (var candidate in new[]
                 {
                     Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), "Steam", "steam.exe"),
                     Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "Steam", "steam.exe"),
                 })
        {
            if (File.Exists(candidate)) return candidate;
        }

        return null;
    }

    public static string? ResolveSteamRoot()
    {
        var exe = ResolveSteamExe();
        if (exe == null) return null;
        return Path.GetDirectoryName(exe);
    }

    /// <summary>
    /// Finds <c>steamapps/common/DayZ</c> via Steam libraryfolders + appmanifest.
    /// </summary>
    public static string? ResolveDayZInstallDir()
    {
        foreach (var library in EnumerateSteamLibraryRoots())
        {
            var manifest = Path.Combine(library, "steamapps", $"appmanifest_{DayZAppId}.acf");
            if (!File.Exists(manifest)) continue;

            var installDirName = "DayZ";
            try
            {
                var text = File.ReadAllText(manifest);
                var m = Regex.Match(text, "\"installdir\"\\s+\"([^\"]+)\"");
                if (m.Success && !string.IsNullOrWhiteSpace(m.Groups[1].Value))
                    installDirName = m.Groups[1].Value;
            }
            catch { /* use default */ }

            var dir = Path.Combine(library, "steamapps", "common", installDirName);
            if (Directory.Exists(dir) &&
                (File.Exists(Path.Combine(dir, BattlEyeExe)) || File.Exists(Path.Combine(dir, ClientExe))))
                return dir;
        }

        // Common default if manifests are weird but the game is present.
        var steamRoot = ResolveSteamRoot();
        if (steamRoot != null)
        {
            var fallback = Path.Combine(steamRoot, "steamapps", "common", "DayZ");
            if (Directory.Exists(fallback)) return fallback;
        }

        return null;
    }

    private static IEnumerable<string> EnumerateSteamLibraryRoots()
    {
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var steamRoot = ResolveSteamRoot();
        if (steamRoot == null) yield break;

        var roots = new List<string>();
        void Add(string path)
        {
            path = path.Replace('/', '\\').Trim().TrimEnd('\\');
            if (path.Length == 0) return;
            if (seen.Add(path)) roots.Add(path);
        }

        Add(steamRoot);

        var vdf = Path.Combine(steamRoot, "steamapps", "libraryfolders.vdf");
        if (File.Exists(vdf))
        {
            try
            {
                foreach (Match m in Regex.Matches(File.ReadAllText(vdf), "\"path\"\\s+\"([^\"]+)\""))
                    Add(m.Groups[1].Value);
            }
            catch { /* ignore parse issues */ }
        }

        foreach (var r in roots) yield return r;
    }

    private static bool StartProcess(string fileName, string workingDirectory)
    {
        try
        {
            Process.Start(new ProcessStartInfo
            {
                FileName = fileName,
                WorkingDirectory = workingDirectory,
                UseShellExecute = true,
            });
            return true;
        }
        catch
        {
            return false;
        }
    }

    private static async Task<bool> WaitForProcessAsync(
        string processName,
        TimeSpan timeout,
        IProgress<string>? progress,
        string message,
        CancellationToken ct)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline)
        {
            ct.ThrowIfCancellationRequested();
            if (Process.GetProcessesByName(processName).Length > 0) return true;
            progress?.Report(message);
            await Task.Delay(1000, ct);
        }
        return false;
    }
}
