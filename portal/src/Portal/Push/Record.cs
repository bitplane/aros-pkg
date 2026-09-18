// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Globalization;
using System.Text;

namespace Portal.Push;

/// <summary>
/// An answer in Pkg's own record format, one "key: value" per line, so that
/// the client relays it untouched. Every answer closes with a summary a
/// person understands without the counts.
/// </summary>
public sealed class Record
{
    readonly List<(string Key, string Value)> lines = [];

    public Record Add(string key, object value)
    {
        // A record is one line: never let a value carry a line break.
        var v = (value.ToString() ?? "").Replace('\r', ' ').Replace('\n', ' ');
        lines.Add((key, v));
        return this;
    }

    public string? Get(string key) => lines.LastOrDefault(l => l.Key == key).Value;

    public override string ToString()
    {
        var sb = new StringBuilder();
        foreach (var (k, v) in lines) sb.Append(k).Append(": ").Append(v).Append('\n');
        return sb.ToString();
    }

    /// A refusal of the whole request, with Pkg's class numbers:
    /// 11 not found, 12 integrity, 13 signature, 14 key, 15 conflict, 20 usage.
    public static Record Refused(int cls, string reason, string next) =>
        new Record().Add("result", "refused").Add("class", ClassName(cls)).Add("code", cls)
            .Add("reason", reason).Add("summary", reason).Add("next", next);

    public static string ClassName(int cls) => cls switch
    {
        11 => "not-found", 12 => "integrity", 13 => "signature", 14 => "key",
        15 => "conflict", 17 => "io", 20 => "usage", _ => "error",
    };

    public static string Size(long bytes) => bytes switch
    {
        < 1024 => $"{bytes} bytes",
        < 1024 * 1024 => string.Create(CultureInfo.InvariantCulture, $"{bytes / 1024.0:0.#} KB"),
        < 1024L * 1024 * 1024 => string.Create(CultureInfo.InvariantCulture, $"{bytes / 1024.0 / 1024:0.#} MB"),
        _ => string.Create(CultureInfo.InvariantCulture, $"{bytes / 1024.0 / 1024 / 1024:0.##} GB"),
    };
}

public static class Results2
{
    public static IResult Text(Record r, int status = 200) =>
        Microsoft.AspNetCore.Http.Results.Text(r.ToString(), "text/plain; charset=utf-8", Encoding.UTF8, status);
}
