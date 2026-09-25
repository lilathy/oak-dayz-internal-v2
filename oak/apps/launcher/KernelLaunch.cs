using System.Diagnostics;
using System.IO;

namespace OakLauncher;

/// <summary>
/// Product-aware launch entry. DayZ uses the BattlEye kernel path; other
/// products return a clear "coming soon" until their backends are wired.
/// </summary>
public static class ProductLaunch
{
    public static async Task<KernelLaunch.Result> RunAsync(
        string productSlug,
        string plaintextDllPath,
        BootstrapHandoffDto? bootstrap = null,
        IProgress<(double Fraction, string Message)>? progress = null,
        CancellationToken ct = default,
        bool startGame = true)
    {
        var profile = ProductRegistry.TryGet(productSlug);
        if (profile is null)
            return new KernelLaunch.Result(false, "Unknown product.", "");
        if (!profile.LaunchImplemented)
            return new KernelLaunch.Result(false,
                $"{profile.DisplayName} launch is not available yet.", "");
        if (profile.Slug == "dayz")
            return await KernelLaunch.RunAsync(profile, plaintextDllPath, bootstrap, progress, ct, startGame);
        if (profile.Slug == "cs2")
            return await KernelLaunch.RunCs2Async(profile, plaintextDllPath, bootstrap, progress, ct, startGame);
        return new KernelLaunch.Result(false, "No launch backend for " + profile.Slug, "");
    }

    public static bool RuntimeAvailable(string productSlug)
    {
        var profile = ProductRegistry.TryGet(productSlug);
        if (profile is null || !profile.LaunchImplemented) return false;
        return KernelLaunch.RuntimeAvailable(profile);
    }
}

/// <summary>
/// BattlEye-compatible DayZ launch path:
/// stage DLL under <c>C:\oak\dayz\</c> → start oak_loader + oak.sys → DayZ_BE → wait.
/// </summary>
public static class KernelLaunch
{
    public sealed record Result(bool Success, string Message, string Log);

    public static string? ResolveOakLoader(ProductProfile? profile = null)
    {
        foreach (var candidate in CandidatePaths(profile ?? ProductRegistry.Dayz, "oak_loader.exe"))
            if (File.Exists(candidate)) return candidate;
        return null;
    }

    public static string? ResolveOakSys(ProductProfile? profile = null)
    {
        foreach (var candidate in CandidatePaths(profile ?? ProductRegistry.Dayz, "oak.sys"))
            if (File.Exists(candidate)) return candidate;
        return null;
    }

    public static bool RuntimeAvailable() => RuntimeAvailable(ProductRegistry.Dayz);

    public static bool RuntimeAvailable(ProductProfile profile) =>
        ResolveOakLoader(profile) != null && ResolveOakSys(profile) != null;

    /// <summary>Legacy entry — DayZ profile.</summary>
    public static Task<Result> RunAsync(
        string plaintextDllPath,
        BootstrapHandoffDto? bootstrap = null,
        IProgress<(double Fraction, string Message)>? progress = null,
        CancellationToken ct = default,
        bool startGame = true) =>
        RunAsync(ProductRegistry.Dayz, plaintextDllPath, bootstrap, progress, ct, startGame);

