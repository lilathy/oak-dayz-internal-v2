using System.IO;
using System.Net;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Net.Http.Json;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace OakLauncher;

public sealed class ApiClient : IDisposable
{
    private static readonly JsonSerializerOptions JsonOpts = new()
    {
        PropertyNameCaseInsensitive = true,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull
    };

    private readonly HttpClient _http;
    private readonly SemaphoreSlim _refreshLock = new(1, 1);

    public string BaseUrl { get; }
    public string? AccessToken { get; private set; }
    public string? RefreshToken { get; private set; }
    public UserDto? User { get; private set; }
    public bool IsAuthenticated => !string.IsNullOrEmpty(AccessToken);

    /// <summary>Raised whenever the refresh token rotates, so it can be re-persisted.</summary>
    public event Action<ApiClient>? SessionChanged;

    public ApiClient(string baseUrl)
    {
        BaseUrl = baseUrl.TrimEnd('/');
        _http = new HttpClient(new SocketsHttpHandler
        {
            ConnectTimeout = TimeSpan.FromSeconds(10),
            PooledConnectionLifetime = TimeSpan.FromMinutes(5),
        })
        {
            BaseAddress = new Uri(BaseUrl + "/"),
            // Client downloads can be tens of megabytes on a slow line.
            Timeout = TimeSpan.FromMinutes(10),
        };
        _http.DefaultRequestHeaders.Accept.Add(new MediaTypeWithQualityHeaderValue("application/json"));
        _http.DefaultRequestHeaders.UserAgent.ParseAdd($"OakLauncher/{LauncherConfig.Version}");
    }

    // ---------------------------------------------------------------- plumbing

    /// <summary>
    /// Sends a request and, on 401, refreshes the access token once and replays
    /// it. Access tokens expire after 15 minutes, so without this the launcher
    /// starts failing every call after sitting idle.
    /// </summary>
    private async Task<HttpResponseMessage> SendAsync(
        Func<HttpRequestMessage> factory,
        HttpCompletionOption completion = HttpCompletionOption.ResponseContentRead,
        bool allowRefresh = true,
        CancellationToken ct = default)
    {
        var res = await _http.SendAsync(Authorize(factory()), completion, ct);
        if (res.StatusCode != HttpStatusCode.Unauthorized || !allowRefresh || string.IsNullOrEmpty(RefreshToken))
            return res;

        res.Dispose();
        if (!await TryRefreshAsync(ct))
            throw new ApiException("session_expired", 401);

        return await _http.SendAsync(Authorize(factory()), completion, ct);
    }

    private HttpRequestMessage Authorize(HttpRequestMessage req)
    {
        if (!string.IsNullOrEmpty(AccessToken))
            req.Headers.Authorization = new AuthenticationHeaderValue("Bearer", AccessToken);
        return req;
    }

    private static HttpRequestMessage Json(HttpMethod method, string path, object? body = null)
    {
        var req = new HttpRequestMessage(method, path);
        if (body != null)
            req.Content = new StringContent(JsonSerializer.Serialize(body, JsonOpts), Encoding.UTF8, "application/json");
        return req;
    }

    private static async Task<T> ReadAsync<T>(HttpResponseMessage res, CancellationToken ct)
    {
        var body = await res.Content.ReadAsStringAsync(ct);
        if (!res.IsSuccessStatusCode)
            throw ApiException.FromBody(body, (int)res.StatusCode);
        return JsonSerializer.Deserialize<T>(body, JsonOpts)
               ?? throw new ApiException("bad_response", (int)res.StatusCode);
    }

    private async Task<T> CallAsync<T>(Func<HttpRequestMessage> factory, CancellationToken ct)
    {
        using var res = await SendAsync(factory, ct: ct);
        return await ReadAsync<T>(res, ct);
    }

    // ------------------------------------------------------------------- auth

    public async Task<AuthResponse> LoginAsync(string login, string password, CancellationToken ct = default)
    {
        using var res = await _http.SendAsync(
            Json(HttpMethod.Post, "v1/auth/login", new { login, password }), ct);
        var auth = await ReadAsync<AuthResponse>(res, ct);
        ApplyAuth(auth.User, auth.AccessToken, auth.RefreshToken);
        return auth;
    }

