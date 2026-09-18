// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Security.Cryptography;
using System.Text;
using Microsoft.Extensions.Options;

namespace Portal.Push;

public sealed record Publisher(string Name, IReadOnlySet<string> Channels)
{
    public bool MayPush(string channel) => Channels.Contains("*") || Channels.Contains(channel);
}

/// <summary>
/// Push keys, one per publisher, each scoped to channels. Only the SHA-256 of
/// a key is configured; the key is shown to the publisher once, by the
/// 'key' command, and never stored.
/// </summary>
public sealed class PublisherKeys
{
    readonly List<(byte[] Hash, Publisher Who)> keys = [];

    public PublisherKeys(IOptions<PortalOptions> options)
    {
        foreach (var entry in options.Value.Keys.Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
        {
            var f = entry.Split(':');
            if (f.Length != 3 || f[1].Length != 64) continue;
            keys.Add((Convert.FromHexString(f[1]),
                new Publisher(f[0], f[2].Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).ToHashSet())));
        }
    }

    public int Count => keys.Count;

    public Publisher? Find(string? authorization)
    {
        if (authorization is null || !authorization.StartsWith("Bearer ", StringComparison.Ordinal)) return null;
        var given = SHA256.HashData(Encoding.UTF8.GetBytes(authorization["Bearer ".Length..].Trim()));
        Publisher? found = null;
        foreach (var (hash, who) in keys)
            if (CryptographicOperations.FixedTimeEquals(hash, given)) found = who;
        return found;
    }

    /// A new key and the line that configures it.
    public static (string Key, string Config) Create(string publisher, string channels)
    {
        var key = "pkgk_" + Convert.ToHexString(RandomNumberGenerator.GetBytes(24)).ToLowerInvariant();
        var hash = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(key))).ToLowerInvariant();
        return (key, $"{publisher}:{hash}:{channels}");
    }
}
