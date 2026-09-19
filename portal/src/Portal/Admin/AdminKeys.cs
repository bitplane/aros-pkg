// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Security.Cryptography;
using System.Text;
using Microsoft.Extensions.Options;

namespace Portal.Admin;

/// <summary>
/// The maintainers' keys, from Portal:AdminKeys ("name:sha256;..."). Only the
/// hash is deployed; the key stays with its maintainer. Separate from push
/// keys on purpose: neither can do the other's work.
/// </summary>
public sealed class AdminKeys(IOptions<PortalOptions> options)
{
    readonly List<(byte[] Hash, string Name)> keys = options.Value.AdminKeys
        .Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
        .Select(e => e.Split(':'))
        .Where(f => f.Length == 2 && f[1].Length == 64)
        .Select(f => (Convert.FromHexString(f[1]), f[0])).ToList();

    public string? Find(string? authorization)
    {
        if (authorization is null || !authorization.StartsWith("Bearer ", StringComparison.Ordinal)) return null;
        var given = SHA256.HashData(Encoding.UTF8.GetBytes(authorization["Bearer ".Length..].Trim()));
        string? found = null;
        foreach (var (hash, name) in keys)
            if (CryptographicOperations.FixedTimeEquals(hash, given)) found = name;
        return found;
    }

    public static (string Key, string Config) Create(string name)
    {
        var key = "pkga_" + Convert.ToHexString(RandomNumberGenerator.GetBytes(24)).ToLowerInvariant();
        var hash = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(key))).ToLowerInvariant();
        return (key, $"{name}:{hash}");
    }
}
