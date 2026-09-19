// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Net;
using System.Net.Http.Headers;
using Microsoft.AspNetCore.Mvc.Testing;
using Portal.Push;

namespace Portal.Tests;

/// The gate in front of every push: https, a known key, a channel the key
/// may push to. The test server speaks plain http, so it stands for a
/// request that reached the portal without TLS.
public class PushGateTests : IClassFixture<PushGateTests.Factory>
{
    public sealed class Factory : WebApplicationFactory<Program>
    {
        public readonly string Data = Directory.CreateTempSubdirectory("portal-test-").FullName;
        public readonly string Key;
        readonly string config;

        public Factory() => (Key, config) = PublisherKeys.Create("tester", "pkg");

        protected override void ConfigureWebHost(Microsoft.AspNetCore.Hosting.IWebHostBuilder b)
        {
            b.UseSetting("Portal:DataDir", Data);
            b.UseSetting("Portal:Keys", config);
            b.UseSetting("Portal:AllowLoopbackHttpPush", "false");
        }
    }

    readonly Factory f;
    public PushGateTests(Factory f) => this.f = f;

    async Task<(HttpStatusCode, string)> Plan(string channel, string? key, string scheme = "http")
    {
        var c = f.CreateClient(new WebApplicationFactoryClientOptions { BaseAddress = new Uri($"{scheme}://localhost") });
        var req = new HttpRequestMessage(HttpMethod.Post, $"/{channel}/_push/plan") { Content = new StringContent("") };
        if (key is not null) req.Headers.Authorization = new AuthenticationHeaderValue("Bearer", key);
        var r = await c.SendAsync(req);
        return (r.StatusCode, await r.Content.ReadAsStringAsync());
    }

    [Fact]
    public async Task A_push_over_plain_http_is_refused_even_with_a_good_key()
    {
        var (status, body) = await Plan("pkg", f.Key, "http");
        Assert.Equal(HttpStatusCode.Forbidden, status);
        Assert.Contains("result: refused", body);
        Assert.Contains("https", body);
    }

    [Fact]
    public async Task Https_with_a_good_key_is_let_through() =>
        Assert.Equal(HttpStatusCode.OK, (await Plan("pkg", f.Key, "https")).Item1);

    [Fact]
    public async Task No_key_or_an_unknown_key_is_refused()
    {
        Assert.Equal(HttpStatusCode.Unauthorized, (await Plan("pkg", null, "https")).Item1);
        Assert.Equal(HttpStatusCode.Unauthorized, (await Plan("pkg", "pkgk_" + new string('0', 48), "https")).Item1);
    }

    [Fact]
    public async Task A_key_is_refused_outside_its_channels() =>
        Assert.Equal(HttpStatusCode.Forbidden, (await Plan("contrib-nightly", f.Key, "https")).Item1);

    [Fact]
    public async Task Pages_and_channel_files_are_not_to_be_indexed()
    {
        var c = f.CreateClient();
        var r = await c.GetAsync("/");
        Assert.Equal(HttpStatusCode.OK, r.StatusCode);
        Assert.Equal("noindex, nofollow", r.Headers.GetValues("X-Robots-Tag").Single());
        Assert.Contains("Disallow: /", await c.GetStringAsync("/robots.txt"));
        Assert.Equal(HttpStatusCode.NotFound, (await c.GetAsync("/pkg/archives/x.tar.bz2.pkgidx")).StatusCode);
    }
}

/// A key without the files right publishes by link only: manifests and
/// signatures, never a payload, an archive or a bootstrap program.
public class LinkOnlyTests : IClassFixture<LinkOnlyTests.Factory>
{
    public sealed class Factory : WebApplicationFactory<Program>
    {
        public readonly string Data = Directory.CreateTempSubdirectory("portal-link-").FullName;
        public readonly string Key;
        readonly string config;
        public Factory() => (Key, config) = PublisherKeys.Create("friend", "shared");
        protected override void ConfigureWebHost(Microsoft.AspNetCore.Hosting.IWebHostBuilder b)
        {
            b.UseSetting("Portal:DataDir", Data);
            b.UseSetting("Portal:Keys", config);
        }
    }

    readonly Factory f;
    public LinkOnlyTests(Factory f) => this.f = f;

    async Task<(HttpStatusCode, string)> Send(HttpMethod m, string url, string body)
    {
        var c = f.CreateClient(new WebApplicationFactoryClientOptions { BaseAddress = new Uri("https://localhost") });
        var req = new HttpRequestMessage(m, url) { Content = new StringContent(body) };
        req.Headers.Authorization = new AuthenticationHeaderValue("Bearer", f.Key);
        var r = await c.SendAsync(req);
        return (r.StatusCode, await r.Content.ReadAsStringAsync());
    }

    [Fact]
    public async Task The_plan_refuses_every_binary_and_accepts_manifests()
    {
        var h = new string('a', 64);
        var (_, body) = await Send(HttpMethod.Post, "/shared/_push/plan",
            $"objects/{h}.pkg {h} 10\narchives/x.tar.bz2 {h} 10\nBootstrap/x86_64/Pkg {h} 10\nInstall-Pkg {h} 10\n" +
            $"objects/{h}.manifest {h} 10\nobjects/{h}.sig {h} 10\n");
        Assert.Equal(4, body.Split('\n').Count(l => l.StartsWith("refused: ") && l.Contains("link only")));
        Assert.Contains($"need: objects/{h}.manifest", body);
        Assert.Contains($"need: objects/{h}.sig", body);
    }

    [Fact]
    public async Task An_upload_of_a_payload_is_refused_even_without_a_plan()
    {
        var (status, body) = await Send(HttpMethod.Put, $"/shared/_push/files/objects/{new string('b', 64)}.pkg", "x");
        Assert.Equal(HttpStatusCode.Forbidden, status);
        Assert.Contains("link only", body);
    }

    [Fact]
    public async Task A_version_with_a_payload_is_refused_at_commit()
    {
        var m = "Format: pkg-manifest 1\nName: evil\nVersion: 1.0\nArchitecture: generic\nKind: data\nPayload: " + new string('c', 64) + "\n";
        var d = Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(System.Text.Encoding.ASCII.GetBytes(m))).ToLowerInvariant();
        var sig = "Signer: " + new string('1', 64) + "\nSignature: " + new string('2', 128) + "\n";
        var ds = Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(System.Text.Encoding.ASCII.GetBytes(sig))).ToLowerInvariant();
        await Send(HttpMethod.Post, "/shared/_push/plan", $"objects/{d}.manifest {d} {m.Length}\nobjects/{d}.sig {ds} {sig.Length}\n");
        await Send(HttpMethod.Put, $"/shared/_push/files/objects/{d}.manifest", m);
        await Send(HttpMethod.Put, $"/shared/_push/files/objects/{d}.sig", sig);
        var (_, body) = await Send(HttpMethod.Post, "/shared/_push/commit", $"evil 1.0 generic {d}\n");
        Assert.Contains("result: refused", body);
        Assert.Contains("link only", body);
        Assert.False(File.Exists(Path.Combine(f.Data, "channels", "shared", "index")) &&
                     File.ReadAllText(Path.Combine(f.Data, "channels", "shared", "index")).Contains("evil"));
    }
}
