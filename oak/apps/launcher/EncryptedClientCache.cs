using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace OakLauncher;

/// <summary>
/// Per-Windows-user encrypted storage for downloaded client artifacts.
///
/// The bulk payload uses AES-256-GCM. Its random key and all identifying
/// metadata are wrapped with CurrentUser DPAPI and additionally checked against
/// Oak's current HWID context. Only the short injection window materializes a
/// plaintext DLL.
/// </summary>
public static class EncryptedClientCache
{
    private static readonly byte[] Magic = "OAKCACHE"u8.ToArray();
    private const int FormatVersion = 1;
    private const int MaxClientBytes = 128 * 1024 * 1024;

    private sealed class CacheMetadata
    {
        public int Format { get; set; } = FormatVersion;
        public string ProductSlug { get; set; } = "";
        public string ReleaseId { get; set; } = "";
        public string PlaintextSha256 { get; set; } = "";
        public string HwidContextSha256 { get; set; } = "";
        public long PlaintextSize { get; set; }
        public DateTimeOffset CreatedAt { get; set; }
    }

    private sealed class ProtectedEnvelope
    {
        public CacheMetadata Metadata { get; set; } = new();
        public byte[] Key { get; set; } = [];
        public byte[] Nonce { get; set; } = [];
        public byte[] Tag { get; set; } = [];
        public string CiphertextSha256 { get; set; } = "";
    }

    public static string CachePath(string slug, string filename) =>
        Path.Combine(LauncherConfig.ClientDir(slug), Path.GetFileName(filename) + ".oakc");

    public static bool IsCurrent(string path, string slug, string releaseId, string expectedSha256)
    {
        try
        {
            var envelope = ReadEnvelope(path, slug);
            var m = envelope.Metadata;
            return m.Format == FormatVersion &&
                   m.ProductSlug.Equals(slug, StringComparison.Ordinal) &&
                   m.ReleaseId.Equals(releaseId, StringComparison.Ordinal) &&
                   m.PlaintextSha256.Equals(expectedSha256, StringComparison.OrdinalIgnoreCase) &&
                   m.HwidContextSha256.Equals(HwidContextHash(), StringComparison.Ordinal);
        }
        catch
        {
            TryDelete(path);
            return false;
        }
    }

    public static async Task StoreAsync(
        Stream plaintext,
        string path,
        string slug,
        string releaseId,
        string expectedSha256,
        CancellationToken ct)
    {
        if (!plaintext.CanSeek) throw new InvalidOperationException("Client staging stream must be seekable.");
        if (plaintext.Length <= 0 || plaintext.Length > MaxClientBytes)
            throw new InvalidDataException("Client artifact has an invalid size.");

        plaintext.Position = 0;
        var bytes = new byte[checked((int)plaintext.Length)];
        await plaintext.ReadExactlyAsync(bytes, ct);

        var actualSha = Convert.ToHexString(SHA256.HashData(bytes));
        if (!actualSha.Equals(expectedSha256, StringComparison.OrdinalIgnoreCase))
        {
            CryptographicOperations.ZeroMemory(bytes);
            throw new InvalidDataException("Client artifact checksum mismatch.");
        }

        var metadata = new CacheMetadata
        {
            ProductSlug = slug,
            ReleaseId = releaseId,
            PlaintextSha256 = actualSha,
            HwidContextSha256 = HwidContextHash(),
            PlaintextSize = bytes.LongLength,
            CreatedAt = DateTimeOffset.UtcNow,
        };
        var aad = JsonSerializer.SerializeToUtf8Bytes(metadata);
        var key = RandomNumberGenerator.GetBytes(32);
        var nonce = RandomNumberGenerator.GetBytes(12);
        var tag = new byte[16];
        var ciphertext = new byte[bytes.Length];
        try
        {
            using (var aes = new AesGcm(key, tag.Length))
                aes.Encrypt(nonce, bytes, ciphertext, tag, aad);

            var envelope = new ProtectedEnvelope
            {
                Metadata = metadata,
                Key = key,
                Nonce = nonce,
                Tag = tag,
                CiphertextSha256 = Convert.ToHexString(SHA256.HashData(ciphertext)),
            };
            var protectedEnvelope = ProtectedData.Protect(
                JsonSerializer.SerializeToUtf8Bytes(envelope),
                Entropy(slug),
                DataProtectionScope.CurrentUser);

            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            var temp = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
            try
            {
                await using (var output = new FileStream(
                                 temp, FileMode.CreateNew, FileAccess.Write, FileShare.None,
                                 1024 * 1024, FileOptions.Asynchronous | FileOptions.WriteThrough))
                {
                    await output.WriteAsync(Magic, ct);
                    await output.WriteAsync(BitConverter.GetBytes(FormatVersion), ct);
                    await output.WriteAsync(BitConverter.GetBytes(protectedEnvelope.Length), ct);
                    await output.WriteAsync(BitConverter.GetBytes(ciphertext.Length), ct);
                    await output.WriteAsync(protectedEnvelope, ct);
                    await output.WriteAsync(ciphertext, ct);
                    await output.FlushAsync(ct);
                    output.Flush(flushToDisk: true);
                }
                File.Move(temp, path, overwrite: true);
            }
            finally
            {
                TryDelete(temp);
            }
        }
        finally
        {
            CryptographicOperations.ZeroMemory(bytes);
            CryptographicOperations.ZeroMemory(key);
        }
    }

