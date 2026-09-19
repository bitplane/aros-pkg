// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Net;
using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Microsoft.AspNetCore.Mvc.Testing;
using Portal.Admin;
using Portal.Push;

namespace Portal.Tests;

/// Each rule of Portal:Policy refuses with its name, its value and the
/// operators' note; and the review's findings stay fixed.
public class PolicyTests
{
    sealed class Site : WebApplicationFactory<Program>
    {
        public readonly string Data = Directory.CreateTempSubdirectory("portal-policy-").FullName;
        public readonly string Alice, Bob, Admin;
        readonly Dictionary<string, string> settings;

        public Site(Dictionary<string, string>? extra = null)
        {
            string a, b, adm;
            (Alice, a) = PublisherKeys.Create("alice", "*", files: true);
            (Bob, b) = PublisherKeys.Create("bob", "*", files: true);
            (Admin, adm) = AdminKeys.Create("owner");
            settings = new() { ["Portal:DataDir"] = Data, ["Portal:Keys"] = $"{a};{b}", ["Portal:AdminKeys"] = adm,
                               ["Portal:Policy:Note"] = "Links only here." };
            foreach (var (k, v) in extra ?? []) settings[k] = v;
        }

        protected override void ConfigureWebHost(Microsoft.AspNetCore.Hosting.IWebHostBuilder builder)
        {
            foreach (var (k, v) in settings) builder.UseSetting(k, v);
        }

        public async Task<(HttpStatusCode, string)> Send(HttpMethod m, string url, string key, string body = "")
        {
            var c = CreateClient(new WebApplicationFactoryClientOptions { BaseAddress = new Uri("https://localhost") });
            var req = new HttpRequestMessage(m, url) { Content = new StringContent(body) };
            req.Headers.Authorization = new AuthenticationHeaderValue("Bearer", key);
            var r = await c.SendAsync(req);
            return (r.StatusCode, await r.Content.ReadAsStringAsync());
        }

        /// A version written straight into a channel, as a commit leaves it.
        public string Publish(string channel, string manifest, string signer)
        {
            var ch = Path.Combine(Data, "channels", channel, "objects");
            Directory.CreateDirectory(ch);
            var d = Hex(manifest);
            File.WriteAllText(Path.Combine(ch, d + ".manifest"), manifest);
            File.WriteAllText(Path.Combine(ch, d + ".sig"), $"Signer: {signer}\nSignature: {new string('0', 128)}\n");
            var f = manifest.Split('\n');
            string V(string k) => f.First(l => l.StartsWith(k + ": "))[(k.Length + 2)..];
            File.AppendAllText(Path.Combine(Data, "channels", channel, "index"), $"{V("Name")} {V("Version")} {V("Architecture")} {d}\n");
            return d;
        }
    }

    static string Hex(string s) => Convert.ToHexString(SHA256.HashData(Encoding.ASCII.GetBytes(s))).ToLowerInvariant();

    /// Plan, upload and commit one manifest and its signature.
    static async Task<string> Push(Site s, string channel, string key, string manifest, string signer)
    {
        var d = Hex(manifest);
        var sig = $"Signer: {signer}\nSignature: {new string('2', 128)}\n";
        await s.Send(HttpMethod.Post, $"/{channel}/_push/plan", key, $"objects/{d}.manifest {d} {manifest.Length}\nobjects/{d}.sig {Hex(sig)} {sig.Length}\n");
        await s.Send(HttpMethod.Put, $"/{channel}/_push/files/objects/{d}.manifest", key, manifest);
        await s.Send(HttpMethod.Put, $"/{channel}/_push/files/objects/{d}.sig", key, sig);
        var f = manifest.Split('\n');
        string V(string k) => f.First(l => l.StartsWith(k + ": "))[(k.Length + 2)..];
        return (await s.Send(HttpMethod.Post, $"/{channel}/_push/commit", key, $"{V("Name")} {V("Version")} {V("Architecture")} {d}\n")).Item2;
    }

    const string K1 = "1111111111111111111111111111111111111111111111111111111111111111";

    [Fact]
    public async Task Uploads_off_says_so_with_the_rule_and_the_note()
    {
        using var s = new Site(new() { ["Portal:Policy:Push"] = "false" });
        var (status, body) = await s.Send(HttpMethod.Post, "/any/_push/plan", s.Alice);
        Assert.Equal(HttpStatusCode.Forbidden, status);
        Assert.Contains("policy: Push=off", body);
        Assert.Contains("note: Links only here.", body);
    }

