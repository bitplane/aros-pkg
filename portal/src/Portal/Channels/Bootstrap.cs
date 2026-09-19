// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.IO.Compression;
using System.Security.Cryptography;

namespace Portal.Channels;

/// <summary>
/// Getting Pkg before one has Pkg. On AROS the route is a drawer laid out as
/// a channel, holding the bare program for one CPU, Install-Pkg, ReadMe and
/// the signed pkg package: "Execute &lt;drawer&gt;/Install-Pkg &lt;drawer&gt;" installs Pkg
/// through its own checks. A stock AROS has no UnZip, so the zip is for the
/// machine next to it; the files are also offered one by one.
/// </summary>
public static class Bootstrap
{
    public sealed record FileOffer(string Path, long Size, string Sha256);

    /// The AROS CPUs this channel has a bare Pkg for.
    public static List<string> ArosCpus(string channelDir) =>
        Directory.Exists(Path.Combine(channelDir, "Bootstrap"))
            ? Directory.EnumerateDirectories(Path.Combine(channelDir, "Bootstrap")).Select(Path.GetFileName).OfType<string>()
                // Host builds are named pkg, which a case-blind disk would take for Pkg.
                .Where(c => !ChannelPaths.HostBootstraps.ContainsKey(c)
                            && File.Exists(Path.Combine(channelDir, "Bootstrap", c, "Pkg"))).Order().ToList()
            : [];

    /// The files of the drawer for one CPU, relative to the channel.
    public static List<string> DrawerFiles(ChannelInfo ch, string channelDir, string cpu)
    {
        var files = new List<string> { $"Bootstrap/{cpu}/Pkg" };
        foreach (var f in new[] { "Install-Pkg", "ReadMe" })
            if (File.Exists(Path.Combine(channelDir, f))) files.Add(f);
        if (ch.Packages.GetValueOrDefault("pkg") is { } pkg)
            foreach (var v in pkg.Versions.Where(v => !v.Withdrawn && (v.Arch == cpu || v.Arch == "generic")).Take(1))
            {
                files.Add($"objects/{v.Line.Digest}.manifest");
                files.Add($"objects/{v.Line.Digest}.sig");
                if (v.Manifest.Payload is { } p) files.Add($"objects/{p}.pkg");
            }
        return files.Where(f => File.Exists(Path.Combine(channelDir, f))).ToList();
    }

    /// The index of that drawer: the pkg lines for this CPU only.
    public static string DrawerIndex(ChannelInfo ch, string cpu) =>
        string.Concat((ch.Packages.GetValueOrDefault("pkg")?.Versions ?? [])
            .Where(v => !v.Withdrawn && (v.Arch == cpu || v.Arch == "generic")).Take(1).Select(v => v.Line + "\n"));

    public static async Task WriteZip(Stream output, ChannelInfo ch, string channelDir, string cpu, CancellationToken ct)
    {
        // Built in memory (under a megabyte) and sent at once: ZipArchive writes
        // its directory synchronously, which a response stream refuses.
        var top = $"Pkg-{cpu}/";
        using var buffer = new MemoryStream();
        using (var zip = new ZipArchive(buffer, ZipArchiveMode.Create, leaveOpen: true))
            await Fill(zip, top, ch, channelDir, cpu, ct);
        buffer.Position = 0;
        await buffer.CopyToAsync(output, ct);
    }

    static async Task Fill(ZipArchive zip, string top, ChannelInfo ch, string channelDir, string cpu, CancellationToken ct)
    {
        await using (var w = new StreamWriter((await zip.CreateEntry(top + "index").OpenAsync(ct))))
            await w.WriteAsync(DrawerIndex(ch, cpu));
        foreach (var rel in DrawerFiles(ch, channelDir, cpu))
        {
            var e = zip.CreateEntry(top + rel, CompressionLevel.Optimal);
            e.LastWriteTime = File.GetLastWriteTimeUtc(Path.Combine(channelDir, rel));
            // Keep the execute bit for hosts that honour it.
            if (rel.EndsWith("/Pkg", StringComparison.Ordinal)) e.ExternalAttributes = 0x81ED << 16;
            await using var dst = await e.OpenAsync(ct);
            await using var src = File.OpenRead(Path.Combine(channelDir, rel));
            await src.CopyToAsync(dst, ct);
        }
    }

    public static FileOffer Offer(string channelDir, string rel)
    {
        var full = Path.Combine(channelDir, rel);
        using var f = File.OpenRead(full);
        return new FileOffer(rel, f.Length, Convert.ToHexString(SHA256.HashData(f)).ToLowerInvariant());
    }
}
