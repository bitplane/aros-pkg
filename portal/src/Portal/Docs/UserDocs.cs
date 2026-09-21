// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Collections.Concurrent;
using System.Text.RegularExpressions;
using Markdig;
using Markdig.Renderers.Html;
using Markdig.Syntax;

namespace Portal.Docs;

/// <summary>
/// The repository's user guides (README.md and docs/**, bundled at build
/// time without the contributors' pages), rendered as the site's
/// documentation. One source: the pages show what the Pkg built from the same
/// commit does. The README's Guides list decides the order.
/// </summary>
public static partial class UserDocs
{
    /// Slug: the path under docs/ without ".md" ("using", "commands/install");
    /// a folder's README.md is the folder ("commands"); the top README is "".
    public sealed record Page(string Slug, string Title, string File, bool InMenu);
    public sealed record Rendered(string Title, string Html, List<(int Level, string Id, string Text)> Headings);

    /// A page a search answers: where it is, how well it matched, and the
    /// sentence it matched in, to show under the title.
    public sealed record Hit(Page Page, int Score, string Snippet, string? Anchor);

    static readonly string Root = Path.Combine(AppContext.BaseDirectory, "userdocs");

    public static readonly Page[] Pages = Discover();

    static Page[] Discover()
    {
        if (!Directory.Exists(Root)) return [];
        var readme = File.Exists(Path.Combine(Root, "README.md")) ? File.ReadAllText(Path.Combine(Root, "README.md")) : "";
        int list = readme.IndexOf("\n## Guides", StringComparison.Ordinal);
        var guides = list >= 0 ? readme[list..] : readme;
        int Rank(string rel) { int i = guides.IndexOf("docs/" + rel, StringComparison.Ordinal); return i < 0 ? int.MaxValue : i; }
        var pages = Directory.EnumerateFiles(Root, "*.md", SearchOption.AllDirectories)
            .Select(f => Path.GetRelativePath(Root, f).Replace('\\', '/'))
            .Where(rel => rel != "README.md")
            .Select(rel => new Page(SlugOf(rel), TitleOf(Path.Combine(Root, rel)) ?? SlugOf(rel), rel, Rank(rel) != int.MaxValue))
            .OrderBy(p => Rank(p.File)).ThenBy(p => p.Slug, StringComparer.Ordinal);
        return [new Page("", "Getting started", "README.md", true), .. pages];
    }

    static string SlugOf(string rel) =>
        rel.EndsWith("/README.md", StringComparison.Ordinal) ? rel[..^"/README.md".Length] : rel[..^".md".Length];

    static string? TitleOf(string path) =>
        File.ReadLines(path).FirstOrDefault(l => l.StartsWith("# ", StringComparison.Ordinal))?[2..].Trim();

    static readonly MarkdownPipeline Pipeline = new MarkdownPipelineBuilder()
        .UseAutoIdentifiers(Markdig.Extensions.AutoIdentifiers.AutoIdentifierOptions.GitHub)
        .UsePipeTables().UseAutoLinks().Build();

    static readonly ConcurrentDictionary<string, Rendered?> Cache = new();
    static readonly ConcurrentDictionary<string, string> PlainText = new();