    [Fact]
    public async Task Binaries_off_refuses_even_a_key_with_the_files_right()
    {
        using var s = new Site(new() { ["Portal:Policy:Binaries"] = "off" });
        var h = new string('a', 64);
        var (_, body) = await s.Send(HttpMethod.Post, "/any/_push/plan", s.Alice, $"objects/{h}.pkg {h} 1\n");
        Assert.Contains("rule Binaries=off", body);
    }

    [Fact]
    public async Task An_archive_on_a_host_outside_LinkHosts_is_refused()
    {
        using var s = new Site(new() { ["Portal:Policy:LinkHosts"] = "github.com" });
        var m = "Format: pkg-manifest 1\nName: tool\nVersion: 1.0\nArchitecture: generic\nKind: data\n" +
                $"Source: t.tar.bz2!/t\nArchive: {new string('b', 64)} 10 https://example.org/t.tar.bz2\n";
        Assert.Contains("rule LinkHosts=github.com", await Push(s, "any", s.Alice, m, K1));
    }

    [Fact]
    public async Task No_new_channels_and_no_admin_are_said_as_rules()
    {
        using var s = new Site(new() { ["Portal:Policy:NewChannels"] = "false", ["Portal:Policy:Admin"] = "false" });
        Assert.Contains("policy: NewChannels=off", (await s.Send(HttpMethod.Post, "/fresh/_push/plan", s.Alice)).Item2);
        Assert.Contains("policy: Admin=off", (await s.Send(HttpMethod.Get, "/_admin/log", s.Admin)).Item2);
    }

    [Fact]
    public async Task The_policy_is_readable_as_json()
    {
        using var s = new Site(new() { ["Portal:Policy:Binaries"] = "off", ["Portal:Policy:LinkHosts"] = "github.com, sourceforge.net" });
        using var d = JsonDocument.Parse(await s.CreateClient().GetStringAsync("/api/policy"));
        Assert.Equal("off", d.RootElement.GetProperty("binaries").GetString());
        Assert.Equal(["github.com", "sourceforge.net"], d.RootElement.GetProperty("linkHosts").EnumerateArray().Select(e => e.GetString()));
        Assert.Equal("Links only here.", d.RootElement.GetProperty("note").GetString());
    }

    [Fact]
    public async Task An_archive_name_that_leaves_its_folder_is_refused()
    {
        using var s = new Site();
        var m = "Format: pkg-manifest 1\nName: tool\nVersion: 1.0\nArchitecture: generic\nKind: data\n" +
                $"Source: ../../../escape.tar!/t\nArchive: {new string('b', 64)} 10 https://example.org/t.tar\n";
        var body = await Push(s, "any", s.Alice, m, K1);
        Assert.Contains("not a plain file name", body);
        Assert.False(File.Exists(Path.Combine(s.Data, "escape.tar.sha256")));
    }

    [Fact]
    public async Task A_name_differing_only_in_case_is_refused()
    {
        using var s = new Site();
        s.Publish("any", "Format: pkg-manifest 1\nName: regina\nVersion: 1.0\nArchitecture: generic\nKind: data\n" +
                         $"Source: r.tar!/r\nArchive: {new string('c', 64)} 10 https://example.org/r.tar\n", K1);
        var m = "Format: pkg-manifest 1\nName: Regina\nVersion: 2.0\nArchitecture: generic\nKind: data\n" +
                $"Source: r.tar!/r\nArchive: {new string('c', 64)} 10 https://example.org/r.tar\n";
        var body = await Push(s, "any", s.Bob, m, new string('2', 64));
        Assert.Contains("differ from another only in case", body);
    }

    [Fact]
    public async Task Channel_files_belong_to_the_key_that_published_them_first()
    {
        using var s = new Site();
        var state = Path.Combine(s.Data, "state", "any");
        Directory.CreateDirectory(state);
        File.WriteAllText(Path.Combine(state, "channel-files-owner"), "alice\n");
        var h = new string('d', 64);
        var (_, body) = await s.Send(HttpMethod.Post, "/any/_push/plan", s.Bob, $"Bootstrap/x86_64/Pkg {h} 5\n");
        Assert.Contains("belong to the key of alice", body);
        var (_, mine) = await s.Send(HttpMethod.Post, "/any/_push/plan", s.Alice, $"Bootstrap/x86_64/Pkg {h} 5\n");
        Assert.Contains("need: Bootstrap/x86_64/Pkg", mine);
    }
}
