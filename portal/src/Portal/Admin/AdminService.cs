// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Globalization;
using Microsoft.Extensions.Options;
using Portal.Channels;
using Portal.Push;

namespace Portal.Admin;

/// <summary>
/// What only the site's maintainers do, through /_admin with an admin key:
/// take versions off a channel, and put them back. A removal moves the files
/// to state/removed/&lt;stash&gt;/, it destroys nothing, and every action is logged.
/// Publishers keep their own tools: signing and withdrawing stay theirs.
/// </summary>
public sealed class AdminService(IOptions<PortalOptions> options, Catalogue catalogue, Downloads downloads,
                                 ILogger<AdminService> log)
{
    readonly PortalOptions o = options.Value;
    string Live(string channel) => Path.Combine(o.ChannelsDir, channel);
    string StashRoot => Path.Combine(o.StateDir, "removed");
    string LogFile => Path.Combine(o.StateDir, "admin-log");

    // ---- remove ---------------------------------------------------------------

    public async Task<Record> Remove(string admin, string channel, string body, bool dryRun, CancellationToken ct)
    {
        var live = Live(channel);
        if (!File.Exists(Path.Combine(live, "index")))
            return Record.Refused(11, $"there is no channel {channel}", "check the channel name");
        var gate = PushService.LockFor(channel);
        await gate.WaitAsync(ct);
        try
        {
            var lines = IndexLine.ParseAll(await File.ReadAllTextAsync(Path.Combine(live, "index"), ct));
            var r = new Record();
            var chosen = new List<IndexLine>();
            int n = 0;
            foreach (var raw in body.Split('\n'))
            {
                n++;
                var f = raw.Trim().Split(' ', StringSplitOptions.RemoveEmptyEntries);
                if (f.Length == 0) continue;
                // An index line, "<name> <version> <arch>", "<name> <version>", or "<name> *".
                var hits = f.Length switch
                {
                    4 => lines.Where(l => l.ToString() == string.Join(' ', f)),
                    3 => lines.Where(l => l.Name == f[0] && l.Version == f[1] && l.Arch == f[2]),
                    2 when f[1] == "*" => lines.Where(l => l.Name == f[0]),
                    2 => lines.Where(l => l.Name == f[0] && l.Version == f[1]),
                    _ => [],
                };
                var found = hits.ToList();
                if (found.Count == 0) { r.Add("refused", $"line {n} 11 '{raw.Trim()}' names nothing in {channel}"); continue; }
                chosen.AddRange(found.Where(l => !chosen.Contains(l)));
            }
            if (chosen.Count == 0)
                return r.Add("result", "refused").Add("class", "not-found").Add("code", 11)
                        .Add("summary", "nothing to remove: no line names a published version").Add("next", "check the names with the channel's index");

            var remaining = lines.Where(l => !chosen.Contains(l)).ToList();
            var keep = Referenced(live, remaining);
            var files = new List<string>();
            foreach (var l in chosen)
            {
                foreach (var ext in new[] { "manifest", "sig", "withdrawn", "withdrawn.sig" })
                    files.Add($"objects/{l.Digest}.{ext}");
                if (PayloadOf(live, l) is { } p) files.Add($"objects/{p}.pkg");
            }
            files = files.Distinct().Where(f => File.Exists(Path.Combine(live, f)) && !keep.Contains(f)).ToList();
            var stamp = DateTime.UtcNow.ToString("yyyyMMdd'T'HHmmss'Z'", CultureInfo.InvariantCulture);

            r.Add("result", dryRun ? "would-remove" : "removed");
            foreach (var l in chosen) r.Add(dryRun ? "would-remove" : "removed", $"{l.Name} {l.Version} {l.Arch}");
            r.Add("files", files.Count).Add("remaining", remaining.Count);
            if (dryRun)
                return r.Add("summary", $"would remove {chosen.Count} version{S(chosen.Count)} and {files.Count} file{S(files.Count)} from {channel}; nothing was changed");

            // Stash first, then the index: a reader never sees a line whose files are gone.
            var stash = Path.Combine(StashRoot, stamp, channel);
            Directory.CreateDirectory(Path.Combine(stash, "objects"));
            await File.WriteAllLinesAsync(Path.Combine(stash, "index-lines"), chosen.Select(l => l.ToString()), ct);
            var tmp = Path.Combine(live, $".index.{Guid.NewGuid():N}");
            await File.WriteAllTextAsync(tmp, string.Concat(remaining.Select(l => l + "\n")), ct);
            File.Move(tmp, Path.Combine(live, "index"), overwrite: true);
            foreach (var f in files) File.Move(Path.Combine(live, f), Path.Combine(stash, f));
            Portal.Channels.Withdrawals.Write(live);

            // The portal's own records of those versions go with them.
            var seenPath = catalogue.FirstSeenPath(channel);
            if (File.Exists(seenPath))
            {
                var digests = chosen.Select(l => l.Digest).ToHashSet();
                var all = await File.ReadAllLinesAsync(seenPath, ct);
                await File.WriteAllLinesAsync(Path.Combine(stash, "first-seen"), all.Where(x => digests.Contains(x.Split('\t')[0])), ct);
                await File.WriteAllLinesAsync(seenPath, all.Where(x => !digests.Contains(x.Split('\t')[0])), ct);
            }
            var counts = files.Where(f => f.EndsWith(".pkg", StringComparison.Ordinal))
                .Select(f => $"payload/{channel}/{f[8..72]}").Select(k => (k, n: downloads.Take(k))).Where(x => x.n > 0).ToList();
            await File.WriteAllLinesAsync(Path.Combine(stash, "downloads"), counts.Select(x => $"{x.k}\t{x.n}"), ct);
            catalogue.Invalidate(channel);
            await Log(admin, "remove", channel, stamp, $"{chosen.Count} versions, {files.Count} files", ct);
            log.LogInformation("admin {Admin} removed {Count} versions from {Channel}, stash {Stamp}", admin, chosen.Count, channel, stamp);
            return r.Add("stash", stamp)
                .Add("summary", $"removed {chosen.Count} version{S(chosen.Count)} from {channel}; their {files.Count} file{S(files.Count)} are kept in stash {stamp}, and restore {stamp} puts them back")
                .Add("next", $"POST /_admin/restore/{stamp} undoes it");
        }
        finally { gate.Release(); }
    }

    // ---- restore --------------------------------------------------------------

    public async Task<Record> Restore(string admin, string stamp, CancellationToken ct)
    {
        var root = Path.Combine(StashRoot, stamp);
        if (stamp.Contains('/') || stamp.Contains("..") || !Directory.Exists(root))
            return Record.Refused(11, $"there is no stash {stamp}", "GET /_admin/log lists them");
        var r = new Record().Add("result", "restored");
        int versions = 0, files = 0;
        foreach (var stash in Directory.EnumerateDirectories(root))
        {
            var channel = Path.GetFileName(stash);
            var live = Live(channel);
            var gate = PushService.LockFor(channel);
            await gate.WaitAsync(ct);
            try
            {
                Directory.CreateDirectory(Path.Combine(live, "objects"));
                // Files first, then the lines, as a push does.
                foreach (var f in Directory.EnumerateFiles(Path.Combine(stash, "objects")))
                {
                    var dst = Path.Combine(live, "objects", Path.GetFileName(f));
                    if (!File.Exists(dst)) { File.Move(f, dst); files++; }
                }
                var index = Path.Combine(live, "index");
                var have = File.Exists(index) ? IndexLine.ParseAll(await File.ReadAllTextAsync(index, ct)) : [];
                var back = (await File.ReadAllLinesAsync(Path.Combine(stash, "index-lines"), ct))
                    .Select(IndexLine.Parse).OfType<IndexLine>().Where(l => !have.Contains(l)).ToList();
                var tmp = Path.Combine(live, $".index.{Guid.NewGuid():N}");
                await File.WriteAllTextAsync(tmp, string.Concat(have.Concat(back).Select(l => l + "\n")), ct);
                File.Move(tmp, index, overwrite: true);
                Portal.Channels.Withdrawals.Write(live);
                if (File.Exists(Path.Combine(stash, "first-seen")))
                {
                    var seen = catalogue.FirstSeenPath(channel);
                    Directory.CreateDirectory(Path.GetDirectoryName(seen)!);
                    await File.AppendAllLinesAsync(seen, await File.ReadAllLinesAsync(Path.Combine(stash, "first-seen"), ct), ct);
                }
                if (File.Exists(Path.Combine(stash, "downloads")))
                    foreach (var l in await File.ReadAllLinesAsync(Path.Combine(stash, "downloads"), ct))
                        if (l.Split('\t') is [var k, var v] && long.TryParse(v, out var c)) downloads.Put(k, c);
                catalogue.Invalidate(channel);
                foreach (var l in back) r.Add("restored", $"{channel}: {l.Name} {l.Version} {l.Arch}");
                versions += back.Count;
            }
            finally { gate.Release(); }
        }
        Directory.Move(root, root + ".restored");
        await Log(admin, "restore", "-", stamp, $"{versions} versions, {files} files", ct);
        return r.Add("summary", $"put back {versions} version{S(versions)} and {files} file{S(files)} from stash {stamp}");
    }

    // ---- listed or not --------------------------------------------------------

    public async Task<Record> SetListed(string admin, string channel, bool listed, CancellationToken ct)
    {
        if (!File.Exists(Path.Combine(Live(channel), "index")))
            return Record.Refused(11, $"there is no channel {channel}", "check the channel name");
        var mark = catalogue.UnlistedPath(channel);
        if (listed)
        {
            File.Delete(mark);
            if (catalogue.IsUnlisted(channel))
                return Record.Refused(15, $"{channel} is unlisted by the Portal:Unlisted setting, which this API does not change",
                                      "take it out of the setting and restart the portal");
        }
        else
        {
            Directory.CreateDirectory(Path.GetDirectoryName(mark)!);
            await File.WriteAllTextAsync(mark, $"{DateTime.UtcNow:O} {admin}\n", ct);
        }
        await Log(admin, listed ? "list" : "unlist", channel, "-", "", ct);
        return new Record().Add("result", listed ? "listed" : "unlisted").Add("channel", channel)
            .Add("summary", listed
                ? $"{channel} is shown on the site again"
                : $"{channel} is served to whoever has its address and shown nowhere: not on the home page, in search, statistics, feeds or publishers");
    }

    // ---- log ------------------------------------------------------------------

    public Record ReadLog()
    {
        var r = new Record().Add("result", "shown");
        var lines = File.Exists(LogFile) ? File.ReadAllLines(LogFile) : [];
        foreach (var l in lines) r.Add("action", l.Replace('\t', ' '));
        return r.Add("summary", lines.Length == 0 ? "no admin action yet" : $"{lines.Length} admin action{S(lines.Length)}");
    }

    async Task Log(string admin, string action, string channel, string stamp, string what, CancellationToken ct)
    {
        Directory.CreateDirectory(o.StateDir);
        await File.AppendAllTextAsync(LogFile, $"{DateTime.UtcNow:O}\t{admin}\t{action}\t{channel}\t{stamp}\t{what}\n", ct);
    }

    // ---- helpers --------------------------------------------------------------

    /// Every file the remaining lines still need, so a shared payload stays.
    static HashSet<string> Referenced(string live, List<IndexLine> remaining)
    {
        var keep = new HashSet<string>(StringComparer.Ordinal);
        foreach (var l in remaining)
        {
            foreach (var ext in new[] { "manifest", "sig", "withdrawn", "withdrawn.sig" }) keep.Add($"objects/{l.Digest}.{ext}");
            if (PayloadOf(live, l) is { } p) keep.Add($"objects/{p}.pkg");
        }
        return keep;
    }

    static string? PayloadOf(string live, IndexLine l)
    {
        var mp = Path.Combine(live, "objects", l.Digest + ".manifest");
        return File.Exists(mp) ? Manifest.Load(mp).Payload : null;
    }

    static string S(int n) => n == 1 ? "" : "s";
}
