using System.IO;

namespace OakLauncher;

/// Shared launcher product metadata. Native clients live under clients/<slug>.
public sealed record ProductProfile(
    string Slug,
    string DisplayName,
    string DllFileName,
    string StageDir,
    bool LaunchImplemented)
{
    public string StagedDll => Path.Combine(StageDir, DllFileName);
    public string InjectLog => Path.Combine(StageDir, "inject.log");
    public string AttachFlag => Path.Combine(StageDir, "dll_attach.flag");
    public string DevUnlock => Path.Combine(StageDir, "oak_dev_unlock");
    public string ProtectAuthFlag => Path.Combine(StageDir, "protect_auth.flag");

    /// <summary>Process name without .exe for Process.GetProcessesByName.</summary>
    public string ProcessName => Slug switch
    {
        "dayz" => "DayZ_x64",
        "cs2" => "cs2",
        _ => Slug,
    };

    public int SteamAppId => Slug switch
    {
        "dayz" => 221100,
        "cs2" => 730,
        _ => 0,
    };
}

public static class ProductRegistry
{
    public static readonly ProductProfile Dayz = new(
        Slug: "dayz",
        DisplayName: "DayZ",
        DllFileName: "dayz_internal.dll",
        StageDir: @"C:\oak\dayz",
        LaunchImplemented: true);

    public static readonly ProductProfile Rust = new(
        Slug: "rust",
        DisplayName: "Rust",
        DllFileName: "rust_internal.dll",
        StageDir: @"C:\oak\rust",
        LaunchImplemented: false);

    public static readonly ProductProfile Eft = new(
        Slug: "eft",
        DisplayName: "Escape from Tarkov",
        DllFileName: "eft_internal.dll",
        StageDir: @"C:\oak\eft",
        LaunchImplemented: false);

    public static readonly ProductProfile Cs2 = new(
        Slug: "cs2",
        DisplayName: "CS2",
        DllFileName: "cs2_internal.dll",
        StageDir: @"C:\oak\cs2",
        LaunchImplemented: true);

    public static IReadOnlyList<ProductProfile> All { get; } = new[] { Dayz, Rust, Eft, Cs2 };

    public static ProductProfile? TryGet(string? slug)
    {
        if (string.IsNullOrWhiteSpace(slug)) return null;
        foreach (var p in All)
            if (p.Slug.Equals(slug.Trim(), StringComparison.OrdinalIgnoreCase))
                return p;
        return null;
    }

    public static ProductProfile Get(string slug) =>
        TryGet(slug) ?? throw new ArgumentException("Unknown product slug: " + slug);
}
