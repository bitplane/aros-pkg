// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using Microsoft.Extensions.Options;

namespace Portal.Channels;

public sealed record PublisherInfo(string Key, string? Name, DateTime Since, List<VersionEntry> Versions,
                                   string? Url = null, string? Contact = null)
{
    public string Short => Key.Length > 16 ? Key[..16] : Key;
    public IEnumerable<IGrouping<string, VersionEntry>> ByPackage =>
        Versions.GroupBy(v => $"{v.Channel}/{v.Name}").OrderBy(g => g.Key, StringComparer.Ordinal);
}

/// <summary>
/// Publishers, as their signing keys: the key is the identity. A name is a
/// label: set in Portal:SignerNames, or learnt when a push under a publisher's
/// upload key first published a version signed by that key (state/signers).
/// </summary>
public sealed class Publishers(Catalogue catalogue, IOptions<PortalOptions> options)
{
    readonly PortalOptions o = options.Value;
    string SignersFile => Path.Combine(o.StateDir, "signers");

    public sealed record Profile(string Name, string? Url, string? Contact);

    /// Profiles from Portal:Publishers, by key.
    public IReadOnlyDictionary<string, Profile> Profiles()
    {
        var d = new Dictionary<string, Profile>(StringComparer.OrdinalIgnoreCase);
        foreach (var e in o.Publishers.Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
        {
            var f = e.Split('|').Select(x => x.Trim()).ToArray();
            if (f.Length >= 2 && f[0].Length == 64 && f[1].Length > 0)
                d[f[0]] = new Profile(f[1], f.Length > 2 && f[2].Length > 0 ? f[2] : null, f.Length > 3 && f[3].Length > 0 ? f[3] : null);
        }
        return d;
    }

    /// The key a package must be signed with from now on: a maintainers'
    /// transfer, else the key of its first version (null for a new package).
    public string? OwnerOf(string channel, string name, string? firstSigner)
    {
        foreach (var e in o.Owners.Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
            if (e.Split('=', 2) is [var who, var key] && who.Trim().Equals($"{channel}/{name}", StringComparison.Ordinal) && key.Trim().Length == 64)
                return key.Trim().ToLowerInvariant();
        return firstSigner;
    }

    public IReadOnlyDictionary<string, string> Names()
    {
        var d = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        if (File.Exists(SignersFile))
            foreach (var l in File.ReadLines(SignersFile))
                if (l.Split('\t') is [var key, var name, ..] && !d.ContainsKey(key)) d[key] = name;
        foreach (var e in o.SignerNames.Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
            if (e.Split('=', 2) is [var key, var name]) d[key.Trim()] = name.Trim();
        foreach (var (key, p) in Profiles()) d[key] = p.Name;
        return d;
    }

    /// Called at commit: the first publisher seen with a key names it.
    public void Learn(string signer, string publisher)
    {
        if (string.IsNullOrEmpty(signer) || Names().ContainsKey(signer)) return;
        Directory.CreateDirectory(o.StateDir);
        File.AppendAllText(SignersFile, $"{signer}\t{publisher}\t{DateTime.UtcNow:O}\n");
    }

    public List<PublisherInfo> All()
    {
        var names = Names();
        var profiles = Profiles();
        return catalogue.Channels().SelectMany(c => c.Packages.Values).SelectMany(p => p.Versions)
            .Where(v => v.Signer is not null).GroupBy(v => v.Signer!, StringComparer.OrdinalIgnoreCase)
            .Select(g => new PublisherInfo(g.Key, names.GetValueOrDefault(g.Key), g.Min(v => v.Published), g.ToList(),
                profiles.GetValueOrDefault(g.Key)?.Url, profiles.GetValueOrDefault(g.Key)?.Contact))
            .OrderByDescending(p => p.Versions.Count).ToList();
    }

    /// By the full key, its first 16 digits, or its name.
    public PublisherInfo? Find(string id) =>
        All().FirstOrDefault(p => p.Key.Equals(id, StringComparison.OrdinalIgnoreCase)
            || (id.Length >= 8 && p.Key.StartsWith(id, StringComparison.OrdinalIgnoreCase))
            || string.Equals(p.Name, id, StringComparison.OrdinalIgnoreCase));
}