    /// <summary>Restores a saved session. Returns false if the token is stale.</summary>
    public async Task<bool> ResumeAsync(string refreshToken, CancellationToken ct = default)
    {
        RefreshToken = refreshToken;
        if (!await TryRefreshAsync(ct))
        {
            RefreshToken = null;
            return false;
        }
        try
        {
            var me = await CallAsync<MeResponse>(() => Json(HttpMethod.Get, "v1/auth/me"), ct);
            User = me.User;
            return User != null;
        }
        catch (ApiException)
        {
            Clear();
            return false;
        }
    }

    private async Task<bool> TryRefreshAsync(CancellationToken ct)
    {
        // Several calls can 401 at once; only one of them should burn the token.
        await _refreshLock.WaitAsync(ct);
        try
        {
            if (string.IsNullOrEmpty(RefreshToken)) return false;
            using var res = await _http.SendAsync(
                Json(HttpMethod.Post, "v1/auth/refresh", new { refreshToken = RefreshToken }), ct);
            if (!res.IsSuccessStatusCode)
            {
                Clear();
                return false;
            }
            var body = await res.Content.ReadAsStringAsync(ct);
            var auth = JsonSerializer.Deserialize<AuthResponse>(body, JsonOpts);
            if (auth == null || string.IsNullOrEmpty(auth.AccessToken))
            {
                Clear();
                return false;
            }
            ApplyAuth(User, auth.AccessToken, auth.RefreshToken);
            return true;
        }
        catch (HttpRequestException)
        {
            // Network blip: keep the token so the next attempt can retry.
            return false;
        }
        finally
        {
            _refreshLock.Release();
        }
    }

    public async Task LogoutAsync(CancellationToken ct = default)
    {
        try
        {
            if (IsAuthenticated)
            {
                using var res = await SendAsync(
                    () => Json(HttpMethod.Post, "v1/auth/logout", new { refreshToken = RefreshToken }),
                    allowRefresh: false, ct: ct);
            }
        }
        catch { /* signing out locally matters more than the server round-trip */ }
        finally
        {
            Clear();
        }
    }

    public Task<MeResponse> MeAsync(CancellationToken ct = default) =>
        CallAsync<MeResponse>(() => Json(HttpMethod.Get, "v1/auth/me"), ct);

    private void ApplyAuth(UserDto? user, string accessToken, string refreshToken)
    {
        User = user ?? User;
        AccessToken = accessToken;
        if (!string.IsNullOrEmpty(refreshToken))
            RefreshToken = refreshToken;
        SessionChanged?.Invoke(this);
    }

    public void Clear()
    {
        AccessToken = null;
        RefreshToken = null;
        User = null;
    }

    // ------------------------------------------------------------------- hwid

    public Task<HwidResponse> BindHwidAsync(string hwid, CancellationToken ct = default) =>
        CallAsync<HwidResponse>(() => Json(HttpMethod.Post, "v1/hwid/bind", new { hwid }), ct);

    public Task<HwidResponse> MigrateHwidAsync(string from, string to, CancellationToken ct = default) =>
        CallAsync<HwidResponse>(() => Json(HttpMethod.Post, "v1/hwid/migrate", new { from, to }), ct);

    public Task<OkResponse> ResetHwidAsync(CancellationToken ct = default) =>
        CallAsync<OkResponse>(() => Json(HttpMethod.Post, "v1/hwid/reset"), ct);

    /// <summary>
    /// Binds this machine, transparently upgrading a binding that was made with
    /// an older fingerprint recipe so an algorithm change never locks anyone out.
    /// </summary>
    public async Task<HwidResponse> BindOrMigrateHwidAsync(CancellationToken ct = default)
    {
        try
        {
            return await BindHwidAsync(HardwareId.Current, ct);
        }
        catch (ApiException ex) when (ex.Error == "hwid_mismatch")
        {
            return await MigrateHwidAsync(HardwareId.Legacy, HardwareId.Current, ct);
        }
    }

    // --------------------------------------------------------------- products

    public Task<ProductsResponse> GetProductsAsync(CancellationToken ct = default) =>
        CallAsync<ProductsResponse>(() => Json(HttpMethod.Get, "v1/products"), ct);

