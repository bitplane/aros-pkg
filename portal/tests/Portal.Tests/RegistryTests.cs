// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using Microsoft.Extensions.Options;
using Portal.Accounts;
using Portal.Channels;

namespace Portal.Tests;

/// What a publisher may register for themselves, and what stays the maintainers'.
public class RegistryTests
{
    static (Registry, Catalogue, string) New(string signedKeys = "")
    {
        var data = Directory.CreateTempSubdirectory("portal-registry-").FullName;
        var o = Options.Create(new PortalOptions { DataDir = data, SignedKeys = signedKeys });
        var c = new Catalogue(o);
        return (new Registry(o, c), c, data);
    }
    static readonly string K1 = new('1', 64), K2 = new('2', 64);

    [Fact]
    public void A_new_publisher_gets_their_own_unlisted_channel_by_link()
    {
        var (r, c, data) = New();
        Assert.Null(r.Register(7, "jane", "Jane Roe", K1.ToUpperInvariant(), "janes-tools"));
        var p = r.PublisherFor(K1)!;
        Assert.Equal("Jane Roe", p.Name);
        Assert.True(p.MayPush("janes-tools") && !p.MayPush("pkg") && !p.Files);
        Assert.True(c.IsUnlisted("janes-tools"));
        // it survives a restart: the file is the registry
        var again = new Registry(Options.Create(new PortalOptions { DataDir = data }), c);
        Assert.NotNull(again.PublisherFor(K1));
    }

    [Theory]
    [InlineData("pkg"), InlineData("aros-core"), InlineData("contrib-nightly"), InlineData("afsplus-alpha"), InlineData("Bad Name"), InlineData("api")]
    public void Names_kept_for_the_project_or_the_site_are_refused(string channel) =>
        Assert.NotNull(New().Item1.Register(7, "jane", "Jane", K1, channel));

    [Fact]
    public void Nobody_takes_another_publishers_key_or_channel_or_the_maintainers_key()
    {
        var (r, _, _) = New($"owner:{K2}:*:files");
        Assert.Null(r.Register(7, "jane", "Jane", K1, "tools"));
        Assert.Contains("another account", r.Register(8, "mallory", "Mallory", K1, "other"));
        Assert.Contains("another publisher", r.Register(8, "mallory", "Mallory", new string('3', 64), "tools"));
        Assert.Contains("maintainers", r.Register(8, "mallory", "Mallory", K2, "other"));
        Assert.Contains("64 hexadecimal", r.Register(8, "mallory", "Mallory", "Pkg-Secret-Key: 1", "other"));
    }

    [Fact]
    public void Three_channels_at_most_and_a_suspended_account_pushes_nothing()
    {
        var (r, _, _) = New();
        foreach (var ch in new[] { "a1", "a2", "a3" }) Assert.Null(r.Register(7, "jane", "Jane", K1, ch));
        Assert.Contains("at most", r.Register(7, "jane", "Jane", K1, "a4"));
        Assert.True(r.Change(7, x => x with { Suspended = true }));
        Assert.Null(r.PublisherFor(K1));
        Assert.Contains("suspended", r.Register(7, "jane", "Jane", K1, "a1"));
    }

    [Fact]
    public void A_maintainer_takes_a_settings_key_under_their_account_as_it_is()
    {
        var (r, _, _) = New($"owner:{K2}:*:files");
        Assert.Contains("not one of", r.Adopt(1, "jonx", K1));
        Assert.Null(r.Adopt(1, "jonx", K2.ToUpperInvariant()));
        var p = r.PublisherFor(K2)!;
        Assert.True(p.Name == "owner" && p.Files && p.MayPush("anything"));
        Assert.Equal("jonx", r.ByAccount(1)!.Login);
        Assert.Contains("another account", r.Adopt(2, "other", K2));
    }
}
