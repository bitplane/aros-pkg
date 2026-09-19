// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Collections.Concurrent;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using Microsoft.Extensions.Options;

namespace Portal.Push;

/// <summary>
/// A push with nothing secret on the wire, for machines without TLS (AROS).
/// The publisher is known by the public half of their signing key
/// (Portal:SignedKeys, name:publickey:channels[:files]); the portal holds no
/// secret of theirs at all. A client asks for a session by public key, then
/// signs each request: "pkg-push-1", the method, the path, the session, a
/// number that only grows, the SHA-256 of the body and the Content-Range. The
/// header states that SHA-256, so the signature is checked before a byte of
/// the body is read, and the body must then have it. So
/// a request cannot be altered (the body's hash), replayed (the number), moved
/// to another address (the path) or to another portal or day (the session,
/// random, held in memory, thirty minutes). Pkg itself checks the signature,
/// CHECKSIG, as it checks everything else here.
/// </summary>
public sealed partial class SignedPush
{
    readonly Dictionary<string, Publisher> byKey = new(StringComparer.OrdinalIgnoreCase);
    readonly ConcurrentDictionary<string, Session> sessions = new();
    readonly PkgRunner pkg;
    readonly PortalOptions o;
    static readonly TimeSpan Lifetime = TimeSpan.FromMinutes(30);

    sealed class Session(string key) { public readonly string Key = key; public long Seq; public DateTime Expires = DateTime.UtcNow + Lifetime; }

    readonly Portal.Accounts.Registry registry;

    public SignedPush(IOptions<PortalOptions> options, PkgRunner pkg, Portal.Accounts.Registry registry)
    {
        this.pkg = pkg;
        this.registry = registry;
        o = options.Value;
        foreach (var entry in o.SignedKeys.Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
        {
            var f = entry.Split(':');
            if (f.Length is not (3 or 4) || !Hex64().IsMatch(f[1])) continue;
            byKey[f[1]] = new Publisher(f[0], f[2].Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).ToHashSet(),
                                        f.Length == 4 && f[3] == "files");
        }
    }

    public int Count => byKey.Count;

    /// The maintainers' settings first, then the publishers who registered themselves.
    Publisher? Known(string key) => byKey.TryGetValue(key, out var who) ? who : registry.PublisherFor(key);

    /// "key: <public key>" in, "session: <hex>" out. Anyone may ask; a session
    /// is worth nothing without the key's signatures.
    public Record Open(string body, string ask)
    {
        var key = body.Split('\n').Select(l => l.Trim()).FirstOrDefault(l => l.StartsWith("key: ", StringComparison.Ordinal))?[5..].Trim() ?? "";
        if (Known(key) is null)
            return Record.Refused(14, "this portal does not know that signing key. " + ask, "send the maintainers your public key (pkg KEYINFO FILE <keyfile>), your publisher name and the channel");
        foreach (var (id, s) in sessions) if (s.Expires < DateTime.UtcNow) sessions.TryRemove(id, out _);
        var mine = sessions.Where(x => string.Equals(x.Value.Key, key, StringComparison.OrdinalIgnoreCase)).OrderBy(x => x.Value.Expires).ToList();
        foreach (var old in mine.Take(Math.Max(0, mine.Count - 7))) sessions.TryRemove(old.Key, out _);   // a public key is public: eight at most each
        if (sessions.Count > 1000) return Record.Refused(17, "too many open sessions", "try again in a few minutes");
        var session = Convert.ToHexString(RandomNumberGenerator.GetBytes(16)).ToLowerInvariant();
        sessions[session] = new Session(key);
        return new Record().Add("result", "session").Add("session", session);
    }

    public static bool IsSigned(string? authorization) =>
        authorization is not null && authorization.StartsWith("Pkg-Signature ", StringComparison.Ordinal);