    public Task<LaunchInfoResponse> GetLaunchInfoAsync(string slug, CancellationToken ct = default) =>
        CallAsync<LaunchInfoResponse>(
            () => Json(HttpMethod.Get, $"v1/products/{Uri.EscapeDataString(slug)}/launch-info"), ct);

    /// <summary>
    /// Requests a one-time bootstrap ticket and immediately exchanges it for a
    /// lease from the launcher process. The injected client must not dial the
    /// API itself — BattlEye terminates DayZ when WinHttp runs in-game.
    /// </summary>
    public async Task<BootstrapHandoffDto> CreateBootstrapAsync(string slug, CancellationToken ct = default)
    {
        var handoff = await CallAsync<BootstrapResponse>(
            () => Json(HttpMethod.Post, $"v1/products/{Uri.EscapeDataString(slug)}/bootstrap",
                new { hwid = HardwareId.Current }),
            ct);
        if (string.IsNullOrWhiteSpace(handoff.BootstrapTicket) ||
            string.IsNullOrWhiteSpace(handoff.ExpiresAt))
            throw new ApiException("invalid_bootstrap_response", 0);

        var clientNonce = Convert.ToBase64String(RandomNumberGenerator.GetBytes(24))
            .TrimEnd('=').Replace('+', '-').Replace('/', '_');
        using var leaseReq = new HttpRequestMessage(HttpMethod.Post,
            $"v1/products/{Uri.EscapeDataString(slug)}/lease");
        leaseReq.Content = JsonContent.Create(new
        {
            bootstrapTicket = handoff.BootstrapTicket,
            hwid = HardwareId.Current,
            clientNonce,
        });
        // Lease is intentionally unauthenticated (ticket is the credential).
        using var leaseRes = await _http.SendAsync(leaseReq, ct);
        var leaseJson = await leaseRes.Content.ReadAsStringAsync(ct);
        if (!leaseRes.IsSuccessStatusCode || string.IsNullOrWhiteSpace(leaseJson))
            throw new ApiException("lease_prefetch_failed", (int)leaseRes.StatusCode);

        return new BootstrapHandoffDto
        {
            ProductSlug = slug,
            BootstrapTicket = handoff.BootstrapTicket,
            ExpiresAt = handoff.ExpiresAt,
            ClientReleaseId = handoff.Release?.Id ?? "",
            ClientNonce = clientNonce,
            LeaseJson = leaseJson,
        };
    }

    public Task<RedeemResponse> RedeemAsync(string code, CancellationToken ct = default) =>
        CallAsync<RedeemResponse>(() => Json(HttpMethod.Post, "v1/licenses/redeem", new { code }), ct);

    /// <summary>
    /// Best-effort launcher telemetry. Swallows transport errors so ops never
    /// blocks a launch path.
    /// </summary>
    public async Task TrackLauncherEventAsync(
        string eventName,
        IReadOnlyDictionary<string, object?>? meta = null,
        CancellationToken ct = default)
    {
        if (string.IsNullOrWhiteSpace(eventName) || !IsAuthenticated) return;
        try
        {
            var body = new Dictionary<string, object?>
            {
                ["event"] = eventName,
            };
            if (meta is { Count: > 0 })
                body["meta"] = meta;
            using var res = await SendAsync(
                () => Json(HttpMethod.Post, "v1/telemetry/launcher", body),
                ct: ct);
            res.Dispose();
        }
        catch
        {
            /* ignore */
        }
    }

    // --------------------------------------------------------------- download

