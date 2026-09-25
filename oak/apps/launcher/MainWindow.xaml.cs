using System.ComponentModel;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;

namespace OakLauncher;

public partial class MainWindow : Window
{
    private readonly LauncherConfig _config = LauncherConfig.Load();
    private readonly ApiClient _api;
    private readonly DispatcherTimer _tick = new() { Interval = TimeSpan.FromSeconds(1) };
    private readonly DispatcherTimer _poll = new() { Interval = TimeSpan.FromSeconds(60) };

    private LaunchInfoResponse? _current;
    private CancellationTokenSource? _work;
    private DateTime? _expiresAtUtc;
    private bool _lifetime;
    private bool _licenseActive;
    private bool _remember = true;
    private bool _busy;
    private bool _gameRunning;
    private readonly LauncherPrefs _prefs = LauncherPrefs.Load();
    private readonly bool _autoLaunchDayZ = Environment.GetCommandLineArgs()
        .Skip(1)
        .Any(a => a.Equals("--auto-launch-dayz", StringComparison.OrdinalIgnoreCase));
    private readonly bool _autoLaunchCs2 = Environment.GetCommandLineArgs()
        .Skip(1)
        .Any(a => a.Equals("--auto-launch-cs2", StringComparison.OrdinalIgnoreCase));
    private bool _autoLaunchStarted;

    public MainWindow()
    {
        InitializeComponent();
        _api = new ApiClient(_config.ApiUrl);
        _api.SessionChanged += PersistSession;
        _tick.Tick += Tick_Elapsed;
        _poll.Tick += async (_, _) => await PollAsync();
        AutoStartGameBox.IsChecked = _prefs.AutoStartGame;
        UpdateLaunchTooltips();
    }

    // ------------------------------------------------------------------ boot

    private async void Window_Loaded(object sender, RoutedEventArgs e)
    {
        LoginApiLine.Text = $"API: {_config.ApiUrl}";
        LoginHwidLine.Text = Injector.IsElevated()
            ? "Running as Administrator."
            : "Not elevated — injection will likely fail.";
        SidebarVersion.Text = $"LAUNCHER {LauncherConfig.Version}";

        BootStatus.Text = "Contacting server…";
        if (!await _api.PingAsync())
        {
            ShowLogin($"Cannot reach the server at {_config.ApiUrl}. Check your connection and try again.");
            return;
        }

        var saved = SavedSession.Load();
        if (saved != null && saved.ApiUrl == _config.ApiUrl && !string.IsNullOrEmpty(saved.RefreshToken))
        {
            BootStatus.Text = "Restoring session…";
            try
            {
                if (await _api.ResumeAsync(saved.RefreshToken))
                {
                    await EnterAppAsync();
                    return;
                }
            }
            catch (ApiException ex) when (ex.Error == "banned")
            {
                SavedSession.Clear();
                ShowLogin(BanMessage(ex));
                return;
            }
            catch { /* fall through to the sign-in form */ }
            SavedSession.Clear();
        }

        ShowLogin(null);
    }

    private void ShowLogin(string? error)
    {
        _tick.Stop();
        _poll.Stop();
        BootView.Visibility = Visibility.Collapsed;
        AppView.Visibility = Visibility.Collapsed;
        LoginView.Visibility = Visibility.Visible;
        LoginError.Text = error ?? "";
        LoginBox.Focus();
    }

    // ----------------------------------------------------------------- login