    /// The guides that answer a query, best first. The search is over the words
    /// of the Markdown itself, so what a reader sees is what is searched.
    public static List<Hit> Search(string query)
    {
        var words = query.Split(' ', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
            .Where(w => w.Length > 1).Distinct(StringComparer.OrdinalIgnoreCase).ToList();
        if (words.Count == 0) return [];
        var hits = new List<Hit>();
        foreach (var page in Pages)
        {
            var text = PlainText.GetOrAdd(page.Slug, _ =>
            {
                var path = Path.Combine(Root, page.File);
                return File.Exists(path) ? File.ReadAllText(path) : "";
            });
            if (text.Length == 0) continue;
            int score = 0, first = -1;
            foreach (var w in words)
            {
                var n = Count(text, w);
                if (n == 0) continue;
                score += n + (page.Title.Contains(w, StringComparison.OrdinalIgnoreCase) ? 40 : 0);
                var at = text.IndexOf(w, StringComparison.OrdinalIgnoreCase);
                if (at >= 0 && (first < 0 || at < first)) { first = at; }
            }
            if (score == 0 || first < 0) continue;
            hits.Add(new Hit(page, score, Sentence(text, first), AnchorAt(text, first)));
        }
        return hits.OrderByDescending(h => h.Score).ThenBy(h => h.Page.Title, StringComparer.Ordinal).ToList();
    }

    static int Count(string text, string word)
    {
        int n = 0;
        for (int i = text.IndexOf(word, StringComparison.OrdinalIgnoreCase); i >= 0;
             i = text.IndexOf(word, i + word.Length, StringComparison.OrdinalIgnoreCase)) n++;
        return n;
    }

    /// The sentence around a match, trimmed of the Markdown that surrounds it.
    static string Sentence(string text, int at)
    {
        int from = Math.Max(0, at - 120), to = Math.Min(text.Length, at + 160);
        var cut = text[from..to].Replace('\n', ' ').Replace('\r', ' ');
        cut = MarkdownBits().Replace(cut, "$1");
        cut = Spaces().Replace(cut, " ").Trim();
        return (from > 0 ? "…" : "") + cut + (to < text.Length ? "…" : "");
    }

    /// The heading a match sits under, so a result lands where the word is.
    static string? AnchorAt(string text, int at)
    {
        var md = TopTitle().Replace(text, "", 1);
        var position = at - (text.Length - md.Length);
        var doc = Markdown.Parse(md, Pipeline);
        return doc.Descendants<HeadingBlock>()
            .Where(h => h.Level is 2 or 3 && h.Span.Start <= position)
            .LastOrDefault()?.GetAttributes().Id;
    }

    [GeneratedRegex(@"[*_`#>]|\[([^\]]*)\]\([^)]*\)")] private static partial Regex MarkdownBits();
    [GeneratedRegex(@"\s{2,}")] private static partial Regex Spaces();

    public static Rendered? Render(string slug) => Cache.GetOrAdd(slug, s =>
    {
        var page = Pages.FirstOrDefault(p => p.Slug == s);
        var path = page is null ? null : Path.Combine(Root, page.File);
        if (page is null || !File.Exists(path)) return null;
        var md = TopTitle().Replace(File.ReadAllText(path), "", 1);   // the page has its own title
        if (page.File == "README.md")
        {
            // The README ends with material for contributors, which stays in the repository.
            foreach (var marker in new[] { "\n## For contributors", "\nFor contributors", "\nHow Pkg is built" })
            {
                int cut = md.IndexOf(marker, StringComparison.Ordinal);
                if (cut > 0) { md = md[..cut]; break; }
            }
        }
        var doc = Markdown.Parse(md, Pipeline);
        foreach (var table in doc.Descendants<Markdig.Extensions.Tables.Table>())
        {
            if (table.Count > 0 && string.Concat(table[0].Descendants<Markdig.Syntax.Inlines.LiteralInline>()
                    .Select(l => l.Content.ToString())) == "VerbFormDoes")
                table.GetAttributes().AddClass("verb-list");
        }
        var headings = doc.Descendants<HeadingBlock>().Where(h => h.Level is 2 or 3)
            .Select(h => (h.Level, h.GetAttributes().Id ?? "", Inline(h))).ToList();
        var here = page.File.Contains('/') ? page.File[..(page.File.LastIndexOf('/') + 1)] : "";
        var html = LocalLink().Replace(doc.ToHtml(Pipeline), m => Link(m, page.File == "README.md" ? "" : "docs/" + here));
        return new Rendered(page.Title, html, headings);
    });

    /// A link between guides becomes a /docs address; one to a page that is not
    /// published here (contributors' pages, GOAL.md) keeps its text, without a link.
    static string Link(Match m, string baseDir)
    {
        var target = Normalize(baseDir + m.Groups["path"].Value);
        var frag = m.Groups["frag"].Value;
        if (target.StartsWith("docs/", StringComparison.Ordinal))
        {
            var page = Pages.FirstOrDefault(p => p.File == target["docs/".Length..]);
            if (page is not null) return $"<a href=\"/docs{(page.Slug.Length > 0 ? "/" + page.Slug : "")}{frag}\"";
        }
        else if (target == "README.md") return $"<a href=\"/docs{frag}\"";
        return "<a data-unpublished=\"1\"";
    }

    static string Normalize(string path)
    {
        var parts = new List<string>();
        foreach (var p in path.Split('/'))
            if (p == "..") { if (parts.Count > 0) parts.RemoveAt(parts.Count - 1); }
            else if (p is not ("" or ".")) parts.Add(p);
        return string.Join('/', parts);
    }

    static string Inline(HeadingBlock h) =>
        string.Concat(h.Inline?.Descendants<Markdig.Syntax.Inlines.LiteralInline>().Select(l => l.Content.ToString()) ?? []);

    [GeneratedRegex(@"\A(?:\s*<!--.*?-->)*\s*# [^\n]*\n", RegexOptions.Singleline)]
    private static partial Regex TopTitle();

    // <a href="docs/using.md#x">, <a href="../reference.md">: relative links to Markdown files.
    [GeneratedRegex(@"<a href=""(?<path>(?![a-z]+:)[^""#]*\.md)(?<frag>#[^""]*)?""")]
    private static partial Regex LocalLink();
}
