// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Net;
using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Text;
using Microsoft.AspNetCore.Mvc.Testing;
using Portal.Admin;
using Portal.Push;

namespace Portal.Tests;

/// The maintainers' API on a channel of two versions that share one payload.
public class AdminTests
{
    sealed class Factory : WebApplicationFactory<Program>
    {
        public readonly string Data = Directory.CreateTempSubdirectory("portal-admin-").FullName;
        public readonly string AdminKey, PushKey, Payload;
        public readonly string[] Digests = new string[2];
        readonly string adminConfig, pushConfig;

        public Factory()
        {
            (AdminKey, adminConfig) = AdminKeys.Create("owner");
            (PushKey, pushConfig) = PublisherKeys.Create("publisher", "*");
            var ch = Path.Combine(Data, "channels", "demo");
            Directory.CreateDirectory(Path.Combine(ch, "objects"));
            var pkg = Encoding.ASCII.GetBytes("shared payload");
            Payload = Convert.ToHexString(SHA256.HashData(pkg)).ToLowerInvariant();
            File.WriteAllBytes(Path.Combine(ch, "objects", Payload + ".pkg"), pkg);
            var index = new StringBuilder();
            for (int i = 0; i < 2; i++)
            {
                var m = $"Format: pkg-manifest 1\nName: tool\nVersion: 1.{i}\nArchitecture: generic\nKind: data\nPayload: {Payload}\n";
                Digests[i] = Convert.ToHexString(SHA256.HashData(Encoding.ASCII.GetBytes(m))).ToLowerInvariant();
                File.WriteAllText(Path.Combine(ch, "objects", Digests[i] + ".manifest"), m);
                File.WriteAllText(Path.Combine(ch, "objects", Digests[i] + ".sig"), "Signer: x\nSignature: y\n");
                index.Append($"tool 1.{i} generic {Digests[i]}\n");
            }
            File.WriteAllText(Path.Combine(ch, "index"), index.ToString());
        }

        protected override void ConfigureWebHost(Microsoft.AspNetCore.Hosting.IWebHostBuilder b)
        {
            b.UseSetting("Portal:DataDir", Data);
            b.UseSetting("Portal:AdminKeys", adminConfig);
            b.UseSetting("Portal:Keys", pushConfig);
        }

        public HttpClient Https() => CreateClient(new WebApplicationFactoryClientOptions { BaseAddress = new Uri("https://localhost") });
        public string Index => File.ReadAllText(Path.Combine(Data, "channels", "demo", "index"));
        public bool Has(string file) => File.Exists(Path.Combine(Data, "channels", "demo", "objects", file));
    }

    static async Task<(HttpStatusCode, string)> Post(HttpClient c, string url, string? key, string body = "")
    {
        var req = new HttpRequestMessage(HttpMethod.Post, url) { Content = new StringContent(body) };
        if (key is not null) req.Headers.Authorization = new AuthenticationHeaderValue("Bearer", key);
        var r = await c.SendAsync(req);
        return (r.StatusCode, await r.Content.ReadAsStringAsync());
    }

    [Fact]
    public async Task Only_an_admin_key_over_https_gets_in()
    {
        using var f = new Factory();
        Assert.Equal(HttpStatusCode.Unauthorized, (await Post(f.Https(), "/_admin/channels/demo/remove", null, "tool *")).Item1);
        Assert.Equal(HttpStatusCode.Unauthorized, (await Post(f.Https(), "/_admin/channels/demo/remove", f.PushKey, "tool *")).Item1);
        Assert.Equal(HttpStatusCode.Forbidden, (await Post(f.CreateClient(), "/_admin/channels/demo/remove", f.AdminKey, "tool *")).Item1);
        Assert.Contains("tool 1.0", f.Index);
    }

    [Fact]
    public async Task A_dry_run_changes_nothing()
    {
        using var f = new Factory();
        var before = f.Index;
        var (status, body) = await Post(f.Https(), "/_admin/channels/demo/remove?dryrun=1", f.AdminKey, "tool *");
        Assert.Equal(HttpStatusCode.OK, status);
        Assert.Contains("result: would-remove", body);
        Assert.Equal(before, f.Index);
        Assert.True(f.Has(f.Payload + ".pkg"));
    }

    [Fact]
    public async Task Removing_one_version_keeps_the_payload_the_other_still_needs()
    {
        using var f = new Factory();
        var (_, body) = await Post(f.Https(), "/_admin/channels/demo/remove", f.AdminKey, "tool 1.0 generic");
        Assert.Contains("removed: tool 1.0 generic", body);
        Assert.DoesNotContain("tool 1.0", f.Index);
        Assert.Contains("tool 1.1", f.Index);
        Assert.False(f.Has(f.Digests[0] + ".manifest"));
        Assert.True(f.Has(f.Payload + ".pkg"));
    }

