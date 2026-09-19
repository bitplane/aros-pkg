// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

namespace Portal.Channels;

public sealed record SearchQuery(string Q = "", string Kind = "", string Arch = "", string Tag = "",
                                 string Category = "", string Channel = "", string Sort = "")
{
    public static SearchQuery From(IQueryCollection q) => new(
        q["q"].ToString().Trim(), q["kind"].ToString(), q["arch"].ToString(), q["tag"].ToString().ToLowerInvariant(),
        q["category"].ToString(), q["channel"].ToString(), q["sort"].ToString());
}

/// A package that answers a query, and why, when the reason is not its name.
public sealed record SearchHit(PackageInfo Package, int Rank, string? Why, long Downloads);

/// <summary>
/// One search for the page and for /api/search: names first, then what a
/// package says about itself and what it provides, then the files it holds.
/// </summary>
public sealed class Search(Catalogue catalogue, Downloads downloads)
{
    public long DownloadsOf(PackageInfo p) =>
        p.Versions.Select(v => v.Manifest.Payload).OfType<string>().Distinct()
            .Sum(d => downloads.Get($"payload/{p.Channel}/{d}"));

    public long DownloadsOf(VersionEntry v) =>
        v.Manifest.Payload is { } d ? downloads.Get($"payload/{v.Channel}/{d}") : 0;

    public List<SearchHit> Run(SearchQuery s)
    {
        var q = s.Q;
        bool Has(string? text) => text is not null && text.Contains(q, StringComparison.OrdinalIgnoreCase);
        (int, string?) Rank(PackageInfo p)
        {
            if (q.Length == 0) return (2, null);
            var m = p.Latest.Manifest;
            if (p.Name.Equals(q, StringComparison.OrdinalIgnoreCase)) return (0, null);
            if (m.Provides.FirstOrDefault(l => l.Equals(q, StringComparison.OrdinalIgnoreCase)) is { } exact) return (1, $"provides {exact}");
            if (p.Name.StartsWith(q, StringComparison.OrdinalIgnoreCase)) return (1, null);
            if (Has(p.Name)) return (2, null);
            if (m.Provides.FirstOrDefault(Has) is { } lib) return (3, $"provides {lib}");
            if (m.Tags.FirstOrDefault(Has) is { } tag) return (3, $"tagged {tag}");
            if (Has(m.Short) || Has(m.Category)) return (3, null);
            if (m.Description.Any(Has) || m.Authors.Any(Has)) return (4, "in its description");
            if (m.Files.FirstOrDefault(f => Has(f.Path)) is { } file) return (5, $"holds {file.Path}");
            return (9, null);
        }
        var hits = catalogue.Listed()
            .Where(c => s.Channel.Length == 0 || c.Name == s.Channel)
            .SelectMany(c => c.Packages.Values)
            .Select(p => { var (r, why) = Rank(p); return new SearchHit(p, r, why, DownloadsOf(p)); })
            .Where(h => h.Rank < 9
                && (s.Kind.Length == 0 || h.Package.Kind == s.Kind)
                && (s.Arch.Length == 0 || h.Package.Archs.Contains(s.Arch))
                && (s.Tag.Length == 0 || h.Package.Latest.Manifest.Tags.Contains(s.Tag))
                && (s.Category.Length == 0 || (h.Package.Latest.Manifest.Category ?? "").StartsWith(s.Category, StringComparison.OrdinalIgnoreCase)));
        return (s.Sort switch
        {
            "newest" => hits.OrderByDescending(h => h.Package.Updated),
            "downloads" => hits.OrderByDescending(h => h.Downloads).ThenBy(h => h.Rank),
            _ => hits.OrderBy(h => catalogue.IsPinned(h.Package) ? 0 : 1).ThenBy(h => h.Rank),
        }).ThenBy(h => h.Package.Name, StringComparer.OrdinalIgnoreCase).ToList();
    }
}
