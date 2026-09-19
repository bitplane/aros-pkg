// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Collections.Concurrent;
using System.Text.RegularExpressions;
using Markdig;
using Markdig.Renderers.Html;
using Markdig.Syntax;

namespace Portal.Docs;

/// <summary>
/// The repository's user guides (README.md and docs/*.md, bundled at build
/// time), rendered as the site's documentation. One source: the pages show
/// what the Pkg built from the same commit does.
/// </summary>
public static partial class UserDocs
{
    public sealed record Page(string Slug, string Title, string File);
    public sealed record Rendered(string Title, string Html, List<(int Level, string Id, string Text)> Headings);

    /// The README first, then every bundled guide in the order the README
    /// links them, each titled by its own first heading. Nothing to update
    /// here when guides are added, renamed or split.
    public static readonly Page[] Pages = Discover();

    static Page[] Discover()
    {
        var dir = Path.Combine(AppContext.BaseDirectory, "userdocs");
        if (!Directory.Exists(dir)) return [];
        var readme = File.Exists(Path.Combine(dir, "README.md")) ? File.ReadAllText(Path.Combine(dir, "README.md")) : "";
        // The README's own list of guides gives the order, when it has one.
        int list = readme.IndexOf("\n## Guides", StringComparison.Ordinal);
        if (list >= 0) readme = readme[list..];
        int Rank(string file) { int i = readme.IndexOf("docs/" + file, StringComparison.Ordinal); return i < 0 ? int.MaxValue : i; }
        var guides = Directory.EnumerateFiles(dir, "*.md").Select(Path.GetFileName).OfType<string>()
            .Where(f => f != "README.md" && !f.StartsWith("development", StringComparison.Ordinal))
            .OrderBy(Rank).ThenBy(f => f, StringComparer.Ordinal)
            .Select(f => new Page(f[..^3], TitleOf(Path.Combine(dir, f)) ?? f[..^3], f));
        return [new Page("", "Getting started", "README.md"), .. guides];
    }

    static string? TitleOf(string path) =>
        File.ReadLines(path).FirstOrDefault(l => l.StartsWith("# ", StringComparison.Ordinal))?[2..].Trim();

    static readonly MarkdownPipeline Pipeline = new MarkdownPipelineBuilder()
        .UseAutoIdentifiers(Markdig.Extensions.AutoIdentifiers.AutoIdentifierOptions.GitHub)
        .UsePipeTables().UseAutoLinks().Build();

    static readonly ConcurrentDictionary<string, Rendered?> Cache = new();

    public static Rendered? Render(string slug) => Cache.GetOrAdd(slug, s =>
    {
        var page = Pages.FirstOrDefault(p => p.Slug == s);
        var path = page is null ? null : Path.Combine(AppContext.BaseDirectory, "userdocs", page.File);
        if (page is null || !File.Exists(path)) return null;
        var md = File.ReadAllText(path);
        // The README ends with the developers' part, which lives in the repository.
        if (page.File == "README.md")
        {
            int cut = md.IndexOf("\nHow Pkg is built", StringComparison.Ordinal);
            if (cut > 0) md = md[..cut];
            md = TopTitle().Replace(md, "", 1);   // the page has its own title
        }
        else md = TopTitle().Replace(md, "", 1);
        var doc = Markdown.Parse(md, Pipeline);
        var headings = doc.Descendants<HeadingBlock>().Where(h => h.Level is 2 or 3)
            .Select(h => (h.Level, h.GetAttributes().Id ?? "", Inline(h))).ToList();
        var html = LocalLink().Replace(doc.ToHtml(Pipeline), m =>
        {
            var file = m.Groups["file"].Value;
            var target = Pages.FirstOrDefault(p => p.File == file);
            return target is null ? m.Value : $"href=\"/docs{(target.Slug.Length > 0 ? "/" + target.Slug : "")}{m.Groups["frag"].Value}\"";
        });
        return new Rendered(page.Title, html, headings);
    });

    static string Inline(HeadingBlock h) =>
        string.Concat(h.Inline?.Descendants<Markdig.Syntax.Inlines.LiteralInline>().Select(l => l.Content.ToString()) ?? []);

    [GeneratedRegex(@"\A\s*# [^\n]*\n")]
    private static partial Regex TopTitle();

    // href="docs/using.md#x", href="using.md", href="../README.md"
    [GeneratedRegex(@"href=""(?:\.\./|docs/)?(?<file>[A-Za-z]+\.md)(?<frag>#[^""]*)?""")]
    private static partial Regex LocalLink();
}