    public static async Task<Result> RunAsync(
        ProductProfile profile,
        string plaintextDllPath,
        BootstrapHandoffDto? bootstrap = null,
        IProgress<(double Fraction, string Message)>? progress = null,
        CancellationToken ct = default,
        bool startGame = true)
    {
        if (profile.Slug != "dayz")
            return new Result(false, "KernelLaunch only implements DayZ.", "");

        if (!File.Exists(plaintextDllPath))
            return new Result(false, "Client file missing.", "");

        var loaderSrc = ResolveOakLoader(profile);
        var sysSrc = ResolveOakSys(profile);
        if (loaderSrc == null || sysSrc == null)
            return new Result(false,
                "oak_loader.exe / oak.sys not found. Place them in C:\\oak\\dayz, C:\\oak, or next to the launcher.",
                "");

        var cfg = LauncherConfig.Load();
        if (!VerifyPinnedHash(loaderSrc, cfg.ExpectedLoaderSha256, out var loaderErr))
            return new Result(false, loaderErr, "");
        if (!VerifyPinnedHash(sysSrc, cfg.ExpectedSysSha256, out var sysErr))
            return new Result(false, sysErr, "");

        void Report(double f, string msg) => progress?.Report((f, msg));

        var stageDir = profile.StageDir;
        var stagedDll = profile.StagedDll;
        var stagedSys = Path.Combine(stageDir, "oak.sys");
        var stagedLoader = Path.Combine(stageDir, "oak_loader.exe");
        var injectLog = profile.InjectLog;
        var attachFlag = profile.AttachFlag;

        Report(0.05, "Preparing launch…");
        s_LaunchUtc = DateTime.UtcNow;
        Directory.CreateDirectory(stageDir);
        Directory.CreateDirectory(@"C:\oak"); // shared root for ops tools

        Report(0.10, "Stopping previous DayZ / loader…");
        KillImage("DayZ_x64");
        KillImage("DayZ_BE");
        KillImage("DayZLauncher");
        KillImage("oak_loader");
        await Task.Delay(600, ct);

        ClearMarkers(profile);

        // New oak.sys maps C:\oak\dayz\oak_payload.dll only. Stale inject threads
        // from prior maps still look for dayz_internal.dll — do not stage that
        // path, or a zombie thread will double-inject and fight Present.
        var payloadDll = Path.Combine(stageDir, "oak_payload.dll");
        var legacyDll = @"C:\oak\dayz_internal.dll";

        Report(0.20, "Staging client…");
        try
        {
            StageFile(plaintextDllPath, payloadDll);
            // Remove legacy inject paths so zombie driver threads fail closed.
            TryDelete(stagedDll);
            TryDelete(legacyDll);
            TryDelete(stagedDll + ".oak-bootstrap");
            TryDelete(stagedDll + ".oak-lease");
            TryDelete(legacyDll + ".oak-bootstrap");
            TryDelete(legacyDll + ".oak-lease");
            StageFile(sysSrc, stagedSys);
            try { StageFile(loaderSrc, stagedLoader); }
            catch when (File.Exists(stagedLoader)) { /* loader may be locked; existing is fine */ }
            if (!File.Exists(stagedLoader))
                return new Result(false, "Could not stage oak_loader.exe.", "");
            if (!File.Exists(payloadDll))
                return new Result(false, "Could not stage oak_payload.dll.", "");
            if (!File.Exists(stagedSys))
                return new Result(false, "Could not stage oak.sys.", "");

            // Stage fonts for ImGui rendering under kernel injection.
            var fontDir = Path.Combine(stageDir, "fonts");
            Directory.CreateDirectory(fontDir);
            var dllDir = Path.GetDirectoryName(plaintextDllPath) ?? "";
            foreach (var font in new[] { "DMSans-Regular.ttf", "DMSans-Medium.ttf", "DMSans-SemiBold.ttf" })
            {
                var fontSrc = Path.Combine(dllDir, "fonts", font);
                if (File.Exists(fontSrc))
                    StageFile(fontSrc, Path.Combine(fontDir, font));
            }
        }
        catch (Exception ex)
        {
            return new Result(false, "Staging failed: " + ex.Message, "");
        }

        if (bootstrap != null)
        {
            // Handoff must sit beside the path the driver actually maps.
            ProtectionHandoff.Write(payloadDll, bootstrap);
            // Compat for older clients that only looked next to dayz_internal.dll.
            ProtectionHandoff.Write(Path.Combine(stageDir, profile.DllFileName), bootstrap);
        }

        if (!Injector.IsElevated())
            return new Result(false, "Run the launcher as Administrator.", "");

        Report(0.30, "Starting Steam…");
        if (!GameLauncher.IsSteamRunning())
        {
            if (!GameLauncher.TryStartSteam())
                return new Result(false, "Could not start Steam.", "");
            var steamOk = await WaitProcessAsync("steam", TimeSpan.FromSeconds(45), ct);
            if (!steamOk)
                return new Result(false, "Steam did not start in time.", "");
            await Task.Delay(2000, ct);
        }

        // Map oak.sys BEFORE DayZ_BE. BEDaisy blocks iqvw64e (NtLoadDriver 0xC0000022)
        // if BattlEye's kernel driver is already loaded.
        Report(0.36, "Stopping BEDaisy (if loaded)…");
        StopBeDaisy();

        Report(0.38, "Mapping driver (oak_loader)…");
        var loaderStartedUtc = DateTime.UtcNow;
        if (!StartOakLoader(stagedLoader, stagedSys, stageDir))
            return new Result(false, "Could not start oak_loader.exe.", "");

        string loaderMsg = "";
        var loaderOk = false;
        for (var i = 0; i < 25; i++)
        {
            await Task.Delay(120, ct);
            ct.ThrowIfCancellationRequested();
            if (TryReadLoaderResult(loaderStartedUtc, out loaderMsg))
            {
                loaderOk = true;
                break;
            }
        }
        if (!loaderOk)
            return new Result(false, loaderMsg, ReadLog(@"C:\oak\loader.log"));

        // Match LaunchDayZSafe: client Present/init takes the BE path when this flag exists.
        WriteBeLaunchFlag(stageDir);

        // BEService is demand-start; warm it so DayZ_BE/Steam do not race a cold start.
        Report(0.42, "Starting BattlEye service…");
        StartBeService();

        if (startGame)
        {
            Report(0.44, "Starting DayZ (BattlEye)…");
            if (!GameLauncher.TryStartDayZ(new Progress<string>(m => Report(0.44, m))))
                return new Result(false, "Could not start DayZ (Steam / DayZ_BE).", ReadLog(injectLog));
        }
        else
        {
            // Map + inject wait only — open DayZLauncher so the user can Play themselves.
            Report(0.44, "Opening DayZ Launcher (game not auto-started)…");
            if (!GameLauncher.TryStartDayZLauncher(new Progress<string>(m => Report(0.44, m))))
                Report(0.44, "Could not open DayZLauncher — start it yourself, then Play.");
            else
                Report(0.45, "DayZ Launcher open — hit Play when ready (Oak will inject)…");
        }

        // Auto-start: DayZ_BE usually up in <15s. Manual: wait for Play.
        var waitLoops = startGame ? 60 : 180; // 500ms ticks
        var dayzUp = false;
        for (var w = 0; w < waitLoops; w++)
        {
            ct.ThrowIfCancellationRequested();
            if (Injector.IsGameRunning())
            {
                dayzUp = true;
                break;
            }
            if ((w % 4) == 0)
            {
                Report(0.46 + w * 0.001,
                    startGame
                        ? $"Waiting for DayZ… ({w + 1}/{waitLoops})"
                        : $"Waiting for DayZ (start from Launcher)… ({w + 1}/{waitLoops})");
            }
            await Task.Delay(500, ct);
        }
        if (!dayzUp)
            return new Result(false,
                startGame
                    ? "DayZ did not start. Open DayZ from Steam once, then try Launch again."
                    : "Timed out waiting for DayZ. Open DayZLauncher, press Play, then Launch again if needed.",
                ReadLog(injectLog));

        await Task.Delay(200, ct);
        ct.ThrowIfCancellationRequested();

        if (ContainsI(ReadLog(injectLog), "refusing duplicate") ||
            ContainsI(ReadLog(@"C:\oak\inject.log"), "refusing duplicate"))
        {
            return new Result(false,
                "A previous inject is still active. Reboot once, then launch again.",
                ReadLog(@"C:\oak\inject.log"));
        }

        Report(0.50, "Waiting for inject…");

        // Fast poll: inject usually lands in a few seconds; overlay soon after.
        // Old path slept 5s × 72 (~6 min) even when Present was already live.
        const int polls = 150; // 400ms → ~60s max after game up
        var retriedLaunch = false;
        var mapped = false;
        for (var i = 0; i < polls; i++)
        {
            ct.ThrowIfCancellationRequested();
            var frac = 0.55 + (0.40 * (i + 1) / polls);
            if ((i % 3) == 0)
            {
                Report(frac, mapped
                    ? $"Waiting for overlay… ({i + 1}/{polls})"
                    : $"Waiting for inject… ({i + 1}/{polls})");
            }

            await Task.Delay(400, ct);

            var dayz = Injector.IsGameRunning();
            if (!dayz && i == 8 && !retriedLaunch && startGame)
            {
                retriedLaunch = true;
                Report(frac, "DayZ exited — launching again…");
                GameLauncher.TryStartDayZ();
            }

            // Prefer product stage log; fall back to legacy C:\oak\inject.log
            var log = ReadLog(injectLog);
            if (string.IsNullOrWhiteSpace(log))
                log = ReadLog(@"C:\oak\inject.log");
            if (ContainsI(log, "refusing duplicate"))
                return new Result(false, "Inject lock held — close oak_loader and try again.", log);
            if (!dayz && ContainsI(log, "DayZ never stabilized"))
                return new Result(false,
                    "DayZ closed before inject finished. Reboot once if you launched multiple times, then try again.",
                    log);

            var attach = File.Exists(attachFlag) || File.Exists(@"C:\oak\dll_attach.flag");
            var mapOk = ContainsI(log, "SUCCESS via spoofed") ||
                        ContainsI(log, "INJECTION COMPLETE") ||
                        ContainsI(log, "DllMain OK");
            if (mapOk && dayz && (attach || ContainsI(log, "INJECTION COMPLETE") || ContainsI(log, "SUCCESS via spoofed")))
                mapped = true;

            // Auth + Present live is enough to stop waiting — don't require overlay_ready
            // (BE soak used to delay that flag for 20s+ while Present was already hooked).
            if (dayz && OverlayReady(profile))
            {
                Report(0.98, "Overlay ready…");
                var wipeProfile = profile;
                var wipeIdentity = GetFileIdentity(wipeProfile.StagedDll);
                _ = Task.Run(async () =>
                {
                    try { await Task.Delay(TimeSpan.FromMinutes(15)); }
                    catch { return; }
                    WipeStagedClientIfUnchanged(wipeProfile, wipeIdentity);
                });
                Report(1.0, "Ready — press K for menu.");
                return new Result(true, "Ready — press K for menu.", log);
            }

            if (!mapped)
                continue;

            Report(Math.Min(0.92, frac), $"Injected — waiting for render… ({i + 1}/{polls})");
        }

        var failLog = ReadLog(injectLog);
        if (string.IsNullOrWhiteSpace(failLog))
            failLog = ReadLog(@"C:\oak\inject.log");
        if (mapped)
            return new Result(false,
                "Injected, but overlay never started drawing. Check LocalAppData\\DayZ\\oak_imgui.log.",
                failLog);
        return new Result(false,
            "Injection did not complete in time. Check C:\\oak\\inject.log (and C:\\oak\\dayz\\inject.log).",
            failLog);
    }

