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
