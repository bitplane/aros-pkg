// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Globalization;
using Portal.Channels;

namespace Portal;

/// Formatting shared by the pages.
public static class Site
{
    public static string ChannelUrl(HttpRequest req, PortalOptions o, string channel) =>
        (string.IsNullOrEmpty(o.PublicUrl) ? $"{req.Scheme}://{req.Host}" : o.PublicUrl.TrimEnd('/')) + "/" + channel;

    /// The command that installs a package, as shown everywhere. ROOT is the
    /// AROS system: SYS: on AROS, the system drawer from a host.
    public static string InstallCommand(string url, string name, string? version = null) =>
        $"Pkg INSTALL {name} ROOT SYS: CHANNEL {url}" + (version is null ? "" : $" VERSION {version}");

    /// <summary>
    /// Text with what a search matched marked in it. Everything is encoded,
    /// and only the words of the query end up inside a mark, so a package's
    /// own text can never carry markup into a page.
    /// </summary>
    public static Microsoft.AspNetCore.Html.IHtmlContent Mark(string? text, string query)
    {
        if (text is null) return new Microsoft.AspNetCore.Html.HtmlString("");
        var words = query.Split(' ', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
            .Where(w => w.Length > 1).Distinct(StringComparer.OrdinalIgnoreCase).ToList();
        if (words.Count == 0) return new Microsoft.AspNetCore.Html.HtmlString(System.Net.WebUtility.HtmlEncode(text));
        var sb = new System.Text.StringBuilder();
        for (int i = 0; i < text.Length; )
        {
            int at = -1, len = 0;
            foreach (var w in words)
            {
                var k = text.IndexOf(w, i, StringComparison.OrdinalIgnoreCase);
                if (k >= 0 && (at < 0 || k < at)) { at = k; len = w.Length; }
            }
            if (at < 0) { sb.Append(System.Net.WebUtility.HtmlEncode(text[i..])); break; }
            sb.Append(System.Net.WebUtility.HtmlEncode(text[i..at]))
              .Append("<mark>").Append(System.Net.WebUtility.HtmlEncode(text.Substring(at, len))).Append("</mark>");
            i = at + len;
        }
        return new Microsoft.AspNetCore.Html.HtmlString(sb.ToString());
    }

    public static string Size(long bytes) => bytes switch
    {
        < 1024 => $"{bytes} B",
        < 1024 * 1024 => string.Create(CultureInfo.InvariantCulture, $"{bytes / 1024.0:0.#} KB"),
        < 1024L * 1024 * 1024 => string.Create(CultureInfo.InvariantCulture, $"{bytes / 1024.0 / 1024:0.#} MB"),
        _ => string.Create(CultureInfo.InvariantCulture, $"{bytes / 1024.0 / 1024 / 1024:0.##} GB"),
    };

    public static string Date(DateTime utc) => utc.ToString("d MMM yyyy", CultureInfo.InvariantCulture);

    public static string Ago(DateTime utc)
    {
        var d = DateTime.UtcNow - utc;
        return d.TotalMinutes < 2 ? "just now"
            : d.TotalHours < 1 ? $"{(int)d.TotalMinutes} minutes ago"
            : d.TotalDays < 1 ? $"{(int)d.TotalHours} hour{((int)d.TotalHours == 1 ? "" : "s")} ago"
            : d.TotalDays < 30 ? $"{(int)d.TotalDays} day{((int)d.TotalDays == 1 ? "" : "s")} ago"
            : Date(utc);
    }

    /// A nightly build such as 20260918, as a date when it is one.
    public static string BuildLabel(string build) =>
        DateTime.TryParseExact(build, "yyyyMMdd", CultureInfo.InvariantCulture, DateTimeStyles.None, out var d)
            ? $"nightly {d.ToString("d MMM yyyy", CultureInfo.InvariantCulture)}" : $"build {build}";

    /// "generic" means the package holds no executable, not "any CPU".
    public static string ArchLabel(string arch) => arch == "generic" ? "any CPU" : arch;

    public static string KindLabel(string kind) => kind switch
    {
        "image" => "program (image)",
        "application" => "program",
        "device" => "device or handler",
        _ => kind,
    };

    /// Only http(s) links are rendered as links: a manifest is signed, not trusted to be harmless.
    public static string? SafeUrl(string? url) =>
        url is not null && Uri.TryCreate(url, UriKind.Absolute, out var u) && (u.Scheme == "https" || u.Scheme == "http")
            ? u.ToString() : null;

    /// The distribution terms of [PKG24], in words.
    public static string DistributionLabel(string terms) => terms switch
    {
        "open-source" => "open source",
        "freeware" => "freeware: free to use, source not included",
        "shareware" => "shareware: try it, then pay the author",
        "public-domain" => "public domain",
        "commercial" => "commercial",
        "demo" => "a demo version",
        _ => terms,
    };

    public static string Short(string? hex, int n = 16) => hex is null ? "" : hex.Length > n ? hex[..n] : hex;

    public static string ArchiveStatus(ChannelInfo ch, string archive) =>
        !ch.Archives.Contains(archive) && ch.Upstream.TryGetValue(archive, out var up)
            ? $"{Size(up.Size)}, downloaded from {up.Host} once; Pkg checks its size and SHA-256 against the signed manifest, then every file"
        : ch.ArchiveChecks.TryGetValue(archive, out var c)
            ? c.Status == "ok" ? $"checked {Ago(c.When)}: {c.Detail}" : $"check failed: {c.Detail}"
            : "check pending";
}