    /// <summary>
    /// CS2 VAC path: stage under C:\oak\cs2\ → map oak.sys (targets cs2.exe) → Steam launch → wait.
    /// </summary>
    public static async Task<Result> RunCs2Async(
        ProductProfile profile,
        string plaintextDllPath,
        BootstrapHandoffDto? bootstrap = null,
        IProgress<(double Fraction, string Message)>? progress = null,
        CancellationToken ct = default,
        bool startGame = true)
    {
        if (profile.Slug != "cs2")
            return new Result(false, "RunCs2Async requires cs2 profile.", "");

        if (!File.Exists(plaintextDllPath))
            return new Result(false, "Client file missing.", "");

        var loaderSrc = ResolveOakLoader(profile);
        var sysSrc = ResolveOakSys(profile);
        if (loaderSrc == null || sysSrc == null)
            return new Result(false,
                "oak_loader.exe / oak.sys not found. Place them in C:\\oak\\cs2, C:\\oak, or next to the launcher.",
                "");

        var cfg = LauncherConfig.Load();
        if (!VerifyPinnedHash(loaderSrc, cfg.ExpectedLoaderSha256, out var loaderErr))
            return new Result(false, loaderErr, "");
        if (!VerifyPinnedHash(sysSrc, cfg.ExpectedSysSha256, out var sysErr))
            return new Result(false, sysErr, "");

        void Report(double f, string msg) => progress?.Report((f, msg));

        var stageDir = profile.StageDir;
        var stagedDll = profile.StagedDll;
        var stagedSys = Path.Combine(stageDir, "oak.sys");
        var stagedLoader = Path.Combine(stageDir, "oak_loader.exe");
        var injectLog = profile.InjectLog;
        var attachFlag = profile.AttachFlag;

        Report(0.05, "Preparing CS2 launch…");
        s_LaunchUtc = DateTime.UtcNow;
        Directory.CreateDirectory(stageDir);
        Directory.CreateDirectory(@"C:\oak");

        Report(0.10, "Stopping previous CS2 / loader…");
        KillImage("cs2");
        KillImage("oak_loader");
        await Task.Delay(2000, ct);

        ClearMarkers(profile);

        var payloadDll = Path.Combine(stageDir, "oak_payload.dll");

        Report(0.20, "Staging client…");
        try
        {
            StageFile(plaintextDllPath, payloadDll);
            TryDelete(stagedDll);
            TryDelete(stagedDll + ".oak-bootstrap");
            TryDelete(stagedDll + ".oak-lease");
            StageFile(sysSrc, stagedSys);
            try { StageFile(loaderSrc, stagedLoader); }
            catch when (File.Exists(stagedLoader)) { }
            if (!File.Exists(stagedLoader))
                return new Result(false, "Could not stage oak_loader.exe.", "");
            if (!File.Exists(payloadDll))
                return new Result(false, "Could not stage oak_payload.dll.", "");
            if (!File.Exists(stagedSys))
                return new Result(false, "Could not stage oak.sys.", "");

            var fontDir = Path.Combine(stageDir, "fonts");
            Directory.CreateDirectory(fontDir);
            var dllDir = Path.GetDirectoryName(plaintextDllPath) ?? "";
            foreach (var font in new[] { "DMSans-Regular.ttf", "DMSans-Medium.ttf", "DMSans-SemiBold.ttf" })
            {
                var fontSrc = Path.Combine(dllDir, "fonts", font);
                if (File.Exists(fontSrc))
                    StageFile(fontSrc, Path.Combine(fontDir, font));
            }

            // Product-aware target marker for ops / future loaders.
            File.WriteAllText(Path.Combine(stageDir, "target.flag"), "cs2.exe");
        }
        catch (Exception ex)
        {
            return new Result(false, "Staging failed: " + ex.Message, "");
        }

        if (bootstrap != null)
            ProtectionHandoff.Write(payloadDll, bootstrap);

        if (!Injector.IsElevated())
            return new Result(false, "Run the launcher as Administrator.", "");

        Report(0.30, "Starting Steam…");
        if (!GameLauncher.IsSteamRunning())
        {
            if (!GameLauncher.TryStartSteam())
                return new Result(false, "Could not start Steam.", "");
            var steamOk = await WaitProcessAsync("steam", TimeSpan.FromSeconds(45), ct);
            if (!steamOk)
                return new Result(false, "Steam did not start in time.", "");
            await Task.Delay(2000, ct);
        }

        Report(0.38, "Mapping driver (oak_loader)…");
        var loaderStartedUtc = DateTime.UtcNow;
        if (!StartOakLoader(stagedLoader, stagedSys, stageDir))
            return new Result(false, "Could not start oak_loader.exe.", "");

        await Task.Delay(3000, ct);
        ct.ThrowIfCancellationRequested();

        if (!TryReadLoaderResult(loaderStartedUtc, out var loaderMsg))
            return new Result(false, loaderMsg, ReadLog(@"C:\oak\loader.log"));

        if (startGame)
        {
            Report(0.44, "Starting CS2…");
            if (!GameLauncher.TryStartCs2(new Progress<string>(m => Report(0.44, m))))
                return new Result(false, "Could not start CS2 via Steam.", ReadLog(injectLog));
        }
        else
        {
            Report(0.44, "Driver ready — start CS2 yourself from Steam…");
        }

        var waitLoops = startGame ? 48 : 72;
        var gameUp = false;
        for (var w = 0; w < waitLoops; w++)
        {
            ct.ThrowIfCancellationRequested();
            if (Injector.IsProductRunning(profile))
            {
                gameUp = true;
                break;
            }
            Report(0.46 + w * 0.002,
                startGame
                    ? $"Waiting for CS2… ({w + 1}/{waitLoops})"
                    : $"Waiting for you to start CS2… ({w + 1}/{waitLoops})");
            await Task.Delay(5000, ct);
        }
        if (!gameUp)
            return new Result(false,
                startGame
                    ? "CS2 did not start. Open Counter-Strike 2 from Steam once, then try Launch again."
                    : "Timed out waiting for CS2. Start it from Steam, then hit Launch again if needed.",
                ReadLog(injectLog));

        await Task.Delay(1000, ct);

        if (ContainsI(ReadLog(injectLog), "refusing duplicate") ||
            ContainsI(ReadLog(@"C:\oak\inject.log"), "refusing duplicate"))
        {
            return new Result(false,
                "A previous inject is still active. Reboot once, then launch again.",
                ReadLog(@"C:\oak\inject.log"));
        }

        Report(0.50, "Waiting for inject…");
        const int polls = 72;
        var retriedLaunch = false;
        var mapped = false;
        for (var i = 0; i < polls; i++)
        {
            ct.ThrowIfCancellationRequested();
            var frac = 0.55 + (0.40 * (i + 1) / polls);
            Report(frac, mapped
                ? $"Waiting for overlay… ({i + 1}/{polls})"
                : $"Waiting for inject… ({i + 1}/{polls})");

            await Task.Delay(5000, ct);

            var running = Injector.IsProductRunning(profile);
            if (!running && i == 2 && !retriedLaunch)
            {
                retriedLaunch = true;
                Report(frac, "CS2 exited — launching again…");
                GameLauncher.TryStartCs2();
            }

            var log = ReadLog(injectLog);
            if (string.IsNullOrWhiteSpace(log))
                log = ReadLog(@"C:\oak\inject.log");
            if (ContainsI(log, "refusing duplicate"))
                return new Result(false, "Inject lock held — close oak_loader and try again.", log);

            var attach = File.Exists(attachFlag) || File.Exists(@"C:\oak\dll_attach.flag");
            var mapOk = ContainsI(log, "SUCCESS via spoofed") ||
                        ContainsI(log, "INJECTION COMPLETE") ||
                        ContainsI(log, "DllMain OK");
            if (mapOk && running && (attach || ContainsI(log, "INJECTION COMPLETE") || ContainsI(log, "SUCCESS via spoofed")))
                mapped = true;

            if (!mapped)
                continue;

            if (OverlayReady(profile))
            {
                Report(0.98, "Overlay rendering…");
                var wipeProfile = profile;
                var wipeIdentity = GetFileIdentity(wipeProfile.StagedDll);
                // Prefer payload identity when that is what we staged.
                var payloadIdentity = GetFileIdentity(payloadDll);
                _ = Task.Run(async () =>
                {
                    try { await Task.Delay(TimeSpan.FromMinutes(15)); }
                    catch { return; }
                    if (payloadIdentity is not null)
                    {
                        var cur = GetFileIdentity(payloadDll);
                        if (cur is not null && cur.Value == payloadIdentity.Value)
                            WipeStagedClient(wipeProfile);
                    }
                    else
                        WipeStagedClientIfUnchanged(wipeProfile, wipeIdentity);
                });
                Report(1.0, "Ready — press K for menu.");
                return new Result(true, "Ready — press K for menu.", log);
            }
        }

        var failLog = ReadLog(injectLog);
        if (string.IsNullOrWhiteSpace(failLog))
            failLog = ReadLog(@"C:\oak\inject.log");
        if (mapped)
            return new Result(false,
                "Injected, but overlay never started drawing. Check LocalAppData\\Oak\\cs2_imgui.log.",
                failLog);
        return new Result(false,
            "Injection did not complete in time. Check C:\\oak\\inject.log (and C:\\oak\\cs2\\inject.log).",
            failLog);
    }