    [Fact]
    public async Task Removing_everything_then_restoring_puts_it_all_back()
    {
        using var f = new Factory();
        var c = f.Https();
        var before = f.Index;
        var (_, body) = await Post(c, "/_admin/channels/demo/remove", f.AdminKey, "tool *");
        Assert.Equal("", f.Index);
        Assert.False(f.Has(f.Payload + ".pkg"));
        var stash = body.Split('\n').Single(l => l.StartsWith("stash: ")).Split(": ")[1];
        var (status, restored) = await Post(c, $"/_admin/restore/{stash}", f.AdminKey);
        Assert.Equal(HttpStatusCode.OK, status);
        Assert.Contains("result: restored", restored);
        Assert.Equal(before.Split('\n').Order(), f.Index.Split('\n').Order());
        Assert.True(f.Has(f.Payload + ".pkg") && f.Has(f.Digests[0] + ".sig"));
        var req = new HttpRequestMessage(HttpMethod.Get, "/_admin/log");
        req.Headers.Authorization = new AuthenticationHeaderValue("Bearer", f.AdminKey);
        var log = await (await c.SendAsync(req)).Content.ReadAsStringAsync();
        Assert.Contains("owner remove demo", log);
        Assert.Contains("owner restore", log);
    }

    [Fact]
    public async Task An_unlisted_channel_is_served_and_shown_nowhere_until_listed_again()
    {
        using var f = new Factory();
        var c = f.Https();
        Assert.Contains("tool", await c.GetStringAsync("/api/search?q=tool"));
        var (status, body) = await Post(c, "/_admin/channels/demo/unlist", f.AdminKey);
        Assert.Equal(HttpStatusCode.OK, status);
        Assert.Contains("result: unlisted", body);
        Assert.DoesNotContain("\"name\":\"tool\"", await c.GetStringAsync("/api/search?q=tool"));
        Assert.DoesNotContain("demo", await c.GetStringAsync("/"));
        Assert.DoesNotContain("tool", await c.GetStringAsync("/feed"));
        // whoever has the address still gets everything
        Assert.Contains("tool 1.0", await c.GetStringAsync("/demo/index"));
        Assert.Contains("unlisted", await c.GetStringAsync("/channels/demo"));
        Assert.Equal(HttpStatusCode.OK, (await c.GetAsync("/packages/demo/tool")).StatusCode);
        // a push key cannot do it
        Assert.Equal(HttpStatusCode.Unauthorized, (await Post(c, "/_admin/channels/demo/list", f.PushKey)).Item1);
        Assert.Contains("result: listed", (await Post(c, "/_admin/channels/demo/list", f.AdminKey)).Item2);
        Assert.Contains("\"name\":\"tool\"", await c.GetStringAsync("/api/search?q=tool"));
    }

    [Fact]
    public async Task A_view_key_in_the_browser_shows_what_is_unlisted_and_nothing_else_does()
    {
        var key = "view-key-for-the-test";
        var hash = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(key))).ToLowerInvariant();
        using var f = new Factory();
        using var g = f.WithWebHostBuilder(b => b.UseSetting("Portal:ViewKeys", $"owner:{hash}").UseSetting("Portal:Unlisted", "demo"));
        var plain = g.CreateClient(new WebApplicationFactoryClientOptions { BaseAddress = new Uri("https://localhost") });
        Assert.DoesNotContain("demo", await plain.GetStringAsync("/"));
        Assert.Equal(HttpStatusCode.NotFound, (await plain.GetAsync("/see/not-the-key")).StatusCode);
        var mine = g.CreateClient(new WebApplicationFactoryClientOptions { BaseAddress = new Uri("https://localhost"), HandleCookies = true });
        var home = await mine.GetStringAsync($"/see/{key}");                 // sets the cookie, lands on the home page
        Assert.Contains("/channels/demo", home);
        Assert.Contains(">unlisted<", home);                                // and it is marked as such
        Assert.Contains("\"name\":\"tool\"", await mine.GetStringAsync("/api/search?q=tool"));
        Assert.DoesNotContain("demo", await plain.GetStringAsync("/"));      // another browser still sees nothing
        Assert.DoesNotContain("/channels/demo", await mine.GetStringAsync("/see/off"));
    }

    [Fact]
    public async Task A_name_that_matches_nothing_is_refused_and_nothing_moves()
    {
        using var f = new Factory();
        var before = f.Index;
        var (_, body) = await Post(f.Https(), "/_admin/channels/demo/remove", f.AdminKey, "other *");
        Assert.Contains("result: refused", body);
        Assert.Equal(before, f.Index);
    }
}
