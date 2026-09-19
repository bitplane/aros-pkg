// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Text.Json;
using System.Text.RegularExpressions;
using Microsoft.Extensions.Options;
using Portal.Channels;
using Portal.Push;

namespace Portal.Accounts;

/// One registered publisher: a GitHub account that says "this signing key is
/// mine". The portal keeps the account's number and login, nothing else of it.
public sealed record Registered(
    long GitHubId, string Login, string Name, string Key, List<string> Channels,
    bool Files, bool Suspended, DateTime Since);

/// <summary>
/// The publishers who registered themselves, in state/publishers.json. A
/// signed push looks its key up here after Portal:SignedKeys. What a new
/// publisher gets is decided here and nowhere else: their own channels, by
/// link only, unlisted until a maintainer lists them.
/// </summary>
public sealed partial class Registry
{
    readonly PortalOptions o;
    readonly Catalogue catalogue;
    readonly object gate = new();
    List<Registered> all;
    string File1 => Path.Combine(o.StateDir, "publishers.json");
    static readonly JsonSerializerOptions Json = new() { WriteIndented = true };

    public const int MaxChannels = 3;

    public Registry(IOptions<PortalOptions> options, Catalogue catalogue)
    {
        o = options.Value;
        this.catalogue = catalogue;
        all = File.Exists(File1) ? JsonSerializer.Deserialize<List<Registered>>(File.ReadAllText(File1)) ?? [] : [];
    }

    public IReadOnlyList<Registered> All() { lock (gate) return all.ToList(); }
    public Registered? ByAccount(long id) { lock (gate) return all.FirstOrDefault(r => r.GitHubId == id); }
    public Registered? ByKey(string key) { lock (gate) return all.FirstOrDefault(r => string.Equals(r.Key, key, StringComparison.OrdinalIgnoreCase)); }

    /// The publisher a signed push with this key comes from; null when unknown or suspended.
    public Publisher? PublisherFor(string key) =>
        ByKey(key) is { Suspended: false } r ? new Publisher(r.Name, r.Channels.ToHashSet(), r.Files) : null;

    /// Names nobody takes by registering: the project's own, and what reads like it.
    public static bool ReservedName(string channel) =>
        channel is "pkg" or "aros" or "contrib" or "contrib-nightly" or "system" or "official" or "core" or "main" or "test" or "admin"
        || channel.StartsWith("aros-", StringComparison.Ordinal) || channel.StartsWith("pkg-", StringComparison.Ordinal)
        || channel.StartsWith("contrib-", StringComparison.Ordinal) || channel.StartsWith("afsplus", StringComparison.Ordinal);

    /// Registers, or updates, what an account declares. Null when done; else what is wrong, in words for the form.
    public string? Register(long id, string login, string name, string key, string channel)
    {
        name = name.Trim(); key = key.Trim().ToLowerInvariant(); channel = channel.Trim().ToLowerInvariant();
        if (name.Length is < 2 or > 60 || name.Any(char.IsControl) || name.Contains(':') || name.Contains(';'))
            return "The publisher name is 2 to 60 characters, without ':' or ';'.";
        if (!Hex64().IsMatch(key))
            return "The public key is the 64 hexadecimal digits that pkg KEYINFO FILE <keyfile> prints. Never paste the key file itself.";
        if (!ChannelPaths.IsChannelName(channel))
            return "A channel name is lowercase letters, digits and '-', and not a word the site uses.";
        if (ReservedName(channel))
            return $"The name {channel} is kept for the project's own channels; choose another.";
        lock (gate)
        {
            if (all.FirstOrDefault(r => string.Equals(r.Key, key, StringComparison.OrdinalIgnoreCase) && r.GitHubId != id) is not null)
                return "That key is already registered by another account.";
            if (o.SignedKeys.Contains(key, StringComparison.OrdinalIgnoreCase))
                return "That key belongs to the portal's maintainers.";
            if (all.Any(r => r.GitHubId != id && r.Channels.Contains(channel)))
                return $"The channel {channel} belongs to another publisher.";
            var mine = all.FirstOrDefault(r => r.GitHubId == id);
            if (mine is null && catalogue.Get(channel) is not null)
                return $"The channel {channel} exists already; ask the maintainer if it is yours.";
            if (mine is { Suspended: true })
                return "This account is suspended; ask the maintainer.";
            if (mine is not null && !mine.Channels.Contains(channel))
            {
                if (catalogue.Get(channel) is not null) return $"The channel {channel} exists already; ask the maintainer if it is yours.";
                if (mine.Channels.Count >= MaxChannels) return $"An account has {MaxChannels} channels at most; ask the maintainer for more.";
            }
            if (mine is not null && !string.Equals(mine.Key, key, StringComparison.OrdinalIgnoreCase)
                && mine.Channels.Any(c => catalogue.Get(c) is { Packages.Count: > 0 }))
                return "You have published with your registered key, and every package stays with the key of its first version: a key change goes through the maintainer.";
            var channels = mine?.Channels.ToList() ?? [];
            if (!channels.Contains(channel)) channels.Add(channel);
            var now = new Registered(id, login, name, key, channels, mine?.Files ?? false, false, mine?.Since ?? DateTime.UtcNow);
            all = all.Where(r => r.GitHubId != id).Append(now).ToList();
            // A new publisher's channel is served and shown nowhere until a maintainer lists it.
            if (mine is null || !mine.Channels.Contains(channel))
            {
                var mark = catalogue.UnlistedPath(channel);
                Directory.CreateDirectory(Path.GetDirectoryName(mark)!);
                if (!File.Exists(mark)) File.WriteAllText(mark, $"{DateTime.UtcNow:O} registered by {login}\n");
            }
            Save();
        }
        return null;
    }

    public bool Change(long id, Func<Registered, Registered> how)
    {
        lock (gate)
        {
            var r = all.FirstOrDefault(x => x.GitHubId == id);
            if (r is null) return false;
            all = all.Where(x => x.GitHubId != id).Append(how(r)).ToList();
            Save();
            return true;
        }
    }

    void Save()
    {
        Directory.CreateDirectory(o.StateDir);
        var tmp = File1 + ".tmp";
        File.WriteAllText(tmp, JsonSerializer.Serialize(all.OrderBy(r => r.Since).ToList(), Json));
        File.Move(tmp, File1, overwrite: true);
    }

    [GeneratedRegex(@"\A[0-9a-fA-F]{64}\z")] private static partial Regex Hex64();
}