    private static DateTime s_LaunchUtc = DateTime.MinValue;

    private static bool OverlayReady(ProductProfile profile)
    {
        foreach (var path in new[]
                 {
                     Path.Combine(profile.StageDir, "overlay_ready.flag"),
                     @"C:\oak\overlay_ready.flag",
                     Path.Combine(profile.StageDir, "imgui_init.flag"),
                     @"C:\oak\imgui_init.flag",
                 })
        {
            try
            {
                if (!File.Exists(path)) continue;
                // Ignore leftover flags from a previous session (caused false "overlay loaded").
                var written = File.GetLastWriteTimeUtc(path);
                if (s_LaunchUtc != DateTime.MinValue && written < s_LaunchUtc)
                    continue;
                // Prefer the dedicated render beacon.
                if (path.EndsWith("overlay_ready.flag", StringComparison.OrdinalIgnoreCase))
                {
                    // Auth must not have failed — otherwise Present never runs and this is stale/fake.
                    // Use ReadFlagFile (NOT ReadLog) — ReadLog falls back to inject.log.
                    var auth = ReadFlagFile(Path.Combine(profile.StageDir, "protect_auth.flag"));
                    if (string.IsNullOrWhiteSpace(auth))
                        auth = ReadFlagFile(@"C:\oak\protect_auth.flag");
                    if (!string.IsNullOrWhiteSpace(auth) &&
                        !ContainsI(auth, "authorized") &&
                        !ContainsI(auth, "lease_ok") &&
                        !ContainsI(auth, "lease_prefetch"))
                        return false;
                    return true;
                }
                // Fallback: imgui init finished (contains "ok") AND auth did not fail empty-handoff.
                var text = File.ReadAllText(path);
                if (path.EndsWith("imgui_init.flag", StringComparison.OrdinalIgnoreCase) &&
                    text.Contains("ok", StringComparison.OrdinalIgnoreCase) &&
                    text.Contains("backends_ok", StringComparison.OrdinalIgnoreCase))
                {
                    // overlay_ready is the real signal; only accept imgui_init after a short age
                    // if protect_auth looks healthy.
                    var auth = ReadFlagFile(Path.Combine(profile.StageDir, "protect_auth.flag"));
                    if (string.IsNullOrWhiteSpace(auth))
                        auth = ReadFlagFile(@"C:\oak\protect_auth.flag");
                    if (ContainsI(auth, "handoff_path_empty") || ContainsI(auth, "unavailable"))
                        return false;
                    // Without overlay_ready, do not treat imgui_init alone as done —
                    // that flag is written mid-init and caused false "complete" before.
                    return false;
                }
            }
            catch { /* ignore */ }
        }
        // Also accept live present / BE-VT markers in the imgui log (drawn frames).
        try
        {
            var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            var imguiLog = Path.Combine(local, "DayZ", "oak_imgui.log");
            if (File.Exists(imguiLog))
            {
                var written = File.GetLastWriteTimeUtc(imguiLog);
                if (s_LaunchUtc != DateTime.MinValue && written < s_LaunchUtc)
                    return false;
                var tail = ReadLogTail(imguiLog, 12000);
                if (ContainsI(tail, "protection authorization unavailable") ||
                    ContainsI(tail, "handoff_path_empty"))
                    return false;

                var authOk = ContainsI(tail, "protection authorized");
                var presentLive =
                    ContainsI(tail, "present#1 (hook live)") ||
                    ContainsI(tail, "present#100") ||
                    ContainsI(tail, "present#600") ||
                    ContainsI(tail, "BE-VT: present armed") ||
                    ContainsI(tail, "BE-VT: beat hits=");
                var imguiReady =
                    ContainsI(tail, "imgui warmed") ||
                    ContainsI(tail, "d3d ok") ||
                    ContainsI(tail, "imgui ok") ||
                    ContainsI(tail, "open=");

                // Success: auth + Present hooked. ImGui may still be soaking briefly.
                if (authOk && (presentLive || imguiReady))
                    return true;

                if (ContainsI(tail, "present#") &&
                    (ContainsI(tail, "vtx=") || imguiReady) &&
                    (imguiReady || ContainsI(tail, "present#60") || ContainsI(tail, "open=")))
                    return true;
            }
        }
        catch { /* ignore */ }
        return false;
    }

