using System.IO;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace OakLauncher;

/// <summary>
/// Runtime settings. Resolution order: OAK_API_URL environment variable,
/// then oak.launcher.json next to the executable, then the built-in default.
/// Shipping a JSON file beside the exe means the same build can point at
/// localhost or production without a rebuild.
/// </summary>
public sealed class LauncherConfig
{
    public string ApiUrl { get; set; } = "http://127.0.0.1:8787";

    /// <summary>Optional SHA-256 (hex) of oak_loader.exe — required when set.</summary>
    public string? ExpectedLoaderSha256 { get; set; }

    /// <summary>Optional SHA-256 (hex) of oak.sys — required when set.</summary>
    public string? ExpectedSysSha256 { get; set; }

    /// <summary>
    /// Dev override: absolute path to a plaintext dayz_internal.dll used instead
    /// of the encrypted API cache (for local rebuilds before republishing).
    /// </summary>
    public string? LocalClientDll { get; set; }

    public static string Version =>
        Assembly.GetExecutingAssembly().GetName().Version is { } v
            ? $"{v.Major}.{v.Minor}.{v.Build}"
            : "1.0.0";

    public static LauncherConfig Load()
    {
        var cfg = new LauncherConfig();

        var file = Path.Combine(AppContext.BaseDirectory, "oak.launcher.json");
        if (File.Exists(file))
        {
            try
            {
                var loaded = JsonSerializer.Deserialize<LauncherConfig>(
                    File.ReadAllText(file),
                    new JsonSerializerOptions { PropertyNameCaseInsensitive = true });
                if (!string.IsNullOrWhiteSpace(loaded?.ApiUrl))
                    cfg.ApiUrl = loaded.ApiUrl;
                if (!string.IsNullOrWhiteSpace(loaded?.ExpectedLoaderSha256))
                    cfg.ExpectedLoaderSha256 = loaded.ExpectedLoaderSha256.Trim();
                if (!string.IsNullOrWhiteSpace(loaded?.ExpectedSysSha256))
                    cfg.ExpectedSysSha256 = loaded.ExpectedSysSha256.Trim();
                if (!string.IsNullOrWhiteSpace(loaded?.LocalClientDll))
                    cfg.LocalClientDll = loaded.LocalClientDll.Trim();
            }
            catch { /* fall back to the default rather than refusing to start */ }
        }

        var env = Environment.GetEnvironmentVariable("OAK_API_URL");
        if (!string.IsNullOrWhiteSpace(env))
            cfg.ApiUrl = env;

        var localDll = Environment.GetEnvironmentVariable("OAK_LOCAL_CLIENT_DLL");
        if (!string.IsNullOrWhiteSpace(localDll))
            cfg.LocalClientDll = localDll.Trim();

        cfg.ApiUrl = cfg.ApiUrl.TrimEnd('/');
#if !DEBUG
        // Shipping builds never honor a local plaintext DLL override — except when
        // talking to loopback (local lab / oak.launcher.json). Production HTTPS
        // keeps LocalClientDll nulled.
        if (!IsLoopbackApiUrl(cfg.ApiUrl))
            cfg.LocalClientDll = null;
#endif
        EnforceApiUrlPolicy(cfg);
        return cfg;
    }

    public static bool IsLoopbackApiUrl(string apiUrl)
    {
        if (!Uri.TryCreate(apiUrl, UriKind.Absolute, out var uri))
            return false;
        return uri.Host is "127.0.0.1" or "localhost" or "::1" || uri.IsLoopback;
    }

    /// <summary>
    /// Release builds refuse non-HTTPS API hosts except loopback (local smoke).
    /// </summary>
    public static void EnforceApiUrlPolicy(LauncherConfig cfg)
    {
#if !DEBUG
        if (!Uri.TryCreate(cfg.ApiUrl, UriKind.Absolute, out var uri))
            throw new InvalidOperationException("Invalid OAK_API_URL / apiUrl.");
        var loopback =
            uri.Host is "127.0.0.1" or "localhost" or "::1" ||
            uri.IsLoopback;
        if (!loopback && !string.Equals(uri.Scheme, Uri.UriSchemeHttps, StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException(
                "Release builds require an https:// API URL (loopback http is allowed for local testing).");
#endif
    }

    public static string DataDir
    {
        get
        {
            var dir = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "Oak");
            Directory.CreateDirectory(dir);
            return dir;
        }
    }

    public static string ClientDir(string slug)
    {
        // Per-user, always writable — unlike the server-provided C:\oak path,
        // which needs elevation and is shared between accounts.
        var dir = Path.Combine(DataDir, "clients", slug);
        Directory.CreateDirectory(dir);
        return dir;
    }
}

/// <summary>
/// "Stay signed in" storage. Only the refresh token is kept, encrypted with
/// DPAPI so it is bound to this Windows user and useless if copied elsewhere.
/// </summary>
public sealed class SavedSession
{
    public string RefreshToken { get; set; } = "";
    public string Username { get; set; } = "";
    public string ApiUrl { get; set; } = "";

    private static string Path_ => Path.Combine(LauncherConfig.DataDir, "session.bin");

    public static void Save(SavedSession session)
    {
        try
        {
            var json = JsonSerializer.SerializeToUtf8Bytes(session);
            var blob = ProtectedData.Protect(json, null, DataProtectionScope.CurrentUser);
            File.WriteAllBytes(Path_, blob);
        }
        catch { /* a session we can't persist just means signing in again */ }
    }

    public static SavedSession? Load()
    {
        try
        {
            if (!File.Exists(Path_)) return null;
            var blob = File.ReadAllBytes(Path_);
            var json = ProtectedData.Unprotect(blob, null, DataProtectionScope.CurrentUser);
            return JsonSerializer.Deserialize<SavedSession>(Encoding.UTF8.GetString(json));
        }
        catch
        {
            Clear();
            return null;
        }
    }

    public static void Clear()
    {
        try
        {
            if (File.Exists(Path_)) File.Delete(Path_);
        }
        catch { /* ignore */ }
    }
}

/// <summary>
/// Per-user launcher UI prefs (LocalAppData). Not secrets — plain JSON is fine.
/// </summary>
public sealed class LauncherPrefs
{
    /// <summary>
    /// When true (default), Launch starts the game after mapping the driver.
    /// When false, Oak stages + maps and waits — you start DayZ/CS2 yourself.
    /// </summary>
    public bool AutoStartGame { get; set; } = true;

    private static string Path_ => Path.Combine(LauncherConfig.DataDir, "launcher_prefs.json");

    public static LauncherPrefs Load()
    {
        try
        {
            if (!File.Exists(Path_)) return new LauncherPrefs();
            var loaded = JsonSerializer.Deserialize<LauncherPrefs>(
                File.ReadAllText(Path_),
                new JsonSerializerOptions { PropertyNameCaseInsensitive = true });
            return loaded ?? new LauncherPrefs();
        }
        catch
        {
            return new LauncherPrefs();
        }
    }

    public void Save()
    {
        try
        {
            Directory.CreateDirectory(LauncherConfig.DataDir);
            File.WriteAllText(Path_, JsonSerializer.Serialize(this, new JsonSerializerOptions
            {
                WriteIndented = true,
            }));
        }
        catch { /* prefs are best-effort */ }
    }
}