    /// <summary>
    /// Fetches and verifies the current client build, then stores it in an
    /// authenticated, per-user encrypted cache. Plaintext exists only in an
    /// OS-delete-on-close staging file during this operation.
    /// </summary>
    public async Task<bool> EnsureClientAsync(
        string slug,
        string cachePath,
        string releaseId,
        string? expectedSha256,
        IProgress<double>? progress = null,
        CancellationToken ct = default)
    {
        if (!string.IsNullOrEmpty(expectedSha256) &&
            EncryptedClientCache.IsCurrent(cachePath, slug, releaseId, expectedSha256))
        {
            return false;
        }
        if (string.IsNullOrWhiteSpace(expectedSha256) || string.IsNullOrWhiteSpace(releaseId))
            throw new ApiException("client_metadata_incomplete", 0);

        Directory.CreateDirectory(Path.GetDirectoryName(cachePath)!);
        EncryptedClientCache.RemoveLegacyPlaintext(Path.GetDirectoryName(cachePath)!);
        var stagingPath = Path.Combine(
            Path.GetDirectoryName(cachePath)!,
            $".{Guid.NewGuid():N}.download");

        using var res = await SendAsync(
            () => Json(HttpMethod.Get, $"v1/products/{Uri.EscapeDataString(slug)}/client"),
            HttpCompletionOption.ResponseHeadersRead, ct: ct);

        if (!res.IsSuccessStatusCode)
        {
            var err = await res.Content.ReadAsStringAsync(ct);
            throw ApiException.FromBody(err, (int)res.StatusCode);
        }

        var total = res.Content.Headers.ContentLength ?? 0;
        var serverSha = Header(res, "X-Oak-Client-Sha256") ?? expectedSha256;

        try
        {
            await using (var src = await res.Content.ReadAsStreamAsync(ct))
            await using (var dst = new FileStream(
                             stagingPath, FileMode.CreateNew, FileAccess.ReadWrite, FileShare.Read,
                             1024 * 1024,
                             FileOptions.Asynchronous | FileOptions.SequentialScan | FileOptions.DeleteOnClose))
            using (var sha = SHA256.Create())
            {
                var buffer = new byte[1024 * 1024];
                long read = 0;
                int n;
                while ((n = await src.ReadAsync(buffer, ct)) > 0)
                {
                    await dst.WriteAsync(buffer.AsMemory(0, n), ct);
                    sha.TransformBlock(buffer, 0, n, null, 0);
                    read += n;
                    if (total > 0) progress?.Report((double)read / total);
                }
                sha.TransformFinalBlock([], 0, 0);
                var actualSha = Convert.ToHexString(sha.Hash!);
                if (!actualSha.Equals(serverSha, StringComparison.OrdinalIgnoreCase))
                    throw new ApiException("client_checksum_mismatch", 0);
                await dst.FlushAsync(ct);
                dst.Position = 0;
                await EncryptedClientCache.StoreAsync(
                    dst, cachePath, slug, releaseId, actualSha, ct);
            }
        }
        finally { TryDelete(stagingPath); }

        progress?.Report(1);
        return true;
    }

    private static void TryDelete(string path)
    {
        try
        {
            if (File.Exists(path)) File.Delete(path);
        }
        catch { /* ignore */ }
    }

    private static string? Header(HttpResponseMessage res, string name) =>
        res.Headers.TryGetValues(name, out var v) ? v.FirstOrDefault() : null;

    public static string FileSha256(string path)
    {
        using var fs = File.OpenRead(path);
        using var sha = SHA256.Create();
        return Convert.ToHexString(sha.ComputeHash(fs));
    }

    /// <summary>Cheap reachability probe used before showing the sign-in form.</summary>
    public async Task<bool> PingAsync(CancellationToken ct = default)
    {
        try
        {
            using var cts = CancellationTokenSource.CreateLinkedTokenSource(ct);
            cts.CancelAfter(TimeSpan.FromSeconds(6));
            using var res = await _http.GetAsync("health", cts.Token);
            return res.IsSuccessStatusCode;
        }
        catch
        {
            return false;
        }
    }

    public void Dispose()
    {
        _http.Dispose();
        _refreshLock.Dispose();
    }
}

public sealed class ApiException(string error, int status, string? reason = null) : Exception(error)
{
    public string Error { get; } = error;
    public int Status { get; } = status;
    /// <summary>Server-supplied detail, e.g. the ban reason.</summary>
    public string? Reason { get; } = reason;

    public static ApiException FromBody(string body, int status)
    {
        try
        {
            using var doc = JsonDocument.Parse(body);
            var error = doc.RootElement.TryGetProperty("error", out var e) ? e.GetString() : null;
            var reason = doc.RootElement.TryGetProperty("reason", out var r) ? r.GetString() : null;
            if (reason == null && doc.RootElement.TryGetProperty("hint", out var h))
                reason = h.GetString();
            if (!string.IsNullOrEmpty(error))
                return new ApiException(error, status, reason);
        }
        catch { /* not JSON */ }
        return new ApiException(string.IsNullOrWhiteSpace(body) ? "request_failed" : body, status);
    }
}

