// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Xml.Linq;
using Microsoft.AspNetCore.Mvc.Testing;

namespace Portal.Tests;

/// The routes for tools and feed readers, on a channel written straight to
/// disk: the catalogue reads what is there, the checks at push are Pkg's.
public class ApiTests : IClassFixture<ApiTests.Factory>
{
    public sealed class Factory : WebApplicationFactory<Program>
    {
        public readonly string Data = Directory.CreateTempSubdirectory("portal-api-").FullName;
        public readonly string Payload;
        const string Signer = "a974a917b19cfc46eb462510fa21f95013932bfbda7c8f343e06a3e988f7bde7";

        public Factory()
        {
            var ch = Path.Combine(Data, "channels", "demo");
            Directory.CreateDirectory(Path.Combine(ch, "objects"));
            var pkg = Encoding.ASCII.GetBytes("a payload");
            Payload = Hex(SHA256.HashData(pkg));
            File.WriteAllBytes(Path.Combine(ch, "objects", Payload + ".pkg"), pkg);
            var index = new StringBuilder();
            foreach (var (version, when) in new[] { ("1.0", -2), ("1.1", -1) })
            {
                var manifest = $"Format: pkg-manifest 1\nName: sdltool\nVersion: {version}\nArchitecture: x86_64\nKind: library\n" +
                               $"Short: Plays with SDL\nTags: games, sdl\nProvides: SDL2.library\nPayload: {Payload}\n" +
                               $"File: {new string('a', 64)} 3 Libs/SDL2.library\n";
                var d = Hex(SHA256.HashData(Encoding.ASCII.GetBytes(manifest)));
                var mp = Path.Combine(ch, "objects", d + ".manifest");
                File.WriteAllText(mp, manifest);
                File.SetLastWriteTimeUtc(mp, DateTime.UtcNow.AddDays(when));
                File.WriteAllText(Path.Combine(ch, "objects", d + ".sig"), $"Signer: {Signer}\nSignature: {new string('0', 128)}\n");
                index.Append($"sdltool {version} x86_64 {d}\n");
            }
            File.WriteAllText(Path.Combine(ch, "index"), index.ToString());
        }

        static string Hex(byte[] b) => Convert.ToHexString(b).ToLowerInvariant();

        protected override void ConfigureWebHost(Microsoft.AspNetCore.Hosting.IWebHostBuilder b)
        {
            b.UseSetting("Portal:DataDir", Data);
            b.UseSetting("Portal:PublicUrl", "https://portal.test");
        }
    }

    readonly Factory f;
    public ApiTests(Factory f) => this.f = f;

    [Fact]
    public async Task Search_answers_json_and_says_why_a_library_name_matched()
    {
        var c = f.CreateClient();
        var r = await c.GetAsync("/api/search?q=SDL2.library");
        Assert.Equal(HttpStatusCode.OK, r.StatusCode);
        Assert.Equal("application/json", r.Content.Headers.ContentType?.MediaType);
        using var doc = JsonDocument.Parse(await r.Content.ReadAsStringAsync());
        var first = doc.RootElement.GetProperty("results")[0];
        Assert.Equal("sdltool", first.GetProperty("name").GetString());
        Assert.Equal("1.1", first.GetProperty("version").GetString());
        Assert.Equal("provides SDL2.library", first.GetProperty("why").GetString());
        Assert.Equal("https://portal.test/demo", first.GetProperty("channelUrl").GetString());
        // A query nothing answers gives an empty list, not an error.
        using var none = JsonDocument.Parse(await c.GetStringAsync("/api/search?q=nothing-like-this"));
        Assert.Equal(0, none.RootElement.GetProperty("total").GetInt32());
    }

    [Fact]
    public async Task A_served_payload_counts_once_and_a_resumed_part_does_not()
    {
        var c = f.CreateClient();
        async Task<long> Count()
        {
            using var d = JsonDocument.Parse(await c.GetStringAsync("/api/packages/demo/sdltool"));
            return d.RootElement.GetProperty("downloads").GetInt64();
        }
        var before = await Count();
        Assert.Equal(HttpStatusCode.OK, (await c.GetAsync($"/demo/objects/{f.Payload}.pkg")).StatusCode);
        var part = new HttpRequestMessage(HttpMethod.Get, $"/demo/objects/{f.Payload}.pkg");
        part.Headers.Range = new System.Net.Http.Headers.RangeHeaderValue(3, 5);
        Assert.Equal(HttpStatusCode.PartialContent, (await c.SendAsync(part)).StatusCode);
        Assert.Equal(before + 1, await Count());
    }

    [Fact]
    public async Task Feeds_are_atom_newest_first()
    {
        var c = f.CreateClient();
        foreach (var url in new[] { "/feed", "/channels/demo/feed" })
        {
            var r = await c.GetAsync(url);
            Assert.Equal("application/atom+xml", r.Content.Headers.ContentType?.MediaType);
            var x = XDocument.Parse(await r.Content.ReadAsStringAsync());
            XNamespace a = "http://www.w3.org/2005/Atom";
            var titles = x.Root!.Elements(a + "entry").Select(e => e.Element(a + "title")!.Value).ToList();
            Assert.Equal(["sdltool 1.1 (x86_64)", "sdltool 1.0 (x86_64)"], titles);
        }
        Assert.Equal(HttpStatusCode.NotFound, (await c.GetAsync("/channels/nope/feed")).StatusCode);
    }

    [Fact]
    public async Task A_badge_is_svg_with_the_latest_version_and_escapes_what_it_shows()
    {
        var c = f.CreateClient();
        var r = await c.GetAsync("/badge/demo/sdltool.svg");
        Assert.Equal("image/svg+xml", r.Content.Headers.ContentType?.MediaType);
        var svg = XDocument.Parse(await r.Content.ReadAsStringAsync());
        Assert.Contains("sdltool: 1.1", svg.Root!.Value);
        Assert.Equal(HttpStatusCode.NotFound, (await c.GetAsync("/badge/demo/missing.svg")).StatusCode);
        Assert.Contains("a&amp;b", Portal.Api.Endpoints.Badge("a&b", "<1>"));
        Assert.DoesNotContain("<1>", Portal.Api.Endpoints.Badge("a&b", "<1>"));
    }
}