    private void LoginField_KeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Enter) Login_Click(sender, new RoutedEventArgs());
    }

    private async void Login_Click(object sender, RoutedEventArgs e)
    {
        if (_busy) return;
        LoginError.Text = "";
        var user = LoginBox.Text.Trim();
        var pass = PasswordBox.Password;
        if (user.Length < 3 || pass.Length < 1)
        {
            LoginError.Text = "Enter your username/email and password.";
            return;
        }

        try
        {
            SetBusy(true);
            LoginBtn.Content = "Signing in…";
            _remember = RememberBox.IsChecked == true;

            await _api.LoginAsync(user, pass);
            Telemetry.Track(_api, "launcher.login_ok");

            // Bind after authenticating so a wrong password never looks like a
            // hardware problem, and so a first-time user is bound automatically.
            try
            {
                await _api.BindOrMigrateHwidAsync();
            }
            catch (ApiException ex) when (ex.Error is "hwid_mismatch")
            {
                await _api.LogoutAsync();
                LoginError.Text =
                    "This account is locked to a different PC" +
                    (string.IsNullOrEmpty(ex.Reason) ? "" : $" ({ex.Reason})") +
                    ". Reset the HWID from your account page, then sign in again.";
                return;
            }

            PersistSession(_api);
            PasswordBox.Password = "";
            await EnterAppAsync();
            Telemetry.Track(_api, "launcher.start", new Dictionary<string, object?>
            {
                ["version"] = LauncherConfig.Version,
            });
        }
        catch (ApiException ex)
        {
            Telemetry.Track(_api, "launcher.login_fail", new Dictionary<string, object?>
            {
                ["reasonCode"] = ex.Error,
            });
            LoginError.Text = LoginErrorText(ex);
        }
        catch (Exception ex)
        {
            LoginError.Text = $"Cannot reach the server at {_config.ApiUrl}.\n{ex.Message}";
        }
        finally
        {
            LoginBtn.Content = "Sign in";
            SetBusy(false);
        }
    }

    private static string LoginErrorText(ApiException ex) => ex.Error switch
    {
        "invalid_credentials" => "Wrong username or password.",
        "banned" => BanMessage(ex),
        "rate_limited" => "Too many attempts. Wait a few minutes and try again.",
        "not_bound" => "This PC is not bound yet. Try signing in again.",
        _ => ex.Error,
    };

    private static string BanMessage(ApiException ex) =>
        "This account is banned." + (string.IsNullOrEmpty(ex.Reason) ? "" : $"\nReason: {ex.Reason}");

    private void PersistSession(ApiClient api)
    {
        if (!_remember || string.IsNullOrEmpty(api.RefreshToken))
            return;
        SavedSession.Save(new SavedSession
        {
            RefreshToken = api.RefreshToken,
            Username = api.User?.Username ?? "",
            ApiUrl = _config.ApiUrl,
        });
    }

    private async void Logout_Click(object sender, RoutedEventArgs e)
    {
        _work?.Cancel();
        _tick.Stop();
        _poll.Stop();
        SavedSession.Clear();
        _current = null;
        ProductList.ItemsSource = null;
        ClearDetail();
        StatusLine.Text = "";
        await _api.LogoutAsync();
        ShowLogin(null);
    }

    // ------------------------------------------------------------------- app

    private async Task EnterAppAsync()
    {
        BootView.Visibility = Visibility.Collapsed;
        LoginView.Visibility = Visibility.Collapsed;
        AppView.Visibility = Visibility.Visible;
        SidebarUser.Text = _api.User?.Username ?? "";
        _gameRunning = Injector.IsGameRunning();
        _tick.Start();
        _poll.Start();
        await LoadProductsAsync();
    }

    private async Task LoadProductsAsync()
    {
        try
        {
            var products = await _api.GetProductsAsync();
            ProductList.DisplayMemberPath = "Name";
            ProductList.SelectedValuePath = "Slug";
            ProductList.ItemsSource = products.Products;
            if (products.Products.Count > 0)
            {
                ProductDto? requested = null;
                if (_autoLaunchDayZ)
                    requested = products.Products.FirstOrDefault(p =>
                        p.Slug.Equals("dayz", StringComparison.OrdinalIgnoreCase));
                else if (_autoLaunchCs2)
                    requested = products.Products.FirstOrDefault(p =>
                        p.Slug.Equals("cs2", StringComparison.OrdinalIgnoreCase));
                ProductList.SelectedItem = requested ?? products.Products[0];
            }
            else
                StatusLine.Text = "No products are available.";
        }
        catch (ApiException ex) when (ex.Error == "session_expired")
        {
            SavedSession.Clear();
            ShowLogin("Your session expired. Please sign in again.");
        }
        catch (Exception ex)
        {
            StatusLine.Text = "Could not load products: " + ex.Message;
        }
    }

    private async void ProductList_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (ProductList.SelectedItem is ProductDto p)
        {
            await LoadLaunchInfoAsync(p.Slug);
            if (!_autoLaunchStarted &&
                _current?.Launch?.CanInject == true &&
                LaunchBtn.IsEnabled)
            {
                if (_autoLaunchDayZ && p.Slug.Equals("dayz", StringComparison.OrdinalIgnoreCase))
                {
                    _autoLaunchStarted = true;
                    StatusLine.Text = "Steam account ready — launching DayZ with Oak…";
                    Launch_Click(LaunchBtn, new RoutedEventArgs());
                }
                else if (_autoLaunchCs2 && p.Slug.Equals("cs2", StringComparison.OrdinalIgnoreCase))
                {
                    _autoLaunchStarted = true;
                    StatusLine.Text = "Steam account ready — launching CS2 with Oak…";
                    Launch_Click(LaunchBtn, new RoutedEventArgs());
                }
            }
        }
    }

    private async Task LoadLaunchInfoAsync(string slug)
    {
        try
        {
            SetBusy(true);
            StatusLine.Text = "Loading…";
            _current = await _api.GetLaunchInfoAsync(slug);
            ApplyLaunchInfo(_current);
            StatusLine.Text = _current.Launch?.CanInject == true
                ? (_gameRunning
                    ? "Ready to launch."
                    : (_prefs.AutoStartGame
                        ? "Ready — hit Launch to start the game."
                        : "Ready — Launch maps Oak, then start the game yourself."))
                : "";
        }
        catch (ApiException ex) when (ex.Error == "session_expired")
        {
            SavedSession.Clear();
            ShowLogin("Your session expired. Please sign in again.");
        }
        catch (ApiException ex)
        {
            StatusLine.Text = ex.Error;
            LaunchBtn.IsEnabled = false;
        }
        catch (Exception ex)
        {
            StatusLine.Text = ex.Message;
            LaunchBtn.IsEnabled = false;
        }
        finally
        {
            SetBusy(false);
        }
    }

    /// <summary>Background status sync so bans, expiry and new builds show up without a click.</summary>
    private async Task PollAsync()
    {
        if (_busy || !_api.IsAuthenticated) return;
        if (ProductList.SelectedItem is not ProductDto p) return;
        try
        {
            _current = await _api.GetLaunchInfoAsync(p.Slug);
            ApplyLaunchInfo(_current);
        }
        catch (ApiException ex) when (ex.Error is "session_expired" or "banned")
        {
            SavedSession.Clear();
            ShowLogin(ex.Error == "banned" ? BanMessage(ex) : "Your session expired. Please sign in again.");
        }
        catch { /* transient — the next tick will retry */ }
    }

    private void ApplyLaunchInfo(LaunchInfoResponse info)
    {
        var product = info.Product;
        var license = info.License;
        var hwid = info.Hwid;
        var launch = info.Launch;

        ProductTitle.Text = product?.Name ?? "Product";
        ProductDesc.Text = product?.Description ?? "";

        StatProductStatus.Text = (product?.Status ?? "—").ToUpperInvariant();
        StatProductStatus.Foreground = BrushForStatus(product?.Status);
        StatVersion.Text = info.Client?.Version ?? product?.LatestVersion ?? "—";
        StatLicense.Text = (license?.Status ?? "—").ToUpperInvariant();
        StatLicense.Foreground = license?.Status == "active" ? OkBrush() : BadBrush();

        _licenseActive = license?.Status == "active";
        _lifetime = license?.Lifetime == true;
        _expiresAtUtc = DateTimeOffset.TryParse(license?.ExpiresAt, out var exp)
            ? exp.UtcDateTime
            : license?.RemainingSeconds is { } secs
                ? DateTime.UtcNow.AddSeconds(secs)
                : null;
        UpdateTimeLeft();

        DetailPlan.Text = string.IsNullOrEmpty(license?.Plan) ? "—" : license!.Plan;
        DetailExpires.Text = _lifetime
            ? "never"
            : _expiresAtUtc?.ToLocalTime().ToString("yyyy-MM-dd HH:mm") ?? "—";
        DetailHwid.Text = hwid?.Bound == true ? (hwid.Hint ?? "bound") : "not bound";
        DetailHwid.Foreground = hwid?.Bound == true ? OkBrush() : WarnBrush();
        ResetHwidBtn.IsEnabled = hwid?.Bound == true;
        DetailBuild.Text = info.Client == null
            ? "no build published"
            : $"{info.Client.Version}  ·  {FormatSize(info.Client.SizeBytes)}  ·  {Short(info.Client.Sha256)}";
        DetailUpdated.Text = FormatStamp(info.Client?.UploadedAt ?? product?.UpdatedAt);
        DetailAccount.Text = $"{_api.User?.Username}  ·  {_api.User?.Email}";
        DetailChangelog.Text = string.IsNullOrWhiteSpace(product?.Changelog) ? "—" : product!.Changelog;

        var outdated = IsLauncherOutdated(product?.MinLauncher);
        var reasons = launch?.Reasons ?? new List<string>();
        var canInject = launch?.CanInject == true && !outdated;

        if (outdated)
            ShowNotice($"This launcher is out of date. Version {product?.MinLauncher} or newer is required — " +
                       "download the latest launcher from the website.");
        else if (reasons.Count > 0)
            ShowNotice(string.Join("  ", reasons.Select(ReasonText)));
        else
            NoticeBar.Visibility = Visibility.Collapsed;

        LaunchBtn.IsEnabled = canInject && !_busy;
    }

    private void ShowNotice(string text)
    {
        NoticeText.Text = text;
        NoticeBar.Visibility = Visibility.Visible;
    }

    private static string ReasonText(string reason) => reason switch
    {
        "license_inactive" => "No active license — redeem a code below to activate.",
        "hwid_unbound" => "This PC is not bound to your account yet. Sign out and back in.",
        "no_client_release" => "No client build has been published yet.",
        "maintenance" => "Oak is under maintenance. Launch is temporarily disabled.",
        "banned" => "This account is banned.",
        "product_maintenance" => "This product is under maintenance.",
        "product_offline" => "This product is offline.",
        _ => reason,
    };

    /// <summary>Compares the running launcher against the product's minimum.</summary>
    private static bool IsLauncherOutdated(string? minLauncher)
    {
        if (string.IsNullOrWhiteSpace(minLauncher)) return false;
        return Version.TryParse(minLauncher, out var min)
               && Version.TryParse(LauncherConfig.Version, out var mine)
               && mine < min;
    }

    // -------------------------------------------------------------- countdown

    private void Tick_Elapsed(object? sender, EventArgs e)
    {
        UpdateTimeLeft();

        var running = false;
        if (ProductList.SelectedItem is ProductDto sel)
        {
            var profile = ProductRegistry.TryGet(sel.Slug);
            running = profile != null
                ? Injector.IsProductRunning(profile)
                : Injector.IsGameRunning();
        }
        else
            running = Injector.IsGameRunning();
        if (running != _gameRunning || StatGame.Text == "—")
        {
            _gameRunning = running;
            StatGame.Text = running ? "RUNNING" : "CLOSED";
            StatGame.Foreground = running ? OkBrush() : WarnBrush();
        }
    }

    private void UpdateTimeLeft()
    {
        string text;
        Brush brush;
        if (_lifetime && _licenseActive)
        {
            text = "lifetime";
            brush = OkBrush();
        }
        else if (!_licenseActive || _expiresAtUtc == null)
        {
            text = "—";
            brush = BadBrush();
        }
        else
        {
            var left = _expiresAtUtc.Value - DateTime.UtcNow;
            if (left <= TimeSpan.Zero)
            {
                text = "expired";
                brush = BadBrush();
            }
            else
            {
                text = left.TotalDays >= 1
                    ? $"{(int)left.TotalDays}d {left.Hours}h"
                    : $"{left.Hours:00}:{left.Minutes:00}:{left.Seconds:00}";
                brush = left.TotalHours < 24 ? WarnBrush() : OkBrush();
            }
        }
        StatTimeLeft.Text = text;
        StatTimeLeft.Foreground = brush;
    }

    // ----------------------------------------------------------------- hwid

    private async void ResetHwid_Click(object sender, RoutedEventArgs e)
    {
        if (_busy) return;
        var confirm = MessageBox.Show(
            "Unbind this account from the current PC?\n\n" +
            "You can only do this once every 72 hours. You will be signed out, and " +
            "the next PC you sign in from becomes the bound one.",
            "Reset HWID", MessageBoxButton.YesNo, MessageBoxImage.Warning);
        if (confirm != MessageBoxResult.Yes) return;

        try
        {
            SetBusy(true);
            await _api.ResetHwidAsync();
            SavedSession.Clear();
            await _api.LogoutAsync();
            ShowLogin("HWID reset. Sign in again to bind this PC.");
        }
        catch (ApiException ex)
        {
            StatusLine.Text = ex.Error == "reset_cooldown"
                ? "HWID was reset recently — try again later or open a support ticket."
                : "Reset failed: " + ex.Error;
        }
        catch (Exception ex)
        {
            StatusLine.Text = "Reset failed: " + ex.Message;
        }
        finally
        {
            SetBusy(false);
        }
    }

    // ---------------------------------------------------------------- redeem

    private void RedeemBox_KeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Enter) Redeem_Click(sender, new RoutedEventArgs());
    }

    private async void Redeem_Click(object sender, RoutedEventArgs e)
    {
        if (_busy) return;
        var code = RedeemBox.Text.Trim();
        if (code.Length < 8)
        {
            RedeemStatus.Text = "Enter a redeem code.";
            RedeemStatus.Foreground = BadBrush();
            return;
        }
        try
        {
            SetBusy(true);
            RedeemStatus.Text = "Redeeming…";
            RedeemStatus.Foreground = MidBrush();
            var result = await _api.RedeemAsync(code);
            RedeemBox.Text = "";
            RedeemStatus.Text = $"Added {result.CreditedDays} day(s) to your license.";
            RedeemStatus.Foreground = OkBrush();
            if (ProductList.SelectedItem is ProductDto p)
            {
                _current = await _api.GetLaunchInfoAsync(p.Slug);
                ApplyLaunchInfo(_current);
            }
        }
        catch (ApiException ex)
        {
            RedeemStatus.Text = ex.Error switch
            {
                "invalid_code" => "That code is not valid.",
                "code_already_used" => "That code has already been used.",
                "code_revoked" => "That code was revoked.",
                "license_banned" => "Your license is banned.",
                "rate_limited" => "Too many attempts. Try again later.",
                _ => ex.Error,
            };
            RedeemStatus.Foreground = BadBrush();
        }
        catch (Exception ex)
        {
            RedeemStatus.Text = ex.Message;
            RedeemStatus.Foreground = BadBrush();
        }
        finally
        {
            SetBusy(false);
        }
    }

    // ---------------------------------------------------------------- launch

    private void Cancel_Click(object sender, RoutedEventArgs e) => _work?.Cancel();

    private async void Launch_Click(object sender, RoutedEventArgs e)
    {
        if (_busy || ProductList.SelectedItem is not ProductDto p) return;

        _work = new CancellationTokenSource();
        var ct = _work.Token;
            LaunchLog.Write($"Launch clicked slug={p.Slug} elevated={Injector.IsElevated()} runtime={ProductLaunch.RuntimeAvailable(p.Slug)} autoStart={_prefs.AutoStartGame}");
            Telemetry.Track(_api, "launcher.launch_start", new Dictionary<string, object?>
            {
                ["slug"] = p.Slug,
                ["elevated"] = Injector.IsElevated(),
                ["autoStartGame"] = _prefs.AutoStartGame,
            });

        try
        {
            SetBusy(true);
            CancelBtn.Visibility = Visibility.Visible;
            ShowProgress(0, "Checking entitlement…");

            // Re-check right before launch: the license may have lapsed or the
            // build may have changed since the panel was last refreshed.
            _current = await _api.GetLaunchInfoAsync(p.Slug, ct);
            ApplyLaunchInfo(_current);
            LaunchLog.Write($"launch-info canInject={_current.Launch?.CanInject} reasons=[{string.Join(',', _current.Launch?.Reasons ?? [])}] client={_current.Client?.Id} sha={_current.Client?.Sha256}");
            if (_current.Launch?.CanInject != true)
            {
                StatusLine.Text = "Launch is not available right now.";
                LaunchLog.Write("abort: canInject=false");
                return;
            }

            var profile = ProductRegistry.TryGet(p.Slug);
            if (profile is null || !profile.LaunchImplemented)
            {
                StatusLine.Text = $"{p.Name} launch is not available yet.";
                LaunchLog.Write("abort: launch not implemented for " + p.Slug);
                return;
            }

            if (!ProductLaunch.RuntimeAvailable(p.Slug))
            {
                StatusLine.Text = "oak_loader.exe / oak.sys missing — put them in C:\\oak\\dayz or C:\\oak.";
                LaunchLog.Write($"abort: runtime missing loader={KernelLaunch.ResolveOakLoader(profile)} sys={KernelLaunch.ResolveOakSys(profile)}");
                return;
            }

            if (!Injector.IsElevated())
            {
                StatusLine.Text = "Run the launcher as Administrator.";
                LaunchLog.Write("abort: not elevated");
                return;
            }

            var fileName = string.IsNullOrWhiteSpace(_current.Client?.Filename)
                ? profile.DllFileName
                : Path.GetFileName(_current.Client!.Filename);
            var cachePath = EncryptedClientCache.CachePath(p.Slug, fileName);
            LaunchLog.Write($"cachePath={cachePath} exists={File.Exists(cachePath)}");

            ShowProgress(0.05, "Checking client build…");
            var downloadProgress = new Progress<double>(v =>
                ShowProgress(0.05 + v * 0.20, $"Downloading client… {(int)(v * 100)}%"));

            var downloaded = await _api.EnsureClientAsync(
                p.Slug,
                cachePath,
                _current.Client?.Id ?? "",
                _current.Client?.Sha256,
                downloadProgress,
                ct);
            LaunchLog.Write($"EnsureClient downloaded={downloaded}");

            ShowProgress(0.28, downloaded
                ? $"Client {_current.Client?.Version} downloaded."
                : "Client up to date.");

            ShowProgress(0.30, "Preparing protected client…");
            var cfg = LauncherConfig.Load();
            var localOverride = cfg.LocalClientDll;
            if (!string.IsNullOrWhiteSpace(localOverride) && File.Exists(localOverride))
            {
                LaunchLog.Write($"localClientDll override bytes={new FileInfo(localOverride).Length}");
                // Local lab: skip in-game signature verify for rebuilds before republish.
                try
                {
                    Directory.CreateDirectory(profile.StageDir);
                    File.WriteAllText(profile.DevUnlock, "localClientDll\n");
                }
                catch (Exception ex)
                {
                    LaunchLog.Write("oak_dev_unlock write failed: " + ex.Message);
                }
                ShowProgress(0.32, "Authorizing protected client…");
                var bootstrap = await _api.CreateBootstrapAsync(p.Slug, ct);
                LaunchLog.Write($"bootstrap ok expires={bootstrap.ExpiresAt} release={bootstrap.ClientReleaseId} leasePrefetch={!string.IsNullOrWhiteSpace(bootstrap.LeaseJson)}");
                Telemetry.Track(_api, "launcher.bootstrap_ok", new Dictionary<string, object?>
                {
                    ["releaseId"] = bootstrap.ClientReleaseId,
                });

                var launchProgress = new Progress<(double Fraction, string Message)>(t =>
                {
                    ShowProgress(0.35 + t.Fraction * 0.65, t.Message);
                    LaunchLog.Write($"kernel {(int)(t.Fraction * 100)}% {t.Message}");
                });

                var result = await ProductLaunch.RunAsync(
                    p.Slug,
                    localOverride,
                    bootstrap,
                    launchProgress,
                    ct,
                    startGame: _prefs.AutoStartGame);
                StatusLine.Text = result.Message;
                LaunchLog.Write($"kernel result success={result.Success} msg={result.Message}");
                Telemetry.Track(_api, result.Success ? "launcher.inject_ok" : "launcher.inject_fail",
                    new Dictionary<string, object?>
                    {
                        ["reasonCode"] = result.Success ? "ok" : "inject_fail",
                        ["message"] = result.Message.Length > 180 ? result.Message[..180] : result.Message,
                    });
#if DEBUG
                if (!result.Success && !string.IsNullOrWhiteSpace(result.Log))
                    LaunchLog.Write("inject.log tail:\n" + string.Join('\n', result.Log.Split('\n').TakeLast(40)));
#endif
                if (result.Success)
                {
                    ShowProgress(1.0, result.Message);
                    await Task.Delay(800, CancellationToken.None);
                }
                return;
            }
            // Encrypted cache → materialize → inject (no local override).

            using (var materialized = await EncryptedClientCache.MaterializeAsync(
                       cachePath,
                       p.Slug,
                       _current.Client?.Id ?? "",
                       _current.Client?.Sha256 ?? "",
                       fileName,
                       ct))
            {
                LaunchLog.Write($"materialized={materialized.Path} bytes={new FileInfo(materialized.Path).Length}");

                // Ticket is short-lived — request it right before the kernel
                // path stages C:\oak\dayz_internal.dll and starts DayZ_BE.
                ShowProgress(0.32, "Authorizing protected client…");
                var bootstrap = await _api.CreateBootstrapAsync(p.Slug, ct);
                LaunchLog.Write($"bootstrap ok expires={bootstrap.ExpiresAt} release={bootstrap.ClientReleaseId}");
                Telemetry.Track(_api, "launcher.bootstrap_ok", new Dictionary<string, object?>
                {
                    ["releaseId"] = bootstrap.ClientReleaseId,
                });

                var launchProgress = new Progress<(double Fraction, string Message)>(t =>
                {
                    ShowProgress(0.35 + t.Fraction * 0.65, t.Message);
                    LaunchLog.Write($"kernel {(int)(t.Fraction * 100)}% {t.Message}");
                });

                var result = await ProductLaunch.RunAsync(
                    p.Slug,
                    materialized.Path,
                    bootstrap,
                    launchProgress,
                    ct,
                    startGame: _prefs.AutoStartGame);
                StatusLine.Text = result.Message;
                LaunchLog.Write($"kernel result success={result.Success} msg={result.Message}");
                Telemetry.Track(_api, result.Success ? "launcher.inject_ok" : "launcher.inject_fail",
                    new Dictionary<string, object?>
                    {
                        ["reasonCode"] = result.Success ? "ok" : "inject_fail",
                        ["message"] = result.Message.Length > 180 ? result.Message[..180] : result.Message,
                    });
#if DEBUG
                if (!result.Success && !string.IsNullOrWhiteSpace(result.Log))
                    LaunchLog.Write("inject.log tail:\n" + string.Join('\n', result.Log.Split('\n').TakeLast(40)));
#endif
                if (result.Success)
                {
                    ShowProgress(1.0, result.Message);
                    // Keep the bar visible briefly so the user sees completion.
                    await Task.Delay(1200, CancellationToken.None);
                }
            }
        }
        catch (OperationCanceledException)
        {
            StatusLine.Text = "Cancelled.";
            LaunchLog.Write("cancelled");
        }
        catch (ApiException ex)
        {
            LaunchLog.Write(ex, "ApiException " + ex.Error);
            if (ex.Error.Contains("bootstrap", StringComparison.OrdinalIgnoreCase) ||
                ex.Error is "hwid_mismatch" or "hwid_unbound" or "no_active_runtime_package")
                Telemetry.Track(_api, "launcher.bootstrap_fail", new Dictionary<string, object?>
                {
                    ["reasonCode"] = ex.Error,
                });
            StatusLine.Text = ex.Error switch
            {
                "session_expired" or "session_revoked" => "Session expired — sign in again.",
                "client_checksum_mismatch" => "Downloaded client failed verification. Try again.",
                "license_inactive" => "Your license is not active.",
                "maintenance" => "Oak is under maintenance.",
                "no_active_runtime_package" => "No active runtime package — ask an admin to publish one.",
                "hwid_required" or "hwid_mismatch" or "hwid_unbound" =>
                    "Hardware ID mismatch — sign in again.",
                "Encrypted client cache does not match this launch." =>
                    "Protected client cache is invalid for this machine. Re-download required.",
                _ => "Launch failed: " + ex.Error,
            };
        }
        catch (InvalidDataException ex)
        {
            LaunchLog.Write(ex, "cache");
            StatusLine.Text = "Client cache invalid — try Launch again.";
        }
        catch (Exception ex)
        {
            LaunchLog.Write(ex, "Launch exception");
            StatusLine.Text = "Launch failed: " + ex.Message;
        }
        finally
        {
            HideProgress();
            CancelBtn.Visibility = Visibility.Collapsed;
            _work?.Dispose();
            _work = null;
            SetBusy(false);
            LaunchLog.Write("Launch_Click finished");
        }
    }

    private void ShowProgress(double fraction, string message)
    {
        Progress.Visibility = Visibility.Visible;
        ProgressLabel.Visibility = Visibility.Visible;
        Progress.IsIndeterminate = false;
        Progress.Minimum = 0;
        Progress.Maximum = 100;
        Progress.Value = Math.Clamp(fraction * 100, 0, 100);
        ProgressLabel.Text = $"{(int)Math.Clamp(fraction * 100, 0, 100)}% — {message}";
        StatusLine.Text = message;
    }

    private void HideProgress()
    {
        Progress.Visibility = Visibility.Collapsed;
        ProgressLabel.Visibility = Visibility.Collapsed;
        Progress.Value = 0;
        ProgressLabel.Text = "";
    }

    // ----------------------------------------------------------------- misc

    private void AutoStartGame_Changed(object sender, RoutedEventArgs e)
    {
        _prefs.AutoStartGame = AutoStartGameBox.IsChecked == true;
        _prefs.Save();
        UpdateLaunchTooltips();
        if (!_busy && _current?.Launch?.CanInject == true && !_gameRunning)
        {
            StatusLine.Text = _prefs.AutoStartGame
                ? "Ready — hit Launch to start the game."
                : "Ready — Launch maps Oak, then start the game yourself.";
        }
    }

    private void UpdateLaunchTooltips()
    {
        if (AutoStartGameBox is null || LaunchBtn is null) return;
        if (_prefs.AutoStartGame)
        {
            AutoStartGameBox.ToolTip =
                "Oak will start DayZ_BE (or CS2) after mapping the driver.";
            LaunchBtn.ToolTip =
                "Starts Steam if needed, maps Oak, then launches the game.";
        }
        else
        {
            AutoStartGameBox.ToolTip =
                "Oak maps the driver and opens DayZ Launcher only. Press Play yourself — Oak injects when DayZ_x64 starts.";
            LaunchBtn.ToolTip =
                "Maps Oak, opens DayZ Launcher, waits for you to press Play.";
        }
    }

    private void ClearDetail()
    {
        ProductTitle.Text = "Select a product";
        ProductDesc.Text = "";
        StatProductStatus.Text = StatVersion.Text = StatLicense.Text = StatTimeLeft.Text = StatGame.Text = "—";
        DetailPlan.Text = DetailExpires.Text = DetailHwid.Text = DetailBuild.Text =
            DetailUpdated.Text = DetailAccount.Text = DetailChangelog.Text = "";
        RedeemBox.Text = "";
        RedeemStatus.Text = "";
        NoticeBar.Visibility = Visibility.Collapsed;
        LaunchBtn.IsEnabled = false;
    }

    private void SetBusy(bool busy)
    {
        _busy = busy;
        LoginBtn.IsEnabled = !busy;
        RedeemBtn.IsEnabled = !busy;
        ResetHwidBtn.IsEnabled = !busy && _current?.Hwid?.Bound == true;
        LaunchBtn.IsEnabled = !busy && _current?.Launch?.CanInject == true;
        Cursor = busy ? Cursors.Wait : null;
    }

    private void Window_Closing(object sender, CancelEventArgs e)
    {
        _work?.Cancel();
        _tick.Stop();
        _poll.Stop();
        _api.Dispose();
    }

    private static string FormatSize(long bytes) =>
        bytes >= 1024 * 1024 ? $"{bytes / 1024d / 1024d:0.0} MB" : $"{bytes / 1024d:0} KB";

    private static string Short(string? hash) =>
        string.IsNullOrEmpty(hash) ? "—" : hash[..Math.Min(12, hash.Length)].ToLowerInvariant();

    private static string FormatStamp(string? iso) =>
        DateTimeOffset.TryParse(iso, out var t) ? t.LocalDateTime.ToString("yyyy-MM-dd HH:mm") : "—";

    private static Brush BrushForStatus(string? status) => status?.ToLowerInvariant() switch
    {
        "online" or "active" => OkBrush(),
        "maintenance" => WarnBrush(),
        _ => BadBrush()
    };

    private static Brush OkBrush() => (Brush)Application.Current.FindResource("Ok");
    private static Brush WarnBrush() => (Brush)Application.Current.FindResource("Warn");
    private static Brush BadBrush() => (Brush)Application.Current.FindResource("Bad");
    private static Brush MidBrush() => (Brush)Application.Current.FindResource("TxtMid");
}