public sealed class AuthResponse
{
    public UserDto? User { get; set; }
    public LicenseDto? License { get; set; }
    public HwidDto? Hwid { get; set; }
    public string AccessToken { get; set; } = "";
    public string RefreshToken { get; set; } = "";
    public int ExpiresIn { get; set; }
}

public sealed class MeResponse
{
    public UserDto? User { get; set; }
    public LicenseDto? License { get; set; }
    public HwidDto? Hwid { get; set; }
}

public sealed class OkResponse
{
    public bool Ok { get; set; }
    public string? Message { get; set; }
}

public sealed class HwidResponse
{
    public HwidDto? Hwid { get; set; }
}

public sealed class ProductsResponse
{
    public List<ProductDto> Products { get; set; } = new();
}

public sealed class LaunchInfoResponse
{
    public ProductDto? Product { get; set; }
    public LicenseDto? License { get; set; }
    public HwidDto? Hwid { get; set; }
    public ClientReleaseDto? Client { get; set; }
    public LaunchDto? Launch { get; set; }
}

public sealed class ClientReleaseDto
{
    public string Id { get; set; } = "";
    public string Version { get; set; } = "";
    public string Sha256 { get; set; } = "";
    public long SizeBytes { get; set; }
    public string? UploadedAt { get; set; }
    public string Filename { get; set; } = "";
}

public sealed class BootstrapResponse
{
    public string BootstrapTicket { get; set; } = "";
    public string ExpiresAt { get; set; } = "";
    public BootstrapReleaseDto? Release { get; set; }
    public string? RuntimePackageVersion { get; set; }
}

public sealed class BootstrapReleaseDto
{
    public string Id { get; set; } = "";
    public string Version { get; set; } = "";
}

public sealed class BootstrapHandoffDto
{
    public string ProductSlug { get; set; } = "";
    public string BootstrapTicket { get; set; } = "";
    public string ExpiresAt { get; set; } = "";
    public string ClientReleaseId { get; set; } = "";

    /// <summary>
    /// Raw JSON body from POST /lease, prefetched by the launcher so the
    /// injected client never needs WinHttp inside DayZ (BE kills that).
    /// </summary>
    public string? LeaseJson { get; set; }

    /// <summary>Nonce used for the prefetched lease exchange.</summary>
    public string? ClientNonce { get; set; }
}

public sealed class UserDto
{
    public string Id { get; set; } = "";
    public string Email { get; set; } = "";
    public string Username { get; set; } = "";
    public string Role { get; set; } = "";
    public bool Banned { get; set; }
}

public sealed class LicenseDto
{
    public string Status { get; set; } = "";
    public string Plan { get; set; } = "";
    public string? ExpiresAt { get; set; }
    public long? RemainingSeconds { get; set; }
    public int? RemainingDays { get; set; }
    public bool Lifetime { get; set; }
}

public sealed class RedeemResponse
{
    public bool Ok { get; set; }
    public int CreditedDays { get; set; }
    public string PlanId { get; set; } = "";
    public LicenseDto? License { get; set; }
}

public sealed class HwidDto
{
    public bool Bound { get; set; }
    public string? Hint { get; set; }
    public string? BoundAt { get; set; }
    public string? LastSeenAt { get; set; }
}

public sealed class ProductDto
{
    public string Id { get; set; } = "";
    public string Slug { get; set; } = "";
    public string Name { get; set; } = "";
    public string Description { get; set; } = "";
    public string Status { get; set; } = "";
    public string LatestVersion { get; set; } = "";
    public string MinLauncher { get; set; } = "";
    public string Changelog { get; set; } = "";
    public string? UpdatedAt { get; set; }
}

public sealed class LaunchDto
{
    public bool CanInject { get; set; }
    public List<string> Reasons { get; set; } = new();
    public string DllPath { get; set; } = "";
    public string LoaderPath { get; set; } = "";
    public string ClientVersion { get; set; } = "";
    public bool FetchClient { get; set; }
}
