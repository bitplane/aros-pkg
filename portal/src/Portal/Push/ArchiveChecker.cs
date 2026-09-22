// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Threading.Channels;
using Microsoft.Extensions.Options;
using Portal.Channels;

namespace Portal.Push;

/// <summary>
/// Checks each published source archive once, in the background: Pkg reads
/// it in one pass and checks every File line of every manifest that names it.
/// This is a publishing-quality gate, not a trust gate: the client checks
/// every file it extracts against the signed manifest anyway. Until the
/// check has run, the catalogue says so.
/// </summary>
public sealed class ArchiveChecker(IOptions<PortalOptions> options, PkgRunner pkg, Catalogue catalogue,
                                   ArchiveStore store, ILogger<ArchiveChecker> log) : BackgroundService
{
    readonly PortalOptions o = options.Value;
    readonly Channel<(string Channel, string Archive)> queue = System.Threading.Channels.Channel.CreateUnbounded<(string, string)>();

    public void Enqueue(string channel, string archive) => queue.Writer.TryWrite((channel, archive));

    protected override async Task ExecuteAsync(CancellationToken stop)
    {
        try
        {
            await Check(stop);
        }
        // The portal is stopping: the wait for the next archive ends this way,
        // and a background service that throws would take the site down with it.
        catch (OperationCanceledException) when (stop.IsCancellationRequested) { }
    }

    async Task Check(CancellationToken stop)
    {
        // Archives published before a restart and never checked.
        // Only archives never checked. Nothing is uploaded at startup: an
        // archive goes to R2 only right after this process has checked it.
        foreach (var ch in catalogue.Channels())
            foreach (var a in ch.Archives)
                if (!ch.ArchiveChecks.ContainsKey(a)) Enqueue(ch.Name, a);

        await foreach (var (channel, archive) in queue.Reader.ReadAllAsync(stop))
        {
            var live = Path.Combine(o.ChannelsDir, channel);
            if (!File.Exists(Path.Combine(live, "archives", archive))) continue;
            var started = DateTime.UtcNow;
            string status, detail;
            try
            {
                var a = await pkg.Run(["SHOW", "CHANNEL", live, "ARCHIVE", archive, "MACHINE"], TimeSpan.FromHours(2), stop);
                var problems = a.All("problem").ToList();
                var count = a.One("count") ?? "0";
                if (a.TimedOut) { status = "failed"; detail = "the check took more than two hours"; }
                else if (a.Exit == 0 && problems.Count == 0)
                {
                    status = "ok";
                    detail = $"{count} package{(count == "1" ? "" : "s")} checked in {(DateTime.UtcNow - started).TotalSeconds:0} s";
                }
                else
                {
                    status = "failed";
                    detail = problems.Count > 0 ? string.Join(" | ", problems.Take(5)) : a.One("reason") ?? $"Pkg exited with {a.Exit}";
                }
            }
            catch (Exception e) when (e is not OperationCanceledException)
            {
                status = "failed"; detail = e.Message;
            }
            var path = catalogue.ArchiveChecksPath(channel);
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            await File.AppendAllTextAsync(path, $"{archive} {status} {DateTime.UtcNow:O} {detail.Replace('\n', ' ')}\n", stop);
            log.LogInformation("archive check {Channel}/{Archive}: {Status}, {Detail}", channel, archive, status, detail);
            catalogue.Invalidate(channel);
            if (status == "ok") await OffloadIfConfigured(channel, archive, stop);
        }
    }

    async Task OffloadIfConfigured(string channel, string archive, CancellationToken stop)
    {
        if (!store.Enabled) return;
        string? why;
        try { why = await store.Offload(channel, archive, stop); }
        catch (Exception e) when (e is not OperationCanceledException) { why = e.Message; }
        if (why is not null)
            log.LogWarning("archive {Channel}/{Archive} stays on the disk: {Why}", channel, archive, why);
}
}
