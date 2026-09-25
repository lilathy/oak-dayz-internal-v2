using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using Microsoft.Win32;

namespace OakLauncher;

/// <summary>
/// Machine fingerprint used for account binding.
/// </summary>
public static class HardwareId
{
    /// <summary>
    /// Identifier version. Bump when the recipe below changes so
    /// <see cref="Legacy"/> can migrate existing bindings instead of locking
    /// the user out.
    /// </summary>
    public const int Version = 2;

    private static string? _cached;
    private static string? _cachedLegacy;

    public static string Current => _cached ??= Compute();

    /// <summary>
    /// The v1 recipe. It mixed in <c>OSVersion.VersionString</c>, which changes
    /// with every Windows build update, so bindings silently broke after a
    /// system update. Kept only so the launcher can prove ownership of an old
    /// binding and migrate it.
    /// </summary>
    public static string Legacy => _cachedLegacy ??= Sha256Hex(string.Join("|",
        Environment.MachineName,
        Environment.UserName,
        Environment.ProcessorCount,
        Environment.OSVersion.VersionString));

    private static string Compute()
    {
        // Every part is stable across reboots, Windows updates and renames.
        var parts = new List<string>
        {
            $"v{Version}",
            MachineGuid() ?? "no-machine-guid",
            SystemVolumeSerial() ?? "no-volume-serial",
            Environment.GetEnvironmentVariable("PROCESSOR_IDENTIFIER") ?? "no-cpu",
            RuntimeInformation.ProcessArchitecture.ToString(),
        };

        // MachineGuid is the anchor. Without it the fingerprint is weak enough
        // that unrelated machines could collide, so fall back to the machine
        // name to keep bindings distinct.
        if (parts[1].StartsWith("no-", StringComparison.Ordinal))
            parts.Add(Environment.MachineName);

        return Sha256Hex(string.Join("|", parts));
    }

    /// <summary>Windows install identity — survives hardware and name changes.</summary>
    private static string? MachineGuid()
    {
        try
        {
            using var key = RegistryKey
                .OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64)
                .OpenSubKey(@"SOFTWARE\Microsoft\Cryptography");
            return key?.GetValue("MachineGuid") as string;
        }
        catch
        {
            return null;
        }
    }

    /// <summary>Serial of the system volume — survives OS updates, changes on reformat.</summary>
    private static string? SystemVolumeSerial()
    {
        try
        {
            var root = Path.GetPathRoot(Environment.GetFolderPath(Environment.SpecialFolder.Windows));
            if (string.IsNullOrEmpty(root)) return null;
            if (!GetVolumeInformation(root, null, 0, out var serial, out _, out _, null, 0))
                return null;
            return serial.ToString("X8");
        }
        catch
        {
            return null;
        }
    }

    private static string Sha256Hex(string input) =>
        Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(input)));

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetVolumeInformation(
        string rootPathName,
        StringBuilder? volumeNameBuffer,
        int volumeNameSize,
        out uint volumeSerialNumber,
        out uint maximumComponentLength,
        out uint fileSystemFlags,
        StringBuilder? fileSystemNameBuffer,
        int fileSystemNameSize);
}