    private static string ReadLogTail(string path, int maxChars)
    {
        try
        {
            using var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            if (fs.Length <= maxChars)
            {
                using var sr = new StreamReader(fs);
                return sr.ReadToEnd();
            }
            fs.Seek(-maxChars, SeekOrigin.End);
            using var sr2 = new StreamReader(fs);
            return sr2.ReadToEnd();
        }
        catch
        {
            return "";
        }
    }

    public static void WipeStagedClient() => WipeStagedClient(ProductRegistry.Dayz);

    private readonly record struct FileIdentity(long Length, DateTime LastWriteUtc);

    private static FileIdentity? GetFileIdentity(string path)
    {
        try
        {
            if (!File.Exists(path)) return null;
            var file = new FileInfo(path);
            return new FileIdentity(file.Length, file.LastWriteTimeUtc);
        }
        catch
        {
            return null;
        }
    }

    private static void WipeStagedClientIfUnchanged(ProductProfile profile, FileIdentity? expected)
    {
        if (expected is null) return;
        var current = GetFileIdentity(profile.StagedDll);
        if (current is null || current.Value != expected.Value)
            return;
        WipeStagedClient(profile);
    }

    public static void WipeStagedClient(ProductProfile profile)
    {
        var stagedDll = profile.StagedDll;
        SecureDelete(stagedDll);
        TryDelete(ProtectionHandoff.PathForClient(stagedDll));
        TryDelete(stagedDll + ".oak-bootstrap");
        TryDelete(stagedDll + ".oak-lease");
        TryDelete(profile.DevUnlock);
        TryDelete(profile.ProtectAuthFlag);
        SecureDelete(profile.InjectLog);
        TryDelete(profile.InjectLog);
        var payload = Path.Combine(profile.StageDir, "oak_payload.dll");
        SecureDelete(payload);
        TryDelete(payload + ".oak-bootstrap");
        TryDelete(payload + ".oak-lease");
        // Legacy flat stage cleanup
        if (profile.Slug == "dayz")
        {
            SecureDelete(@"C:\oak\dayz_internal.dll");
            TryDelete(@"C:\oak\dayz_internal.dll.oak-bootstrap");
            TryDelete(@"C:\oak\dayz_internal.dll.oak-lease");
            TryDelete(@"C:\oak\oak_dev_unlock");
        }
        if (profile.Slug == "cs2")
        {
            SecureDelete(@"C:\oak\cs2_internal.dll");
            TryDelete(@"C:\oak\cs2_internal.dll.oak-bootstrap");
            TryDelete(@"C:\oak\cs2_internal.dll.oak-lease");
        }
    }

