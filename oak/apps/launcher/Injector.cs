using System.Diagnostics;
using System.IO;
using System.Security.Principal;

namespace OakLauncher;

public sealed record InjectResult(bool Success, string Message, string Log);

/// <summary>
/// Drives the existing usermode loader (OakImGuiOverlayLoader.exe). The loader
/// contract is unchanged: it is invoked as <c>loader.exe "&lt;dll path&gt;"</c>,
/// waits for DayZ_x64.exe, and reports status on stdout.
/// </summary>
public static class Injector
{
    public const string GameProcess = "DayZ_x64";

    /// <summary>The loader's own wait for the game is 120s plus a 8s D3D settle.</summary>
    private static readonly TimeSpan Timeout = TimeSpan.FromSeconds(180);

    public static bool IsElevated()
    {
        try
        {
            using var identity = WindowsIdentity.GetCurrent();
            return new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator);
        }
        catch
        {
            return false;
        }
    }

    public static bool IsGameRunning() => Process.GetProcessesByName(GameProcess).Length > 0;

    public static bool IsProcessRunning(string processName) =>
        !string.IsNullOrWhiteSpace(processName) &&
        Process.GetProcessesByName(processName).Length > 0;

    public static bool IsProductRunning(ProductProfile profile) =>
        IsProcessRunning(profile.ProcessName);

    public static async Task<bool> WaitForGameAsync(
        TimeSpan timeout,
        IProgress<string>? progress = null,
        CancellationToken ct = default)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline)
        {
            ct.ThrowIfCancellationRequested();
            if (IsGameRunning()) return true;
            progress?.Report("Waiting for DayZ to start…");
            await Task.Delay(1000, ct);
        }
        return false;
    }

    /// <summary>
    /// Resolves the loader. The API stores an absolute path from the build
    /// machine, which will not exist on a customer's PC, so a copy shipped
    /// alongside the launcher wins if the configured path is missing.
    /// </summary>
    public static string? ResolveLoader(string? configuredPath)
    {
        if (!string.IsNullOrWhiteSpace(configuredPath) && File.Exists(configuredPath))
            return configuredPath;

        var name = string.IsNullOrWhiteSpace(configuredPath)
            ? "OakImGuiOverlayLoader.exe"
            : Path.GetFileName(configuredPath);

        foreach (var dir in new[]
                 {
                     AppContext.BaseDirectory,
                     Path.Combine(AppContext.BaseDirectory, "bin"),
                     Path.Combine(AppContext.BaseDirectory, "..", "bin"),
                     Path.Combine(LauncherConfig.DataDir, "bin"),
                 })
        {
            var candidate = Path.GetFullPath(Path.Combine(dir, name));
            if (File.Exists(candidate)) return candidate;
        }
        return null;
    }

    public static async Task<InjectResult> RunAsync(
        string loaderPath,
        string dllPath,
        IProgress<string>? progress = null,
        CancellationToken ct = default)
    {
        if (!File.Exists(loaderPath))
            return new InjectResult(false, $"Loader not found: {loaderPath}", "");
        if (!File.Exists(dllPath))
            return new InjectResult(false, $"Client not found: {dllPath}", "");

        var psi = new ProcessStartInfo
        {
            FileName = loaderPath,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            WorkingDirectory = Path.GetDirectoryName(loaderPath) ?? AppContext.BaseDirectory,
        };
        psi.ArgumentList.Add(dllPath);

        using var proc = new Process { StartInfo = psi, EnableRaisingEvents = true };
        var log = new System.Text.StringBuilder();
        var lastLine = "";

        // Both streams are drained via events. Reading one to the end while the
        // other fills its pipe buffer is the classic way to deadlock here.
        proc.OutputDataReceived += (_, e) =>
        {
            if (e.Data == null) return;
            lock (log) log.AppendLine(e.Data);
            lastLine = e.Data;
            progress?.Report(e.Data);
        };
        proc.ErrorDataReceived += (_, e) =>
        {
            if (e.Data == null) return;
            lock (log) log.AppendLine(e.Data);
        };

        try
        {
            proc.Start();
        }
        catch (Exception ex)
        {
            return new InjectResult(false, "Could not start loader: " + ex.Message, "");
        }

        proc.BeginOutputReadLine();
        proc.BeginErrorReadLine();

        using var timeoutCts = CancellationTokenSource.CreateLinkedTokenSource(ct);
        timeoutCts.CancelAfter(Timeout);

        try
        {
            await proc.WaitForExitAsync(timeoutCts.Token);
        }
        catch (OperationCanceledException)
        {
            TryKill(proc);
            var cancelled = ct.IsCancellationRequested;
            return new InjectResult(
                false,
                cancelled ? "Injection cancelled." : "Loader timed out — is DayZ running?",
                log.ToString());
        }

        // Flush whatever the async readers still have buffered.
        proc.WaitForExit();

        var text = log.ToString();
        return proc.ExitCode switch
        {
            0 => new InjectResult(true,
                string.IsNullOrWhiteSpace(lastLine) ? "Injected." : "Injected — overlay loaded.", text),
            1 => new InjectResult(false, "Loader could not read the client file.", text),
            2 => new InjectResult(false, "DayZ never started — launch the game, then inject.", text),
            3 => new InjectResult(false,
                IsElevated()
                    ? "Injection was blocked by the game process."
                    : "Injection blocked — run the launcher as Administrator.", text),
            _ => new InjectResult(false, $"Loader failed (exit {proc.ExitCode}).", text),
        };
    }

    private static void TryKill(Process proc)
    {
        try
        {
            if (!proc.HasExited) proc.Kill(entireProcessTree: true);
        }
        catch { /* already gone */ }
    }
}
