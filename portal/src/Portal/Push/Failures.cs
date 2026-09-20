// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Globalization;

namespace Portal.Push;

/// <summary>
/// What went wrong when the portal could not answer, kept so a maintainer can
/// read it afterwards: the time, the method and path, and the exception. Never
/// an address, a key or anything about who asked. The last hundred, in
/// state/failures; /_admin/failures shows them.
/// </summary>
public static class Failures
{
    static readonly object Gate = new();

    public static void Note(string stateDir, string where, Exception? ex)
    {
        try
        {
            var line = string.Create(CultureInfo.InvariantCulture,
                $"{DateTime.UtcNow:O}\t{where}\t{ex?.GetType().Name ?? "unknown"}\t{One(ex?.Message)}");
            lock (Gate)
            {
                Directory.CreateDirectory(stateDir);
                var path = Path.Combine(stateDir, "failures");
                var lines = File.Exists(path) ? File.ReadAllLines(path).ToList() : [];
                lines.Add(line);
                if (lines.Count > 100) lines.RemoveRange(0, lines.Count - 100);
                File.WriteAllLines(path, lines);
            }
        }
        catch (IOException) { }          // a portal that cannot write this still answers
        catch (UnauthorizedAccessException) { }
    }

    public static Record Read(string stateDir)
    {
        var path = Path.Combine(stateDir, "failures");
        var lines = File.Exists(path) ? File.ReadAllLines(path) : [];
        var r = new Record().Add("result", "shown");
        foreach (var l in lines.Reverse()) r.Add("failure", l.Replace('\t', ' '));
        return r.Add("summary", lines.Length == 0
            ? "the portal has not failed to answer since this file was last cleared"
            : $"{lines.Length} failure{(lines.Length == 1 ? "" : "s")}, newest first");
    }

    static string One(string? message) =>
        (message ?? "").ReplaceLineEndings(" ") is { Length: > 300 } long_ ? long_[..300] + "…" : (message ?? "").ReplaceLineEndings(" ");
}