    private static bool VerifyPinnedHash(string path, string? expected, out string error)
    {
        error = "";
        if (string.IsNullOrWhiteSpace(expected))
            return true;
        try
        {
            var actual = ApiClient.FileSha256(path);
            if (!actual.Equals(expected.Trim(), StringComparison.OrdinalIgnoreCase))
            {
                error = $"Integrity check failed for {Path.GetFileName(path)}.";
                return false;
            }
            return true;
        }
        catch (Exception ex)
        {
            error = "Could not hash " + Path.GetFileName(path) + ": " + ex.Message;
            return false;
        }
    }

    private static void SecureDelete(string path)
    {
        try
        {
            if (!File.Exists(path)) return;
            TryNormalizeAttributes(path);
            var len = new FileInfo(path).Length;
            if (len > 0 && len < 256L * 1024 * 1024)
            {
                using var fs = new FileStream(path, FileMode.Open, FileAccess.Write, FileShare.None);
                var buf = new byte[Math.Min(len, 1024 * 1024)];
                Array.Clear(buf, 0, buf.Length);
                long left = len;
                while (left > 0)
                {
                    var n = (int)Math.Min(left, buf.Length);
                    fs.Write(buf, 0, n);
                    left -= n;
                }
                fs.Flush(true);
            }
        }
        catch { /* best effort before delete */ }
        TryDelete(path);
    }