    public static async Task<MaterializedClient> MaterializeAsync(
        string cachePath,
        string slug,
        string expectedReleaseId,
        string expectedSha256,
        string filename,
        CancellationToken ct)
    {
        CleanupRuntimeDirectory();
        var (ciphertext, envelope) = Read(cachePath, slug);
        var m = envelope.Metadata;
        if (m.Format != FormatVersion ||
            !m.ProductSlug.Equals(slug, StringComparison.Ordinal) ||
            !m.ReleaseId.Equals(expectedReleaseId, StringComparison.Ordinal) ||
            !m.PlaintextSha256.Equals(expectedSha256, StringComparison.OrdinalIgnoreCase) ||
            !m.HwidContextSha256.Equals(HwidContextHash(), StringComparison.Ordinal) ||
            m.PlaintextSize != ciphertext.LongLength)
        {
            throw new InvalidDataException("Encrypted client cache does not match this launch.");
        }
        if (!Convert.ToHexString(SHA256.HashData(ciphertext))
                .Equals(envelope.CiphertextSha256, StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException("Encrypted client cache is corrupt.");
        }

        var plaintext = new byte[ciphertext.Length];
        var aad = JsonSerializer.SerializeToUtf8Bytes(m);
        try
        {
            using (var aes = new AesGcm(envelope.Key, envelope.Tag.Length))
                aes.Decrypt(envelope.Nonce, ciphertext, envelope.Tag, plaintext, aad);
            if (!Convert.ToHexString(SHA256.HashData(plaintext))
                    .Equals(expectedSha256, StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidDataException("Decrypted client checksum mismatch.");
            }

            var runtimeDir = RuntimeDirectory();
            var safeName = Path.GetFileNameWithoutExtension(filename);
            var runtimePath = Path.Combine(runtimeDir, $"{safeName}-{Guid.NewGuid():N}.dll");
            await File.WriteAllBytesAsync(runtimePath, plaintext, ct);
            // Hidden only — avoid Temporary, which File.Copy can propagate onto
            // C:\oak\dayz_internal.dll and make later launches fail with access denied.
            File.SetAttributes(runtimePath, FileAttributes.Hidden);
            return new MaterializedClient(runtimePath);
        }
        finally
        {
            CryptographicOperations.ZeroMemory(plaintext);
            CryptographicOperations.ZeroMemory(envelope.Key);
        }
    }

    public static void RemoveLegacyPlaintext(string clientDirectory)
    {
        if (!Directory.Exists(clientDirectory)) return;
        foreach (var pattern in new[] { "*.dll", "*.download" })
        foreach (var path in Directory.EnumerateFiles(clientDirectory, pattern))
            TryDelete(path);
    }

    private static ProtectedEnvelope ReadEnvelope(string path, string slug)
    {
        using var input = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        Span<byte> header = stackalloc byte[20];
        input.ReadExactly(header);
        if (!header[..8].SequenceEqual(Magic)) throw new InvalidDataException("Unknown client cache format.");
        var version = BitConverter.ToInt32(header[8..12]);
        var envelopeLength = BitConverter.ToInt32(header[12..16]);
        var ciphertextLength = BitConverter.ToInt32(header[16..20]);
        if (version != FormatVersion ||
            envelopeLength <= 0 || envelopeLength > 64 * 1024 ||
            ciphertextLength <= 0 || ciphertextLength > MaxClientBytes ||
            input.Length != header.Length + envelopeLength + (long)ciphertextLength)
        {
            throw new InvalidDataException("Invalid client cache header.");
        }

        var protectedEnvelope = new byte[envelopeLength];
        input.ReadExactly(protectedEnvelope);
        var envelopeBytes = ProtectedData.Unprotect(
            protectedEnvelope, Entropy(slug), DataProtectionScope.CurrentUser);
        var envelope = JsonSerializer.Deserialize<ProtectedEnvelope>(envelopeBytes)
                       ?? throw new InvalidDataException("Invalid client cache envelope.");
        if (envelope.Key.Length != 32 || envelope.Nonce.Length != 12 || envelope.Tag.Length != 16)
            throw new InvalidDataException("Invalid client cache cryptography.");
        if (envelope.Metadata.PlaintextSize != ciphertextLength)
            throw new InvalidDataException("Encrypted client cache size mismatch.");
        return envelope;
    }

    private static (byte[] Ciphertext, ProtectedEnvelope Envelope) Read(string path, string slug)
    {
        using var input = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        Span<byte> header = stackalloc byte[20];
        input.ReadExactly(header);
        if (!header[..8].SequenceEqual(Magic)) throw new InvalidDataException("Unknown client cache format.");
        var version = BitConverter.ToInt32(header[8..12]);
        var envelopeLength = BitConverter.ToInt32(header[12..16]);
        var ciphertextLength = BitConverter.ToInt32(header[16..20]);
        if (version != FormatVersion ||
            envelopeLength <= 0 || envelopeLength > 64 * 1024 ||
            ciphertextLength <= 0 || ciphertextLength > MaxClientBytes ||
            input.Length != header.Length + envelopeLength + (long)ciphertextLength)
        {
            throw new InvalidDataException("Invalid client cache header.");
        }

        var protectedEnvelope = new byte[envelopeLength];
        input.ReadExactly(protectedEnvelope);
        var ciphertext = new byte[ciphertextLength];
        input.ReadExactly(ciphertext);
        var envelopeBytes = ProtectedData.Unprotect(
            protectedEnvelope, Entropy(slug), DataProtectionScope.CurrentUser);
        var envelope = JsonSerializer.Deserialize<ProtectedEnvelope>(envelopeBytes)
                       ?? throw new InvalidDataException("Invalid client cache envelope.");
        if (envelope.Key.Length != 32 || envelope.Nonce.Length != 12 || envelope.Tag.Length != 16)
            throw new InvalidDataException("Invalid client cache cryptography.");
        return (ciphertext, envelope);
    }

    private static byte[] Entropy(string slug) =>
        SHA256.HashData(Encoding.UTF8.GetBytes($"Oak.ClientCache.v1|{slug}|{HardwareId.Current}"));

    private static string HwidContextHash() =>
        Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(HardwareId.Current)));

    private static string RuntimeDirectory()
    {
        var dir = Path.Combine(LauncherConfig.DataDir, "runtime");
        Directory.CreateDirectory(dir);
        return dir;
    }

    private static void CleanupRuntimeDirectory()
    {
        var dir = RuntimeDirectory();
        foreach (var path in Directory.EnumerateFiles(dir, "*.dll"))
        {
            try
            {
                if (File.GetLastWriteTimeUtc(path) < DateTime.UtcNow.AddMinutes(-10))
                    File.Delete(path);
            }
            catch { /* an active loader may still hold it */ }
        }
        foreach (var path in Directory.EnumerateFiles(dir, "*.oak-bootstrap"))
            TryDelete(path);
    }

    internal static void TryDelete(string path)
    {
        try
        {
            if (!File.Exists(path)) return;
            File.SetAttributes(path, FileAttributes.Normal);
            File.Delete(path);
        }
        catch { /* best effort; next startup retries stale runtime files */ }
    }
}

public sealed class MaterializedClient(string path) : IDisposable
{
    public string Path { get; } = path;

    public void Dispose()
    {
        ProtectionHandoff.TryDelete(Path);
        EncryptedClientCache.TryDelete(Path);
    }
}
