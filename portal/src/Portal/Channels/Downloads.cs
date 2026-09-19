// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Collections.Concurrent;
using Microsoft.Extensions.Options;

namespace Portal.Channels;

/// <summary>
/// Downloads the portal can count honestly: signed payloads (.pkg) and the
/// ways of getting Pkg. Archives come from their makers' servers, and every
/// client reads manifests to look at a channel, so neither is counted.
/// Kept in memory and written to state/downloads every minute and at exit.
/// </summary>
public sealed class Downloads : BackgroundService
{
    readonly string path;
    readonly ConcurrentDictionary<string, long> counts = new(StringComparer.Ordinal);

    public Downloads(IOptions<PortalOptions> options)
    {
        path = Path.Combine(options.Value.StateDir, "downloads");
        if (File.Exists(path))
            foreach (var l in File.ReadLines(path))
                if (l.Split('\t') is [var k, var v] && long.TryParse(v, out var n)) counts[k] = n;
    }

    /// Keys: "payload/<channel>/<sha256>", "get/<channel>/<what>".
    public void Count(string key) => counts.AddOrUpdate(key, 1, (_, n) => n + 1);

    public long Get(string key) => counts.GetValueOrDefault(key);

    /// Takes a count away (a removed version); its value, or 0.
    public long Take(string key) => counts.TryRemove(key, out var n) ? n : 0;

    /// Puts a count back (a restored version).
    public void Put(string key, long n) { if (n > 0) counts.AddOrUpdate(key, n, (_, m) => m + n); }

    public IEnumerable<KeyValuePair<string, long>> All => counts;

    protected override async Task ExecuteAsync(CancellationToken stop)
    {
        try
        {
            while (!stop.IsCancellationRequested)
            {
                await Task.Delay(TimeSpan.FromMinutes(1), stop);
                Save();
            }
        }
        catch (OperationCanceledException) { }
        Save();
    }

    void Save()
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var tmp = path + ".tmp";
        File.WriteAllLines(tmp, counts.OrderBy(k => k.Key, StringComparer.Ordinal).Select(k => $"{k.Key}\t{k.Value}"));
        File.Move(tmp, path, overwrite: true);
    }
}
