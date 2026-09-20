// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Text.RegularExpressions;

namespace Portal.Channels;

/// <summary>
/// Which relative paths a channel serves and which a push may write. A hosted
/// channel is the directory channel byte for byte, so anything outside this
/// list is never served, whatever lies on disk.
/// </summary>
public static partial class ChannelPaths
{
    /// Names that are pages of the portal, never channels.
    static readonly HashSet<string> Reserved = new(StringComparer.OrdinalIgnoreCase)
    {
        "api", "channels", "packages", "search", "about", "static", "css", "robots.txt",
        "favicon.ico", "error", "health", "_push", "_admin", "index", "get", "downloads", "statistics", "docs", "install", "install.sh", "install.ps1", "get-pkg", "privacy", "see", "account", "signin", "signin-github", "signout", "admin", "report", "trust", "publishers", "verify-manifest.py", "badge", "feed",
    };

    [GeneratedRegex("^[a-z0-9][a-z0-9-]{0,39}\\z")]
    private static partial Regex ChannelName();

    [GeneratedRegex("^objects/[0-9a-f]{64}\\.(manifest|sig|pkg|withdrawn|withdrawn\\.sig)\\z")]
    private static partial Regex ObjectPath();

    [GeneratedRegex("^archives/[A-Za-z0-9][A-Za-z0-9._+-]{0,199}\\z")]
    private static partial Regex ArchivePath();

    [GeneratedRegex("^Bootstrap/[a-z0-9_]{1,32}/Pkg\\z")]
    private static partial Regex BootstrapPath();

    /// Pkg for the machines people manage AROS from, as agreed with the Pkg
    /// side: exactly these five paths, by platform.
    public static readonly IReadOnlyDictionary<string, string> HostBootstraps = new Dictionary<string, string>
    {
        ["macos-arm64"] = "Bootstrap/macos-arm64/pkg",
        ["macos-x86_64"] = "Bootstrap/macos-x86_64/pkg",
        ["linux-x86_64"] = "Bootstrap/linux-x86_64/pkg",
        ["linux-arm64"] = "Bootstrap/linux-arm64/pkg",
        ["windows-x86_64"] = "Bootstrap/windows-x86_64/pkg.exe",
    };

    public static bool IsChannelName(string name) =>
        ChannelName().IsMatch(name) && !Reserved.Contains(name);

    public enum Kind { None, Index, Withdrawals, Object, Archive, ArchiveDigest, Mutable }

    /// What a relative path is; None for anything a channel does not hold.
    public static Kind Classify(string path)
    {
        if (path == "index") return Kind.Index;
        // The server writes it from the signed withdrawals it holds; a push never sends it.
        if (path == Withdrawals.File1) return Kind.Withdrawals;
        if (ObjectPath().IsMatch(path)) return Kind.Object;
        if (path is "Install-Pkg" or "ReadMe" or "Bootstrap/SHA256SUMS" or "Bootstrap/SHA256SUMS.sig"
            || BootstrapPath().IsMatch(path) || HostBootstraps.Values.Contains(path))
            return Kind.Mutable;
        if (ArchivePath().IsMatch(path))
        {
            // The publisher's cache of an archive is never served: installers
            // do not read it and nobody should trust it.
            if (path.EndsWith(".pkgidx", StringComparison.Ordinal)) return Kind.None;
            if (path.EndsWith(".sha256", StringComparison.Ordinal)) return Kind.ArchiveDigest;
            if (path.EndsWith(".url", StringComparison.Ordinal)) return Kind.None;
            return Kind.Archive;
        }
        return Kind.None;
    }

    /// Paths a push may send. The index arrives with the commit, and an
    /// archive's .sha256 is written by the server.
    public static bool Pushable(string path) =>
        Classify(path) is Kind.Object or Kind.Archive or Kind.Mutable;

    /// Immutable once published: content-addressed objects and archives.
    public static bool Immutable(string path) =>
        Classify(path) is Kind.Object or Kind.Archive;

    /// The digest a content-addressed name promises, or null when the name
    /// promises none (.sig and withdrawals are named after their manifest).
    public static string? PromisedDigest(string path)
    {
        if (Classify(path) != Kind.Object) return null;
        var file = path["objects/".Length..];
        return file.EndsWith(".manifest", StringComparison.Ordinal) || file.EndsWith(".pkg", StringComparison.Ordinal)
            ? file[..64]
            : null;
    }

    public static string ContentType(string path) => Classify(path) switch
    {
        Kind.Index or Kind.Withdrawals or Kind.ArchiveDigest => "text/plain; charset=utf-8",
        Kind.Object when path.EndsWith(".pkg", StringComparison.Ordinal) => "application/octet-stream",
        Kind.Object => "text/plain; charset=utf-8",
        Kind.Mutable when path is "ReadMe" or "Install-Pkg" => "text/plain; charset=iso-8859-1",
        Kind.Mutable when path.StartsWith("Bootstrap/SHA256SUMS", StringComparison.Ordinal) => "text/plain; charset=utf-8",
        Kind.Archive when path.EndsWith(".tar.bz2", StringComparison.Ordinal) => "application/x-bzip2",
        Kind.Archive when path.EndsWith(".tar", StringComparison.Ordinal) => "application/x-tar",
        _ => "application/octet-stream",
    };
}
