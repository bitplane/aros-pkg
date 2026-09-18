// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Diagnostics;
using Microsoft.Extensions.Options;

namespace Portal.Push;

/// What Pkg said, line by line, in its machine record format.
public sealed record PkgAnswer(int Exit, IReadOnlyList<(string Key, string Value)> Lines, bool TimedOut)
{
    public IEnumerable<string> All(string key) => Lines.Where(l => l.Key == key).Select(l => l.Value);
    public string? One(string key) => Lines.LastOrDefault(l => l.Key == key).Value;
}

/// <summary>
/// Runs Pkg itself to check a channel, so that the trust rules (digests,
/// signatures, withdrawals, several signers) exist in one implementation.
/// </summary>
public sealed class PkgRunner(IOptions<PortalOptions> options, ILogger<PkgRunner> log)
{
    readonly PortalOptions o = options.Value;

    public async Task<PkgAnswer> Run(IEnumerable<string> args, TimeSpan timeout, CancellationToken ct = default)
    {
        var psi = new ProcessStartInfo(o.PkgPath)
        {
            RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false,
        };
        foreach (var a in args) psi.ArgumentList.Add(a);
        psi.Environment["PKG_OUTPUT"] = "machine";
        using var p = Process.Start(psi) ?? throw new InvalidOperationException($"cannot start {o.PkgPath}");
        var stdout = p.StandardOutput.ReadToEndAsync(ct);
        var stderr = p.StandardError.ReadToEndAsync(ct);
        using var cts = CancellationTokenSource.CreateLinkedTokenSource(ct);
        cts.CancelAfter(timeout);
        bool timedOut = false;
        try { await p.WaitForExitAsync(cts.Token); }
        catch (OperationCanceledException)
        {
            timedOut = true;
            try { p.Kill(entireProcessTree: true); } catch (InvalidOperationException) { }
        }
        var text = (await stdout) + (await stderr);
        var lines = text.Split('\n')
            .Select(l => l.TrimEnd('\r'))
            .Where(l => l.Contains(": "))
            .Select(l => (l[..l.IndexOf(": ", StringComparison.Ordinal)], l[(l.IndexOf(": ", StringComparison.Ordinal) + 2)..]))
            .ToList();
        log.LogInformation("pkg {Args}: exit {Exit}{TimedOut}", string.Join(' ', psi.ArgumentList),
            timedOut ? -1 : p.ExitCode, timedOut ? " (timed out)" : "");
        return new PkgAnswer(timedOut ? -1 : p.ExitCode, lines, timedOut);
    }
}
