using System.Text.Json;

namespace OakLauncher;

/// <summary>
/// Fire-and-forget launcher telemetry. Failures never affect launch/inject.
/// </summary>
public static class Telemetry
{
    public static void Track(
        ApiClient api,
        string eventName,
        Dictionary<string, object?>? meta = null)
    {
        if (api is null || string.IsNullOrWhiteSpace(eventName)) return;
        _ = Task.Run(async () =>
        {
            try
            {
                await api.TrackLauncherEventAsync(eventName, meta, CancellationToken.None);
            }
            catch
            {
                /* telemetry must never surface */
            }
        });
    }
}
