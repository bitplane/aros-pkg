// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Collections.Concurrent;
using System.Globalization;
using System.Text.RegularExpressions;
using Microsoft.Extensions.Options;

namespace Portal.Channels;

/// <summary>
/// How Pkg spreads: for each UTC day, how many channel reads, package downloads
/// and pushes came from each Pkg version, system and CPU. Those three words are
/// all Pkg says about itself (User-Agent: "Pkg/1.7.0+20260920 (aros; aarch64)"),
/// and the three words are all the portal keeps. There is no record of a single
/// request, no address, no identifier and no count of people: a day's line says
/// only "this many, of this kind, from this build". A day holds at most a few
/// hundred different builds; past that, the rest of the day counts as other, so
/// a flood of invented names cannot grow this file.
/// Kept in memory and written to state/usage every minute and at exit.
/// </summary>
public sealed partial class Usage : BackgroundService
{
    public const string Reads = "reads", Downloads = "downloads", Pushes = "pushes";
    public const int MaxBuildsADay = 300;

    /// One day of one build: what it asked for, and how often.
    public sealed record Line(string Day, string Version, string System, string Cpu, string Kind, long Count);

    readonly string path;
    readonly ConcurrentDictionary<string, long> counts = new(StringComparer.Ordinal);

    public Usage(IOptions<PortalOptions> options)
    {
        path = Path.Combine(options.Value.StateDir, "usage");
        if (File.Exists(path))
            foreach (var l in File.ReadLines(path))
                if (l.Split('\t') is [var day, var v, var sys, var cpu, var kind, var n] && long.TryParse(n, out var c))
                    counts[Key(day, v, sys, cpu, kind)] = c;
    }

    static string Key(string day, string v, string sys, string cpu, string kind) => $"{day}\t{v}\t{sys}\t{cpu}\t{kind}";

    /// The three words Pkg says about itself, or "other" for anything that is
    /// not Pkg, and "before 1.5" for a Pkg from before it said its version.
    public static (string Version, string System, string Cpu) Who(string? userAgent)
    {
        var ua = (userAgent ?? "").Trim();
        if (!ua.StartsWith("Pkg", StringComparison.Ordinal)) return ("other", "other", "other");
        if (Agent().Match(ua) is { Success: true } m)
            return (m.Groups[1].Value, m.Groups[2].Value.ToLowerInvariant(), m.Groups[3].Value.ToLowerInvariant());
        if (ua.StartsWith("Pkg/", StringComparison.Ordinal))
            return (ua[4..].Split(' ')[0], "unknown", "unknown");   // a version, but not the rest
        return ("before 1.5", "unknown", "unknown");                // Pkg said only its name
    }

    public void Note(string? userAgent, string kind)
    {
        var (v, sys, cpu) = Who(userAgent);
        var day = DateTime.UtcNow.ToString("yyyy-MM-dd", CultureInfo.InvariantCulture);
        var key = Key(day, v, sys, cpu, kind);
        // A day's shelf is finite: once it is full, the rest of that day is other.
        if (!counts.ContainsKey(key) && Today(day) >= MaxBuildsADay) key = Key(day, "other", "other", "other", kind);
        counts.AddOrUpdate(key, 1, (_, n) => n + 1);
    }

    int Today(string day)
    {
        var prefix = day + "\t";
        return counts.Keys.Count(k => k.StartsWith(prefix, StringComparison.Ordinal));
    }

    public IReadOnlyList<Line> All() =>
        counts.Select(k => k.Key.Split('\t') is [var day, var v, var sys, var cpu, var kind]
                ? new Line(day, v, sys, cpu, kind, k.Value) : null)
            .OfType<Line>().OrderBy(l => l.Day, StringComparer.Ordinal).ToList();

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

    [GeneratedRegex(@"\APkg/([^\s(]{1,40}) \(([a-zA-Z]{1,16}); ([a-zA-Z0-9_]{1,16})\)")] private static partial Regex Agent();
}
