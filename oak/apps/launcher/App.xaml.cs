using System.IO;
using System.Text;
using System.Threading;
using System.Windows;
using System.Windows.Threading;

namespace OakLauncher;

public partial class App : Application
{
    private static Mutex? _instanceLock;

    protected override void OnStartup(StartupEventArgs e)
    {
        // Two launchers fighting over the same session file and injecting twice
        // is never what the user wants. Scoped to the session rather than
        // Global\, which needs a privilege a standard user does not have.
        try
        {
            _instanceLock = new Mutex(true, @"Local\OakLauncher.SingleInstance", out var isFirst);
            if (!isFirst)
            {
                MessageBox.Show("Oak Launcher is already running.", "Oak",
                    MessageBoxButton.OK, MessageBoxImage.Information);
                Shutdown();
                return;
            }
        }
        catch (Exception ex)
        {
            // Never let the guard itself stop the launcher from opening.
            LogCrash(ex);
        }

        DispatcherUnhandledException += OnDispatcherException;
        AppDomain.CurrentDomain.UnhandledException += (_, args) =>
            LogCrash(args.ExceptionObject as Exception);
        TaskScheduler.UnobservedTaskException += (_, args) =>
        {
            LogCrash(args.Exception);
            args.SetObserved();
        };

        base.OnStartup(e);
    }

    private void OnDispatcherException(object sender, DispatcherUnhandledExceptionEventArgs e)
    {
        LogCrash(e.Exception);
        MessageBox.Show(
            "Something went wrong:\n\n" + e.Exception.Message +
            "\n\nDetails were written to " + LogPath,
            "Oak Launcher", MessageBoxButton.OK, MessageBoxImage.Error);
        // Keep the window alive: a failed request should not close the launcher.
        e.Handled = true;
    }

    private static string LogPath => Path.Combine(LauncherConfig.DataDir, "launcher.log");

    private static void LogCrash(Exception? ex)
    {
        if (ex == null) return;
        try
        {
            var sb = new StringBuilder()
                .AppendLine($"--- {DateTime.Now:yyyy-MM-dd HH:mm:ss} ---")
                .AppendLine(ex.ToString());
            File.AppendAllText(LogPath, sb.ToString());
        }
        catch { /* logging must never throw */ }
    }

    protected override void OnExit(ExitEventArgs e)
    {
        _instanceLock?.Dispose();
        base.OnExit(e);
    }
}
