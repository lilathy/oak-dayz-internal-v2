using System.IO;
using System.Text;
using System.Text.Json;
using System.Text.Encodings.Web;

namespace OakLauncher;

/// <summary>
/// Short-lived, local-only handoff for the injected client bootstrap.
///
/// The launcher prefetches the product lease so the mapped client never opens
/// WinHttp inside DayZ (BattlEye terminates that). The loader contract is
/// unchanged: it still only receives the client DLL path.
/// </summary>
public sealed class ProtectionHandoff
{
    // Client's hand-rolled JSON parser does not decode \u002B — keep base64 as + / =
    private static readonly JsonWriterOptions LeaseWriterOptions = new()
    {
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    public string ApiUrl { get; set; } = "";
    public string ProductSlug { get; set; } = "";
    public string BootstrapTicket { get; set; } = "";
    public string HardwareId { get; set; } = "";
    public string ExpiresAt { get; set; } = "";
    public string ClientReleaseId { get; set; } = "";
    public string ClientNonce { get; set; } = "";
    /// <summary>1 when LeaseJson was written to the companion .oak-lease file.</summary>
    public int PrefetchedLease { get; set; }

    public static string PathForClient(string dllPath) => dllPath + ".oak-bootstrap";
    public static string LeasePathForClient(string dllPath) => dllPath + ".oak-lease";

    public static void Write(string dllPath, BootstrapHandoffDto bootstrap)
    {
        var handoff = new ProtectionHandoff
        {
            ApiUrl = LauncherConfig.Load().ApiUrl,
            ProductSlug = bootstrap.ProductSlug,
            BootstrapTicket = bootstrap.BootstrapTicket,
            HardwareId = OakLauncher.HardwareId.Current,
            ExpiresAt = bootstrap.ExpiresAt,
            ClientReleaseId = bootstrap.ClientReleaseId,
            ClientNonce = bootstrap.ClientNonce ?? "",
            PrefetchedLease = string.IsNullOrWhiteSpace(bootstrap.LeaseJson) ? 0 : 1,
        };
        var path = PathForClient(dllPath);
        var temp = path + ".tmp";
        File.WriteAllText(temp, JsonSerializer.Serialize(handoff, new JsonSerializerOptions
        {
            Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
        }));
        File.Move(temp, path, overwrite: true);
        TryHide(path);

        var leasePath = LeasePathForClient(dllPath);
        if (!string.IsNullOrWhiteSpace(bootstrap.LeaseJson))
        {
            var leaseTemp = leasePath + ".tmp";
            File.WriteAllText(leaseTemp, FlattenLeaseForClient(bootstrap.LeaseJson));
            File.Move(leaseTemp, leasePath, overwrite: true);
            TryHide(leasePath);
        }
        else
        {
            try { if (File.Exists(leasePath)) File.Delete(leasePath); } catch { /* ignore */ }
        }
    }

    /// <summary>
    /// Client VerifyRuntimePackage reads top-level sha256/payloadBase64/signature.
    /// The API nests those under runtimePackage — flatten before staging.
    /// </summary>
    public static string FlattenLeaseForClient(string leaseJson)
    {
        using var doc = JsonDocument.Parse(leaseJson);
        var root = doc.RootElement;
        if (!root.TryGetProperty("runtimePackage", out var pkg))
            return leaseJson;

        using var stream = new MemoryStream();
        using (var writer = new Utf8JsonWriter(stream, LeaseWriterOptions))
        {
            writer.WriteStartObject();
            if (root.TryGetProperty("leaseToken", out var lt))
                writer.WriteString("leaseToken", lt.GetString());
            if (root.TryGetProperty("renewChallenge", out var rc))
                writer.WriteString("renewChallenge", rc.GetString());
            if (pkg.TryGetProperty("sha256", out var sha))
                writer.WriteString("sha256", sha.GetString());
            if (pkg.TryGetProperty("signature", out var sig))
                writer.WriteString("signature", sig.GetString());
            if (pkg.TryGetProperty("signingKeyId", out var kid))
                writer.WriteString("signingKeyId", kid.GetString());
            if (pkg.TryGetProperty("publicKey", out var pem))
                writer.WriteString("publicKey", pem.GetString());
            if (pkg.TryGetProperty("payloadBase64", out var payload))
                writer.WriteString("payloadBase64", payload.GetString());
            writer.WriteEndObject();
        }
        return Encoding.UTF8.GetString(stream.ToArray());
    }

    public static void TryDelete(string dllPath)
    {
        try
        {
            var path = PathForClient(dllPath);
            if (File.Exists(path)) File.Delete(path);
        }
        catch { /* best effort cleanup */ }
        try
        {
            var lease = LeasePathForClient(dllPath);
            if (File.Exists(lease)) File.Delete(lease);
        }
        catch { /* best effort cleanup */ }
    }

    private static void TryHide(string path)
    {
        try
        {
            if (!File.Exists(path)) return;
            File.SetAttributes(path, FileAttributes.Hidden | FileAttributes.NotContentIndexed);
        }
        catch { /* best effort */ }
    }

    public static async Task DeleteAfterAsync(string dllPath, TimeSpan delay)
    {
        try { await Task.Delay(delay); }
        catch { return; }
        TryDelete(dllPath);
    }
}
