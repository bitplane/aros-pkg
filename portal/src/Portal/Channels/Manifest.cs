// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Text;

namespace Portal.Channels;

/// <summary>
/// A manifest read for display. The portal never decides trust from it: Pkg
/// checks every manifest and signature before a push is committed. This reader
/// is lenient on purpose, so that a page still renders what it can.
/// </summary>
public sealed class Manifest
{
    public string Name { get; private set; } = "";
    public string Version { get; private set; } = "";
    public string Architecture { get; private set; } = "";
    public string Kind { get; private set; } = "";
    public string? Payload { get; private set; }
    public string? Source { get; private set; }
    public List<ManifestFile> Files { get; } = [];
    public List<ManifestFile> Content { get; } = [];
    public List<Dependency> Depends { get; } = [];

    // Catalogue fields, all optional (board thread 24). Signed like the rest.
    public string? Short { get; private set; }
    public List<string> Description { get; } = [];
    public string? Category { get; private set; }
    public List<string> Tags { get; } = [];
    public string? Author { get; private set; }
    public string? Homepage { get; private set; }
    public string? Repository { get; private set; }
    public string? License { get; private set; }
    public string? Distribution { get; private set; }
    public List<string> Changes { get; } = [];
    public string? Icon { get; private set; }
    public List<string> Screenshots { get; } = [];

    /// The long text as paragraphs: blank Description lines separate them.
    public IEnumerable<string> Paragraphs()
    {
        var cur = new List<string>();
        foreach (var l in Description)
        {
            if (l.Trim().Length == 0) { if (cur.Count > 0) yield return string.Join(" ", cur); cur.Clear(); }
            else cur.Add(l.Trim());
        }
        if (cur.Count > 0) yield return string.Join(" ", cur);
    }

    /// The archive a Source line names, by basename.
    public string? SourceArchive => Source is null ? null : Source.Split("!/", 2)[0];
    public string? SourcePrefix => Source is null ? null : Source.Split("!/", 2) is [_, var p] ? p : null;

    public long InstalledSize => Files.Sum(f => f.Size);

    public static Manifest Parse(string text)
    {
        var m = new Manifest();
        var byPath = new Dictionary<string, ManifestFile>(StringComparer.Ordinal);
        var attrs = new List<(string Key, string Value)>();
        foreach (var raw in text.Split('\n'))
        {
            var line = raw.TrimEnd('\r');
            if (line == "Description:") { m.Description.Add(""); continue; }
            int c = line.IndexOf(": ", StringComparison.Ordinal);
            if (c <= 0) continue;
            var key = line[..c];
            var val = line[(c + 2)..];
            switch (key)
            {
                case "Name": m.Name = val; break;
                case "Version": m.Version = val; break;
                case "Architecture": m.Architecture = val; break;
                case "Kind": m.Kind = val; break;
                case "Payload": m.Payload = val; break;
                case "Source": m.Source = val; break;
                case "File":
                case "Content":
                    if (ParseFile(val) is { } f)
                    {
                        (key == "File" ? m.Files : m.Content).Add(f);
                        byPath.TryAdd((key == "File" ? "F:" : "C:") + f.Path, f);
                    }
                    break;
                case "Depends":
                    var parts = val.Split(" >= ", 2);
                    m.Depends.Add(new Dependency(parts[0].Trim(), parts.Length > 1 ? parts[1].Trim() : null));
                    break;
                case "Short": m.Short = val.Trim(); break;
                case "Description": m.Description.Add(val); break;
                case "Category": m.Category = val.Trim(); break;
                case "Tags":
                    m.Tags.AddRange(val.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
                        .Select(t => t.ToLowerInvariant()).Where(t => !m.Tags.Contains(t)));
                    break;
                case "Author": m.Author = val.Trim(); break;
                case "Homepage": m.Homepage = val.Trim(); break;
                case "Repository": m.Repository = val.Trim(); break;
                case "License": m.License = val.Trim(); break;
                case "Distribution": m.Distribution = val.Trim(); break;
                case "Changes": m.Changes.Add(val.Trim()); break;
                case "Icon": m.Icon = val.Trim(); break;
                case "Screenshot": m.Screenshots.Add(val.Trim()); break;
                case "Protect":
                case "Comment":
                    attrs.Add((key, val));
                    break;
            }
        }
        // Protect and Comment name their file by path, which may hold spaces:
        // "Protect: 0x00000041 S/Go", "Comment: Starts%20the%20tool S/Go".
        foreach (var (key, val) in attrs)
        {
            int s = val.IndexOf(' ');
            if (s <= 0) continue;
            var head = val[..s];
            var path = val[(s + 1)..];
            var target = byPath.GetValueOrDefault("F:" + path) ?? byPath.GetValueOrDefault("C:" + path);
            if (target is null) continue;
            if (key == "Protect") target.Protect = head;
            else target.Comment = Uri.UnescapeDataString(head);
        }
        return m;
    }

    static ManifestFile? ParseFile(string val)
    {
        // <sha256> <size> <path>, the path being the rest of the line.
        var p = val.Split(' ', 3);
        if (p.Length < 3 || p[0].Length != 64 || !long.TryParse(p[1], out var size)) return null;
        return new ManifestFile(p[2], p[0], size);
    }

    public static Manifest Load(string path) => Parse(File.ReadAllText(path, Encoding.UTF8));
}

public sealed class ManifestFile(string path, string digest, long size)
{
    public string Path { get; } = path;
    public string Digest { get; } = digest;
    public long Size { get; } = size;
    public string? Protect { get; set; }
    public string? Comment { get; set; }
}

public sealed record Dependency(string Name, string? Min)
{
    public override string ToString() => Min is null ? Name : $"{Name} >= {Min}";
}