    private static void StageFile(string source, string destination)
    {
        var srcFull = Path.GetFullPath(source);
        var dstFull = Path.GetFullPath(destination);
        if (srcFull.Equals(dstFull, StringComparison.OrdinalIgnoreCase))
        {
            TryNormalizeAttributes(destination);
            return;
        }

        for (var attempt = 1; attempt <= 5; attempt++)
        {
            try
            {
                TryDelete(destination);
                using (var src = new FileStream(source, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                using (var dst = new FileStream(destination, FileMode.Create, FileAccess.Write, FileShare.None))
                    src.CopyTo(dst);
                TryNormalizeAttributes(destination);
                return;
            }
            catch (IOException) when (attempt < 5)
            {
                Thread.Sleep(400 * attempt);
            }
            catch (UnauthorizedAccessException) when (attempt < 5)
            {
                TryNormalizeAttributes(destination);
                Thread.Sleep(400 * attempt);
            }
        }

        TryDelete(destination);
        using (var src = new FileStream(source, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
        using (var dst = new FileStream(destination, FileMode.Create, FileAccess.Write, FileShare.None))
            src.CopyTo(dst);
        TryNormalizeAttributes(destination);
    }

    private static void TryNormalizeAttributes(string path)
    {
        try
        {
            if (File.Exists(path))
                File.SetAttributes(path, FileAttributes.Normal);
        }
        catch { /* best effort */ }
    }

    private static IEnumerable<string> CandidatePaths(ProductProfile profile, string fileName)
    {
        yield return Path.Combine(profile.StageDir, fileName);
        yield return Path.Combine(@"C:\oak", fileName);
        yield return Path.Combine(AppContext.BaseDirectory, fileName);
        yield return Path.Combine(AppContext.BaseDirectory, "bin", fileName);
        yield return Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "bin", fileName));
        yield return Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", "bin", fileName));
        // Monorepo: apps/launcher -> ../../bin and clients/<slug>/driver/bin
        yield return Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", "..", "bin", fileName));
        yield return Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", "..", "clients", "dayz", "driver", "bin", fileName));
        yield return Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", "..", "clients", "cs2", "driver", "bin", fileName));
        yield return Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", "..", "clients", profile.Slug, "driver", "bin", fileName));
    }

    private static bool StartOakLoader(string stagedLoader, string stagedSys, string stageDir)
    {
        try
        {
            Process.Start(new ProcessStartInfo
            {
                FileName = stagedLoader,
                Arguments = $"\"{stagedSys}\"",
                WorkingDirectory = stageDir,
                UseShellExecute = false,
                CreateNoWindow = true,
            });
            return true;
        }
        catch
        {
            return false;
        }
    }

    private static bool TryReadLoaderResult(DateTime startedUtc, out string message)
    {
        message = "";
        const string path = @"C:\oak\loader.log";
        try
        {
            if (!File.Exists(path))
            {
                message = "oak_loader did not write loader.log — Intel driver mapping likely failed. Run C:\\oak\\dayz\\oak_loader.exe as Admin to see the error.";
                return false;
            }
            var written = File.GetLastWriteTimeUtc(path);
            if (written < startedUtc.AddSeconds(-2))
            {
                message = "oak_loader.log is stale — the vulnerable Intel driver (iqvw64e) did not load. Reboot once, confirm VulnerableDriverBlocklistEnable=0, then try again.";
                return false;
            }
            var text = File.ReadAllText(path);
            if (!ContainsI(text, "result=ok"))
            {
                var intel = "";
                foreach (var line in text.Split('\n'))
                {
                    if (line.StartsWith("intel_status=", StringComparison.OrdinalIgnoreCase))
                        intel = " (" + line.Trim() + ")";
                }
                message = "oak_loader failed to map oak.sys — Intel vulnerable driver blocked" + intel +
                          ". Set HKLM\\SYSTEM\\CurrentControlSet\\Control\\CI\\Config\\VulnerableDriverBlocklistEnable=0, reboot, launch as Admin.";
                return false;
            }
            return true;
        }
        catch (Exception ex)
        {
            message = "Could not read loader.log: " + ex.Message;
            return false;
        }
    }

    private static void KillImage(string processName)
    {
        try
        {
            foreach (var p in Process.GetProcessesByName(processName))
            {
                try
                {
                    p.Kill(entireProcessTree: true);
                    p.WaitForExit(3000);
                }
                catch { /* already gone / access */ }
                finally { p.Dispose(); }
            }
        }
        catch { /* ignore */ }
    }

    /// <summary>
    /// BEDaisy blocks iqvw64e load (0xC0000022). Stop the driver only — never
    /// delete/disable the service; BattlEye recreates it on next DayZ_BE start.
    /// </summary>
    private static void StopBeDaisy()
    {
        try
        {
            var psi = new ProcessStartInfo
            {
                FileName = "sc.exe",
                Arguments = "stop BEDaisy",
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
            };
            using var p = Process.Start(psi);
            p?.WaitForExit(8000);
        }
        catch { /* service may not exist yet */ }
    }

    private static void StartBeService()
    {
        try
        {
            var psi = new ProcessStartInfo
            {
                FileName = "sc.exe",
                Arguments = "start BEService",
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
            };
            using var p = Process.Start(psi);
            p?.WaitForExit(8000);
        }
        catch { /* already running / missing */ }
    }

    private static void WriteBeLaunchFlag(string stageDir)
    {
        try
        {
            Directory.CreateDirectory(stageDir);
            Directory.CreateDirectory(@"C:\oak");
            File.WriteAllText(Path.Combine(stageDir, "be_launch.flag"), "1");
            File.WriteAllText(@"C:\oak\be_launch.flag", "1");
        }
        catch { /* client can still detect BEClient if this fails */ }
    }

    private static void ClearMarkers(ProductProfile profile)
    {
        TryDelete(profile.InjectLog);
        TryDelete(profile.AttachFlag);
        TryDelete(Path.Combine(profile.StageDir, "imgui_init.flag"));
        TryDelete(Path.Combine(profile.StageDir, "mainthread.flag"));
        TryDelete(Path.Combine(profile.StageDir, "overlay_ready.flag"));
        TryDelete(Path.Combine(profile.StageDir, "protect_auth.flag"));
        TryDelete(Path.Combine(profile.StageDir, "be_launch.flag"));
        TryDelete(@"C:\oak\inject.log");
        TryDelete(@"C:\oak\dll_attach.flag");
        TryDelete(@"C:\oak\imgui_init.flag");
        TryDelete(@"C:\oak\overlay_ready.flag");
        TryDelete(@"C:\oak\protect_auth.flag");
        TryDelete(@"C:\oak\mainthread.flag");
        TryDelete(@"C:\oak\be_launch.flag");
        try
        {
            var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            TryDelete(Path.Combine(local, "DayZ", "oak_imgui.log"));
        }
        catch { /* ignore */ }
    }

    private static void TryDelete(string path)
    {
        try
        {
            if (File.Exists(path))
            {
                File.SetAttributes(path, FileAttributes.Normal);
                File.Delete(path);
            }
        }
        catch { /* best effort */ }
    }

    private static string ReadFlagFile(string path)
    {
        try
        {
            if (!File.Exists(path)) return "";
            return File.ReadAllText(path);
        }
        catch
        {
            return "";
        }
    }

    private static string ReadLog(string injectLog)
    {
        try
        {
            if (File.Exists(injectLog)) return File.ReadAllText(injectLog);
            if (File.Exists(@"C:\oak\inject.log")) return File.ReadAllText(@"C:\oak\inject.log");
            return "";
        }
        catch
        {
            return "";
        }
    }

    private static bool ContainsI(string hay, string needle) =>
        !string.IsNullOrEmpty(hay) &&
        hay.Contains(needle, StringComparison.OrdinalIgnoreCase);

    private static async Task<bool> WaitProcessAsync(string name, TimeSpan timeout, CancellationToken ct)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline)
        {
            ct.ThrowIfCancellationRequested();
            if (Process.GetProcessesByName(name).Length > 0) return true;
            await Task.Delay(500, ct);
        }
        return false;
    }
}
