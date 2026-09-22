// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Globalization;
using System.Security;
using System.Text;
using Microsoft.Extensions.Options;
using Portal.Channels;

namespace Portal.Api;

/// <summary>
/// What tools, agents and feed readers use: /api/search (JSON, the search
/// page's results), Atom feeds of new versions (/feed, /channels/&lt;name&gt;/feed)
/// and an SVG badge per package (/badge/&lt;channel&gt;/&lt;name&gt;.svg).
/// </summary>
public static class Endpoints
{
    public static void MapPortalApi(this WebApplication app)
    {
        var api = app.MapGroup("/api").RequireRateLimiting("api");
        api.MapGet("/search", (HttpContext http, Search search, IOptions<PortalOptions> o) =>
        {
            var s = SearchQuery.From(http.Request.Query);
            var take = int.TryParse(http.Request.Query["take"], out var t) ? Math.Clamp(t, 1, 500) : 100;
            var hits = search.Run(s);
            var site = Site(http, o.Value);
            return Results.Json(new
            {
                query = s,
                total = hits.Count,
                results = hits.Take(take).Select(h => Describe(h.Package, site, search, h.Why)),
            });
        });

        // This instance's rules, for publishers and tools deciding before they push.
        api.MapGet("/policy", (HttpContext http, IOptions<PortalOptions> o) =>
        {
            var p = o.Value.Policy;
            return Results.Json(new
            {
                push = p.Push,
                binaries = p.BinariesAllowed ? "keys with the files right" : "off",
                linkHosts = p.Hosts.Count == 0 ? ["any https host"] : p.Hosts,
                newChannels = p.NewChannels,
                admin = p.Admin,
                plainHttp = p.PlainHttp,
                signedPush = p.SignedPush,
                publish = $"{Site(http, o.Value)}/publishers",
                minPkg = p.MinPkg.Trim().Length > 0 ? p.MinPkg.Trim() : null,
                minPkgReads = p.MinPkgReads,
                note = p.Note.Length > 0 ? p.Note : null,
                contact = p.Contact.Length > 0 ? p.Contact : null,
            });
        });

        api.MapGet("/packages/{channel}/{name}", (HttpContext http, string channel, string name, Catalogue c,
                                                      Search search, IOptions<PortalOptions> o) =>
            c.Package(channel, name) is { } p ? Results.Json(Describe(p, Site(http, o.Value), search, null, versions: true))
                                              : Results.NotFound());

        // How Pkg spreads, as the Statistics page shows it: a day, a build, a
        // kind of request and how many. Nothing per request, per address or per
        // person; /privacy says what is kept and what is not.
        api.MapGet("/usage", (Usage usage) => Results.Json(new
        {
            kept = new[] { "day", "version", "system", "cpu", "kind", "count" },
            notKept = new[] { "addresses", "identifiers", "any record of a single request", "any count of people" },
            privacy = "/privacy",
            days = usage.All().GroupBy(u => u.Day).OrderBy(g => g.Key, StringComparer.Ordinal).Select(g => new
            {
                day = g.Key,
                reads = g.Where(u => u.Kind == Usage.Reads).Sum(u => u.Count),
                downloads = g.Where(u => u.Kind == Usage.Downloads).Sum(u => u.Count),
                pushes = g.Where(u => u.Kind == Usage.Pushes).Sum(u => u.Count),
                builds = g.GroupBy(u => (u.Version, u.System, u.Cpu)).Select(b => new
                {
                    version = b.Key.Version, system = b.Key.System, cpu = b.Key.Cpu, count = b.Sum(u => u.Count),
                }),
            }),
        }));

        app.MapGet("/feed", (HttpContext http, Catalogue c, IOptions<PortalOptions> o) =>
            Atom(http, o.Value, "AROS Packages: new versions", "/feed", "/",
                 c.Listed().SelectMany(ch => ch.Packages.Values)));

        app.MapGet("/channels/{name}/feed", (HttpContext http, string name, Catalogue c, IOptions<PortalOptions> o) =>
            c.Get(name) is { } ch
                ? Atom(http, o.Value, $"AROS Packages: new in {ch.Name}", $"/channels/{ch.Name}/feed", $"/channels/{ch.Name}", ch.Packages.Values)
                : Results.NotFound());

        app.MapGet("/api", () => Results.Redirect("/developer/api"));

        app.MapGet("/examples/{name}", (string name) =>
            name is "selfupdate.c" or "selfupdate-demo.sh" or "basic.c" or "browse.c" or "activity.c"
                ? Results.Text(File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "examples", name)), "text/plain; charset=utf-8")
                : Results.NotFound());

        // A badge needs no more than the site's address and the package's name:
        // /badge/<name>.svg, and /badge/<channel>/<name>.svg when two channels
        // carry the same name.
        app.MapGet("/badge/{a}/{b?}", (HttpContext http, string a, string? b, Catalogue c) =>
        {
            var file = b ?? a;
            if (!file.EndsWith(".svg", StringComparison.Ordinal)) return Results.NotFound();
            var p = b is null ? Find(c, file[..^4]) : c.Package(a, file[..^4]);
            if (p is null) return Results.NotFound();
            http.Response.Headers.CacheControl = "public, max-age=300";
            return Results.Text(Badge(p.Name, p.Latest.Version, http.Request.Query["size"] == "large"), "image/svg+xml; charset=utf-8");
        });

        // One address for a package, badge and link both: a README shows it as an
        // image, a person who clicks it lands on the page that installs it.
        app.MapGet("/b/{a}/{b?}", (HttpContext http, string a, string? b, Catalogue c) =>
        {
            var p = b is null ? Find(c, a) : c.Package(a, b);
            if (p is null) return Results.NotFound();
            http.Response.Headers.Vary = "Accept";
            var wants = http.Request.Headers.Accept.ToString();
            if (wants.Contains("text/html", StringComparison.OrdinalIgnoreCase))
                return Results.Redirect($"/packages/{p.Channel}/{p.Name}#install");
            http.Response.Headers.CacheControl = "public, max-age=300";
            return Results.Text(Badge(p.Name, p.Latest.Version, http.Request.Query["size"] == "large"), "image/svg+xml; charset=utf-8");
        });
    }

    /// The package of that name: a pinned one first, else the channel that has
    /// the newest of it. A name is enough for a badge; two channels rarely share one.
    public static PackageInfo? Find(Catalogue c, string name) =>
        c.Pinned().FirstOrDefault(p => string.Equals(p.Name, name, StringComparison.OrdinalIgnoreCase))
        ?? c.Listed().SelectMany(ch => ch.Packages.Values)
            .Where(p => string.Equals(p.Name, name, StringComparison.OrdinalIgnoreCase))
            .OrderByDescending(p => p.Updated).FirstOrDefault();

    static string Site(HttpContext http, PortalOptions o) =>
        o.PublicUrl.Length > 0 ? o.PublicUrl.TrimEnd('/') : $"{http.Request.Scheme}://{http.Request.Host}";

    static object Describe(PackageInfo p, string site, Search search, string? why, bool versions = false)
    {
        var m = p.Latest.Manifest;
        var channelUrl = $"{site}/{p.Channel}";
        return new
        {
            channel = p.Channel,
            name = p.Name,
            version = p.Latest.Version,
            kind = p.Kind,
            archs = p.Archs.ToArray(),
            @short = m.Short,
            category = m.Category,
            tags = m.Tags,
            provides = m.Provides,
            authors = m.Authors,
            license = m.License,
            signer = p.Latest.Signer,
            updated = p.Updated,
            downloads = search.DownloadsOf(p),
            why,
            page = $"{site}/packages/{p.Channel}/{p.Name}",
            channelUrl,
            install = $"pkg INSTALL {p.Name} ROOT <root> CHANNEL {channelUrl}",
            versions = versions
                ? p.Versions.Select(v => new
                {
                    version = v.Version, arch = v.Arch, withdrawn = v.Withdrawn, firstSeen = v.Published,
                    manifest = v.Line.Digest, payload = v.Manifest.Payload, signer = v.Signer, downloads = search.DownloadsOf(v),
                })
                : null,
        };
    }

    /// The newest versions first, one entry per version and CPU.
    static IResult Atom(HttpContext http, PortalOptions o, string title, string self, string alternate, IEnumerable<PackageInfo> packages)
    {
        var site = Site(http, o);
        var entries = packages.SelectMany(p => p.Versions).OrderByDescending(v => v.Published).Take(50).ToList();
        var updated = entries.Count > 0 ? entries[0].Published : DateTime.UnixEpoch;
        static string X(string? s) => SecurityElement.Escape(s ?? "");
        static string T(DateTime d) => d.ToUniversalTime().ToString("yyyy-MM-dd'T'HH:mm:ss'Z'", CultureInfo.InvariantCulture);
        var sb = new StringBuilder();
        sb.Append("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<feed xmlns=\"http://www.w3.org/2005/Atom\">\n");
        sb.Append($"  <title>{X(title)}</title>\n  <id>{X(site + self)}</id>\n  <updated>{T(updated)}</updated>\n");
        sb.Append($"  <link rel=\"self\" href=\"{X(site + self)}\"/>\n  <link rel=\"alternate\" href=\"{X(site + alternate)}\"/>\n");
        foreach (var v in entries)
        {
            var page = $"{site}/packages/{v.Channel}/{v.Name}?version={Uri.EscapeDataString(v.Version)}&arch={v.Arch}";
            sb.Append("  <entry>\n");
            sb.Append($"    <title>{X($"{v.Name} {v.Version} ({v.Arch})")}</title>\n");
            sb.Append($"    <id>{X($"{site}/{v.Channel}/objects/{v.Line.Digest}.manifest")}</id>\n");
            sb.Append($"    <updated>{T(v.Published)}</updated>\n");
            sb.Append($"    <link href=\"{X(page)}\"/>\n");
            sb.Append($"    <author><name>{X(v.Manifest.Authors.FirstOrDefault() ?? v.Channel)}</name></author>\n");
            sb.Append($"    <summary>{X(v.Manifest.Short ?? $"{v.Manifest.Kind} for {v.Arch}, {v.Manifest.Files.Count} files")}</summary>\n");
            sb.Append("  </entry>\n");
        }
        sb.Append("</feed>\n");
        return Results.Text(sb.ToString(), "application/atom+xml; charset=utf-8");
    }

    /// A two-part badge, name and version, sized from the text.
    public static string Badge(string name, string version, bool large = false)
    {
        static int W(string s) => 10 + (int)Math.Ceiling(s.Length * 6.6);
        int a = W(name), b = W(version), w = a + b;
        var n = SecurityElement.Escape(name);
        var v = SecurityElement.Escape(version);
        if (large)
        {
            int width = Math.Max(280, 84 + Math.Max((name.Length + 4) * 12, (version.Length + 13) * 8));
            return $"""
                <svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="72" viewBox="0 0 {width} 72" role="img" aria-label="Get {n} from the pkg portal, version {v}">
                  <title>Get {n} from the pkg portal, version {v}</title>
                  <rect width="{width}" height="72" rx="10" fill="#59358c"/>
                  <path d="M28 20v22m-8-8 8 8 8-8M18 48v6h20v-6" fill="none" stroke="#fff" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"/>
                  <g fill="#fff" font-family="Verdana,DejaVu Sans,sans-serif">
                    <text x="56" y="29" font-size="18" font-weight="bold">Get {n}</text>
                    <text x="56" y="51" font-size="12">{v} · pkg portal</text>
                  </g>
                </svg>
                """;
        }
        return $"""
            <svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="20" role="img" aria-label="{n}: {v}">
              <title>{n}: {v}</title>
              <clipPath id="r"><rect width="{w}" height="20" rx="3"/></clipPath>
              <g clip-path="url(#r)">
                <rect width="{a}" height="20" fill="#555"/>
                <rect x="{a}" width="{b}" height="20" fill="#8547b3"/>
              </g>
              <g fill="#fff" text-anchor="middle" font-family="Verdana,DejaVu Sans,sans-serif" font-size="11">
                <text x="{a / 2.0:0.#}" y="14">{n}</text>
                <text x="{a + b / 2.0:0.#}" y="14">{v}</text>
              </g>
            </svg>
            """;
    }
}