    /// The publisher a signed request comes from, or the refusal. The body is
    /// read to a file to be hashed, and handed back for the route to read.
    public async Task<(Publisher? Who, Record? Refusal, Stream? Body)> Check(HttpContext http, string ask, CancellationToken ct)
    {
        var m = Header().Match(http.Request.Headers.Authorization.ToString());
        if (!m.Success) return (null, Record.Refused(14, "the Pkg-Signature header is not in the form key=,session=,seq=,sha256=,sig=", "use Pkg 1.4 or later"), null);
        var (key, id, sig) = (m.Groups[1].Value, m.Groups[2].Value, m.Groups[5].Value);
        if (!long.TryParse(m.Groups[3].Value, out var seq)) seq = -1;
        if (Known(key) is not { } who)
            return (null, Record.Refused(14, "this portal does not know that signing key. " + ask, "send the maintainers your public key (pkg KEYINFO FILE <keyfile>), your publisher name and the channel"), null);
        if (!sessions.TryGetValue(id, out var s) || s.Expires < DateTime.UtcNow || !string.Equals(s.Key, key, StringComparison.OrdinalIgnoreCase))
            return (null, Record.Refused(14, "the session is unknown or over", "push again: Pkg asks for a new one"), null);

        // The signature first, over the digest the header claims: nothing of the body
        // is read for a request that does not check. Then the body must have that digest.
        var claimed = m.Groups[4].Value.ToLowerInvariant();
        var range = http.Request.Headers.ContentRange.FirstOrDefault() ?? "-";
        var text = $"pkg-push-1\n{http.Request.Method}\n{http.Request.PathBase}{http.Request.Path}{http.Request.QueryString}\n{id}\n{seq}\n{claimed}\n{range}\n";
        if (!await Good(text, key, sig, ct))
            return (null, Record.Refused(13, "the request's signature does not check against that key", "push again; if it persists, the request was altered on the way"), null);
        // Only a request that checks moves the number: a forger cannot use up the session.
        lock (s)
        {
            if (seq <= s.Seq) return (null, Record.Refused(14, "this request was already sent once", "push again"), null);
            s.Seq = seq;
            s.Expires = DateTime.UtcNow + Lifetime;
        }
        if ((http.Request.ContentLength ?? 0) > o.MaxPartBytes)
            return (null, Record.Refused(12, $"a request of more than {o.MaxPartBytes} bytes", "send large files in parts"), null);
        var tmp = Path.Combine(o.StateDir, "signed-tmp");
        Directory.CreateDirectory(tmp);
        var file = new FileStream(Path.Combine(tmp, Guid.NewGuid().ToString("N")), FileMode.CreateNew, FileAccess.ReadWrite,
                                  FileShare.None, 1 << 16, FileOptions.DeleteOnClose | FileOptions.Asynchronous);
        try
        {
            using var sha = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
            var buf = new byte[1 << 16];
            long total = 0;
            int n;
            while ((n = await http.Request.Body.ReadAsync(buf, ct)) > 0)
            {
                if ((total += n) > o.MaxPartBytes) { await file.DisposeAsync(); return (null, Record.Refused(12, "the request is larger than it said", "send large files in parts"), null); }
                sha.AppendData(buf, 0, n);
                await file.WriteAsync(buf.AsMemory(0, n), ct);
            }
            if (Convert.ToHexString(sha.GetHashAndReset()).ToLowerInvariant() != claimed)
            {
                await file.DisposeAsync();
                return (null, Record.Refused(13, "the request's body does not check against its signature", "push again; if it persists, the request was altered on the way"), null);
            }
            file.Position = 0;
            return (who, null, file);
        }
        catch { await file.DisposeAsync(); throw; }
    }

    async Task<bool> Good(string text, string key, string sig, CancellationToken ct)
    {
        var dir = Path.Combine(o.StateDir, "signed-tmp", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        try
        {
            var msg = Path.Combine(dir, "request");
            await File.WriteAllTextAsync(msg, text, new UTF8Encoding(false), ct);
            await File.WriteAllTextAsync(msg + ".sig", $"Signer: {key.ToLowerInvariant()}\nSignature: {sig.ToLowerInvariant()}\n", ct);
            var a = await pkg.Run(["CHECKSIG", msg, "FILE", msg + ".sig", "KEY", key.ToLowerInvariant(), "MACHINE"], TimeSpan.FromSeconds(10), ct);
            return a.Exit == 0 && a.One("result") == "good";
        }
        finally { try { Directory.Delete(dir, true); } catch (IOException) { } }
    }

    [GeneratedRegex(@"\A[0-9a-fA-F]{64}\z")] private static partial Regex Hex64();
    [GeneratedRegex(@"\APkg-Signature key=([0-9a-fA-F]{64}),session=([0-9a-f]{32}),seq=([0-9]{1,18}),sha256=([0-9a-fA-F]{64}),sig=([0-9a-fA-F]{128})\z")] private static partial Regex Header();
}
