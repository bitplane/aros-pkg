// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using Microsoft.Extensions.Options;

namespace Portal.Channels;

public sealed record PublisherInfo(string Key, string? Name, DateTime Since, List<VersionEntry> Versions)
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

    public IReadOnlyDictionary<string, string> Names()
    {
        var d = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        if (File.Exists(SignersFile))
            foreach (var l in File.ReadLines(SignersFile))
                if (l.Split('\t') is [var key, var name, ..] && !d.ContainsKey(key)) d[key] = name;
        foreach (var e in o.SignerNames.Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
            if (e.Split('=', 2) is [var key, var name]) d[key.Trim()] = name.Trim();
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
        return catalogue.Channels().SelectMany(c => c.Packages.Values).SelectMany(p => p.Versions)
            .Where(v => v.Signer is not null).GroupBy(v => v.Signer!, StringComparer.OrdinalIgnoreCase)
            .Select(g => new PublisherInfo(g.Key, names.GetValueOrDefault(g.Key), g.Min(v => v.Published), g.ToList()))
            .OrderByDescending(p => p.Versions.Count).ToList();
    }

    /// By the full key, its first 16 digits, or its name.
    public PublisherInfo? Find(string id) =>
        All().FirstOrDefault(p => p.Key.Equals(id, StringComparison.OrdinalIgnoreCase)
            || (id.Length >= 8 && p.Key.StartsWith(id, StringComparison.OrdinalIgnoreCase))
            || string.Equals(p.Name, id, StringComparison.OrdinalIgnoreCase));
}
