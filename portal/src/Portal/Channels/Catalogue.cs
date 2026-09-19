// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Collections.Concurrent;
using Microsoft.Extensions.Options;

namespace Portal.Channels;

/// One line of a channel index: name version arch digest.
public sealed record IndexLine(string Name, string Version, string Arch, string Digest)
{
    public override string ToString() => $"{Name} {Version} {Arch} {Digest}";

    public static IndexLine? Parse(string line)
    {
        var p = line.Trim().Split(' ', StringSplitOptions.RemoveEmptyEntries);
        return p.Length == 4 && p[3].Length == 64 ? new IndexLine(p[0], p[1], p[2], p[3]) : null;
    }

    public static List<IndexLine> ParseAll(string text) =>
        text.Split('\n').Select(Parse).OfType<IndexLine>().ToList();
}

public sealed class VersionEntry
{
    public required IndexLine Line { get; init; }
    public required string Channel { get; init; }
    public required Manifest Manifest { get; init; }
    public string? Signer { get; init; }
    public bool Withdrawn { get; init; }
    public DateTime Published { get; init; }
    public long PayloadSize { get; init; }
    public string Name => Line.Name;
    public string Version => Line.Version;
    public string Arch => Line.Arch;
    public string? Build => PkgVersion.Build(Line.Version);
}

public sealed class PackageInfo
{
    public required string Channel { get; init; }
    public required string Name { get; init; }
    /// Highest version first, as Pkg orders them.
    public required List<VersionEntry> Versions { get; init; }
    public List<string> UsedBy { get; } = [];

    public VersionEntry Latest => Versions.FirstOrDefault(v => !v.Withdrawn) ?? Versions[0];
    public IEnumerable<string> Archs => Versions.Where(v => v.Version == Latest.Version).Select(v => v.Arch).Distinct();
    public string Kind => Latest.Manifest.Kind;
    public DateTime Updated => Versions.Max(v => v.Published);
}

/// What one nightly (a build after '+') brought to a channel.
public sealed record BuildStats(string Build, int Published, int NewPackages, int Unchanged, long InstalledBytes, int Files);

public sealed class ChannelInfo
{
    public required string Name { get; init; }
    public required List<IndexLine> Lines { get; init; }
    public required SortedDictionary<string, PackageInfo> Packages { get; init; }
    public required List<string> Archives { get; init; }
    public required IReadOnlyDictionary<string, ArchiveCheck> ArchiveChecks { get; init; }
    /// Archives the manifests name upstream, by name.
    public required IReadOnlyDictionary<string, UpstreamArchive> Upstream { get; init; }
    public DateTime Updated { get; init; }

    public IEnumerable<BuildStats> Builds()
    {
        var builds = Lines.Select(l => PkgVersion.Build(l.Version)).OfType<string>().Distinct()
            .OrderByDescending(b => b, StringComparer.Ordinal).ToList();
        foreach (var b in builds)
        {
            var mine = Packages.Values.SelectMany(p => p.Versions).Where(v => v.Build == b).ToList();
            var names = mine.Select(v => v.Name).ToHashSet();
            int fresh = names.Count(n => Packages[n].Versions.All(v => v.Build == b
                || string.CompareOrdinal(v.Build ?? "", b) > 0));
            // Packages already in the channel before this build that it did not
            // republish: their files were unchanged.
            int unchanged = Packages.Values.Count(p => !names.Contains(p.Name)
                && p.Versions.Any(v => string.CompareOrdinal(v.Build ?? "", b) < 0));
            yield return new BuildStats(b, names.Count, fresh, unchanged,
                mine.Sum(v => v.Manifest.InstalledSize), mine.Sum(v => v.Manifest.Files.Count));
        }
    }
}

public sealed record ArchiveCheck(string Archive, string Status, DateTime When, string Detail);

/// <summary>
/// The catalogue of every channel, read from the channel directories and
/// rebuilt when an index changes on disk. There is no database: the channel
/// files are the single source.
/// </summary>
public sealed class Catalogue(IOptions<PortalOptions> options)
{
    readonly PortalOptions o = options.Value;
    readonly ConcurrentDictionary<string, (DateTime Stamp, long Len, ChannelInfo Info)> cache = new();

    public IEnumerable<string> ChannelNames() =>
        Directory.Exists(o.ChannelsDir)
            ? Directory.EnumerateDirectories(o.ChannelsDir).Select(Path.GetFileName).OfType<string>()
                .Where(ChannelPaths.IsChannelName).Order(StringComparer.Ordinal)
            : [];

    public IEnumerable<ChannelInfo> Channels() => ChannelNames().Select(Get).OfType<ChannelInfo>();

    public ChannelInfo? Get(string channel)
    {
        if (!ChannelPaths.IsChannelName(channel)) return null;
        var dir = Path.Combine(o.ChannelsDir, channel);
        if (!Directory.Exists(dir)) return null;
        var index = new FileInfo(Path.Combine(dir, "index"));
        var checks = new FileInfo(ArchiveChecksPath(channel));
        var stamp = new[] { index.Exists ? index.LastWriteTimeUtc : DateTime.MinValue,
                            checks.Exists ? checks.LastWriteTimeUtc : DateTime.MinValue }.Max();
        long len = index.Exists ? index.Length : -1;
        if (cache.TryGetValue(channel, out var c) && c.Stamp == stamp && c.Len == len) return c.Info;
        var info = Build(channel, dir, index);
        cache[channel] = (stamp, len, info);
        return info;
    }

