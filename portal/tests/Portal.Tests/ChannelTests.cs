// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using Portal.Channels;

namespace Portal.Tests;

public class ChannelTests
{
    // The order pkg_version_cmp gives, from src/pkg_manifest.c and the README.
    [Theory]
    [InlineData("41.7", "41.7+20260917")]
    [InlineData("41.7+20260917", "41.7+20260918")]
    [InlineData("41.7+20260918", "41.8")]
    [InlineData("1.9", "1.10")]
    [InlineData("0+20260918", "0.1")]
    [InlineData("2", "2.0.1")]
    public void Versions_order_as_Pkg_orders_them(string lower, string higher)
    {
        Assert.True(PkgVersion.Order.Compare(lower, higher) < 0);
        Assert.True(PkgVersion.Order.Compare(higher, lower) > 0);
    }

    [Fact]
    public void Equal_versions_compare_equal_and_a_stray_letter_does_not_hang()
    {
        Assert.Equal(0, PkgVersion.Order.Compare("1.0", "1.0"));
        Assert.Equal(0, PkgVersion.Order.Compare("1.0", "1"));
        PkgVersion.Order.Compare("1.0a", "1.0b");
    }

    [Theory]
    [InlineData("index", ChannelPaths.Kind.Index)]
    [InlineData("objects/0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef.manifest", ChannelPaths.Kind.Object)]
    [InlineData("objects/0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef.withdrawn.sig", ChannelPaths.Kind.Object)]
    [InlineData("archives/AROS-20260918-pc-x86_64-contrib.tar.bz2", ChannelPaths.Kind.Archive)]
    [InlineData("archives/AROS-20260918-pc-x86_64-contrib.tar.bz2.sha256", ChannelPaths.Kind.ArchiveDigest)]
    [InlineData("Bootstrap/x86_64/Pkg", ChannelPaths.Kind.Mutable)]
    [InlineData("Install-Pkg", ChannelPaths.Kind.Mutable)]
    // Never served, whatever lies on disk.
    [InlineData("archives/AROS-20260918-pc-x86_64-contrib.tar.bz2.pkgidx", ChannelPaths.Kind.None)]
    [InlineData("archives/x.tar.bz2.url", ChannelPaths.Kind.None)]
    [InlineData("objects/0123.manifest", ChannelPaths.Kind.None)]
    [InlineData("objects/../../keys", ChannelPaths.Kind.None)]
    [InlineData("archives/../index", ChannelPaths.Kind.None)]
    [InlineData(".plan", ChannelPaths.Kind.None)]
    [InlineData("Bootstrap/x86_64/../../../etc/passwd", ChannelPaths.Kind.None)]
    public void Only_channel_files_are_served(string path, ChannelPaths.Kind kind) =>
        Assert.Equal(kind, ChannelPaths.Classify(path));

    [Theory]
    [InlineData("contrib-nightly", true)]
    [InlineData("pkg", true)]
    [InlineData("packages", false)]
    [InlineData("Pkg", false)]
    [InlineData("-x", false)]
    [InlineData("_push", false)]
    public void Channel_names(string name, bool ok) => Assert.Equal(ok, ChannelPaths.IsChannelName(name));

    [Fact]
    public void Manifest_attributes_reach_their_file_even_with_spaces_in_the_path()
    {
        var m = Manifest.Parse(
            "Format: pkg-manifest 1\nName: go\nVersion: 1.0+20260918\nArchitecture: m68k\nKind: application\n" +
            "Depends: lib >= 2.1\nDepends: other\n" +
            $"Source: A.tar.bz2!/A\nFile: {new string('a', 64)} 12 S/Go\nFile: {new string('b', 64)} 3 My Tool/Read Me\n" +
            "Protect: 0x00000041 S/Go\nComment: Starts%20the%20tool My Tool/Read Me\n");
        Assert.Equal(("go", "m68k", "A.tar.bz2", "A"), (m.Name, m.Architecture, m.SourceArchive, m.SourcePrefix));
        Assert.Equal(15, m.InstalledSize);
        Assert.Equal("0x00000041", m.Files[0].Protect);
        Assert.Equal("Starts the tool", m.Files[1].Comment);
        Assert.Equal(["lib >= 2.1", "other"], m.Depends.Select(d => d.ToString()));
    }
}
