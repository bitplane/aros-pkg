// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using Microsoft.Extensions.Options;

namespace Portal.Channels;

/// <summary>
/// The channel's list of withdrawn versions, in one file beside the index.
///
/// A withdrawal lives in <c>objects/&lt;digest&gt;.withdrawn</c>, signed by the
/// key that signed the version. Without a list, a reader learns of one only by
/// asking for that file for every entry of the index: 208 requests where seven
/// answer. The list names the digests that have one, so a reader asks for the
/// signed file only where there is something to read.
///
/// The list is a hint, never authority: what it names is still the signed
/// <c>.withdrawn</c> file, checked as before. A list missing a digest hides a
/// withdrawal exactly as deleting the signed file would, which a copy of a
/// channel could always do; a list naming a digest that has no valid
/// withdrawal costs a request and changes nothing.
///
/// Every channel the portal serves has one, empty when nothing is withdrawn,
/// so a reader that finds it knows the channel answers this way. A channel
/// that has none (an older portal, a directory a publisher copied) is read as
/// before.
/// </summary>
public static class Withdrawals
{
    public const string File1 = "withdrawals";

    /// First line, so a later shape can be told apart from this one.
    public const string Header = "Format: pkg-withdrawals 1";

    /// The text for a channel directory: the header, then one digest per line,
    /// in order.
    public static string Text(string channelDir)
    {
        var objects = Path.Combine(channelDir, "objects");
        IEnumerable<string> digests = Directory.Exists(objects)
            ? Directory.EnumerateFiles(objects, "*.withdrawn")
                .Select(Path.GetFileNameWithoutExtension).OfType<string>()
                .Where(d => d.Length == 64 && d.All(Uri.IsHexDigit))
                .Select(d => d.ToLowerInvariant())
                .OrderBy(d => d, StringComparer.Ordinal)
            : [];
        return Header + "\n" + string.Concat(digests.Select(d => d + "\n"));
    }

    /// Writes it whole, in one move, so a reader never sees half a list. A
    /// channel that does not exist yet (a push that published nothing) gets
    /// none: there is nothing to serve it from.
    public static void Write(string channelDir)
    {
        if (!Directory.Exists(channelDir)) return;
        var text = Text(channelDir);
        var path = Path.Combine(channelDir, File1);
        if (System.IO.File.Exists(path) && System.IO.File.ReadAllText(path) == text) return;
        var tmp = Path.Combine(channelDir, $".withdrawals.{Guid.NewGuid():N}");
        System.IO.File.WriteAllText(tmp, text);
        System.IO.File.Move(tmp, path, overwrite: true);
    }
}

/// Writes the list for every channel once at start, so channels published
/// before the portal kept one answer the same way as those published after.
public sealed class WithdrawalsAtStart(IOptions<PortalOptions> options, ILogger<WithdrawalsAtStart> log) : IHostedService
{
    public Task StartAsync(CancellationToken ct)
    {
        var dir = options.Value.ChannelsDir;
        if (!Directory.Exists(dir)) return Task.CompletedTask;
        foreach (var channel in Directory.EnumerateDirectories(dir))
        {
            try { Withdrawals.Write(channel); }
            catch (Exception e) { log.LogWarning(e, "withdrawals list for {Channel}", Path.GetFileName(channel)); }
        }
        return Task.CompletedTask;
    }

    public Task StopAsync(CancellationToken ct) => Task.CompletedTask;
}
