using System.IO;
using System.Text;
using System.Text.RegularExpressions;

namespace OakLauncher;

/// <summary>Append-only launch diagnostics under %LOCALAPPDATA%\Oak\launch.log.</summary>
public static class LaunchLog
{
    public static string Path => System.IO.Path.Combine(LauncherConfig.DataDir, "launch.log");

    // Absolute user paths, JWT-shaped blobs, and bearer headers must never hit disk.
    private static readonly Regex Sensitive = new(
        @"([A-Za-z]:\\Users\\[^\s""']+)|" +
        @"(Bearer\s+[A-Za-z0-9\-._~+/]+=*)|" +
        @"(eyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]+)|" +
        @"(localClientDll\s*=\s*[^\r\n]+)|" +
        @"(leaseToken|refreshToken|accessToken|bootstrapTicket)\s*[:=]\s*\S+",
        RegexOptions.IgnoreCase | RegexOptions.Compiled);

    public static void Write(string message)
    {
        try
        {
#if !DEBUG
            // Release: keep a short operational trail, not inject dumps or full paths.
            message = Scrub(message);
            if (message.Length > 400)
                message = message[..400] + "…";
#endif
            Directory.CreateDirectory(LauncherConfig.DataDir);
            File.AppendAllText(Path,
                $"[{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff}] {message}{Environment.NewLine}");
        }
        catch { /* never throw from logging */ }
    }

    public static void Write(Exception ex, string context)
    {
        var sb = new StringBuilder()
            .Append(context).Append(": ").Append(ex.GetType().Name)
            .Append(" — ").Append(ex.Message);
        if (ex.InnerException != null)
            sb.Append(" | inner: ").Append(ex.InnerException.Message);
        Write(sb.ToString());
#if DEBUG
        Write(ex.ToString());
#endif
    }

    public static string Scrub(string message)
    {
        if (string.IsNullOrEmpty(message)) return message;
        return Sensitive.Replace(message, "[redacted]");
    }
}