    public void Invalidate(string channel) => cache.TryRemove(channel, out _);

    /// The pinned packages that exist, in the order the setting gives.
    public List<PackageInfo> Pinned() =>
        o.Pinned.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
            .Select(e => e.Split('/', 2))
            .Select(p => p.Length == 2 ? Package(p[0], p[1]) : null)
            .OfType<PackageInfo>().ToList();

    public bool IsPinned(PackageInfo p) =>
        o.Pinned.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
            .Contains($"{p.Channel}/{p.Name}", StringComparer.OrdinalIgnoreCase);

    public PackageInfo? Package(string channel, string name) =>
        Get(channel)?.Packages.GetValueOrDefault(name);

    public string ArchiveChecksPath(string channel) => Path.Combine(o.StateDir, channel, "archive-checks");

    ChannelInfo Build(string channel, string dir, FileInfo index)
    {
        var lines = index.Exists ? IndexLine.ParseAll(File.ReadAllText(index.FullName)) : [];
        var firstSeen = ReadFirstSeen(channel);
        var entries = new List<VersionEntry>();
        foreach (var l in lines)
        {
            var mp = Path.Combine(dir, "objects", l.Digest + ".manifest");
            if (!File.Exists(mp)) continue;
            var m = Manifest.Load(mp);
            string? signer = null;
            var sp = Path.Combine(dir, "objects", l.Digest + ".sig");
            if (File.Exists(sp))
                signer = File.ReadLines(sp).FirstOrDefault(s => s.StartsWith("Signer: "))?["Signer: ".Length..].Trim();
            long payload = m.Payload is { } p && File.Exists(Path.Combine(dir, "objects", p + ".pkg"))
                ? new FileInfo(Path.Combine(dir, "objects", p + ".pkg")).Length : 0;
            entries.Add(new VersionEntry
            {
                Line = l, Channel = channel, Manifest = m, Signer = signer,
                // Shown as withdrawn; Pkg checked the withdrawal's signature
                // when the push that brought it was committed.
                Withdrawn = File.Exists(Path.Combine(dir, "objects", l.Digest + ".withdrawn")),
                // When the portal published it, recorded at commit; for versions older
                // than that record, when its manifest was written here.
                Published = firstSeen.TryGetValue(l.Digest, out var seen) ? seen : File.GetLastWriteTimeUtc(mp),
                PayloadSize = payload,
            });
        }
        var packages = new SortedDictionary<string, PackageInfo>(StringComparer.OrdinalIgnoreCase);
        foreach (var g in entries.GroupBy(e => e.Name))
            packages[g.Key] = new PackageInfo
            {
                Channel = channel, Name = g.Key,
                Versions = g.OrderByDescending(e => e.Version, PkgVersion.Order).ThenBy(e => e.Arch, StringComparer.Ordinal).ToList(),
            };
        foreach (var p in packages.Values)
            foreach (var d in p.Latest.Manifest.Depends)
                if (packages.TryGetValue(d.Name, out var dep) && !dep.UsedBy.Contains(p.Name))
                    dep.UsedBy.Add(p.Name);
        var archives = Directory.Exists(Path.Combine(dir, "archives"))
            // Every published archive has its .sha256, on the disk or in R2.
            ? Directory.EnumerateFiles(Path.Combine(dir, "archives"), "*.sha256").Select(Path.GetFileName).OfType<string>()
                .Select(f => f[..^".sha256".Length])
                .Where(f => ChannelPaths.Classify("archives/" + f) == ChannelPaths.Kind.Archive).Order().ToList()
            : [];
        return new ChannelInfo
        {
            Name = channel, Lines = lines, Packages = packages, Archives = archives,
            ArchiveChecks = ReadChecks(channel),
            Upstream = entries.Where(e => e.Manifest.Upstream is not null && e.Manifest.SourceArchive is not null)
                .GroupBy(e => e.Manifest.SourceArchive!).ToDictionary(g => g.Key, g => g.First().Manifest.Upstream!),
            Updated = entries.Count > 0 ? entries.Max(e => e.Published) : Directory.GetLastWriteTimeUtc(dir),
        };
    }

    public string FirstSeenPath(string channel) => Path.Combine(o.StateDir, channel, "first-seen");

    Dictionary<string, DateTime> ReadFirstSeen(string channel)
    {
        var d = new Dictionary<string, DateTime>(StringComparer.Ordinal);
        var p = FirstSeenPath(channel);
        if (File.Exists(p))
            foreach (var l in File.ReadLines(p))
                if (l.Split('\t') is [var digest, var when] && DateTime.TryParse(when, null, System.Globalization.DateTimeStyles.RoundtripKind, out var t))
                    d.TryAdd(digest, t);
        return d;
    }

    Dictionary<string, ArchiveCheck> ReadChecks(string channel)
    {
        var d = new Dictionary<string, ArchiveCheck>(StringComparer.Ordinal);
        var p = ArchiveChecksPath(channel);
        if (!File.Exists(p)) return d;
        // <archive> <status> <iso time> <detail...>, the last line per archive wins.
        foreach (var line in File.ReadLines(p))
        {
            var f = line.Split(' ', 4);
            if (f.Length >= 3 && DateTime.TryParse(f[2], null, System.Globalization.DateTimeStyles.RoundtripKind, out var when))
                d[f[0]] = new ArchiveCheck(f[0], f[1], when, f.Length > 3 ? f[3] : "");
        }
        return d;
    }
}
