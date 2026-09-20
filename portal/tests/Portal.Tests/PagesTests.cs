// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Net;
using System.Net.Http.Headers;
using System.Security.Claims;
using System.Security.Cryptography;
using System.Text;
using System.Text.Encodings.Web;
using Microsoft.AspNetCore.Authentication;
using Microsoft.AspNetCore.Mvc.Testing;
using Microsoft.AspNetCore.TestHost;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using Portal.Admin;
using Portal.Channels;
using Portal.Push;

namespace Portal.Tests;

/// What the pages themselves must do: mark what a search matched, search the
/// guides, and keep a maintainer from shutting their own account out.
public class PagesTests
{
    sealed class Site : WebApplicationFactory<Program>
    {
        public readonly string Data = Directory.CreateTempSubdirectory("portal-pages-").FullName;
        public readonly string AdminKey;
        readonly string adminConfig;

        public Site()
        {
            (AdminKey, adminConfig) = AdminKeys.Create("owner");
            var ch = Path.Combine(Data, "channels", "demo", "objects");
            Directory.CreateDirectory(ch);
            var m = "Format: pkg-manifest 1\nName: sdltool\nVersion: 1.0\nArchitecture: x86_64\nKind: library\n" +
                    "Short: Plays with SDL & <sound>\nTags: games\n";
            var d = Convert.ToHexString(SHA256.HashData(Encoding.ASCII.GetBytes(m))).ToLowerInvariant();
            File.WriteAllText(Path.Combine(ch, d + ".manifest"), m);
            File.WriteAllText(Path.Combine(ch, d + ".sig"), $"Signer: {new string('a', 64)}\nSignature: {new string('0', 128)}\n");
            File.WriteAllText(Path.Combine(Data, "channels", "demo", "index"), $"sdltool 1.0 x86_64 {d}\n");
        }

        protected override void ConfigureWebHost(Microsoft.AspNetCore.Hosting.IWebHostBuilder b)
        {
            b.UseSetting("Portal:DataDir", Data);
            b.UseSetting("Portal:AdminKeys", adminConfig);
            b.UseSetting("Portal:GitHub:ClientId", "dev");
            b.UseSetting("Portal:GitHub:ClientSecret", "dev");
            b.UseSetting("Portal:Admins", "jonx");
            // Signed in as the maintainer, without GitHub: the test server has no
            // loopback address, so the site's own development sign-in cannot apply.
            b.ConfigureTestServices(services => services.AddAuthentication(o =>
            {
                o.DefaultAuthenticateScheme = "Test";
                o.DefaultChallengeScheme = "Test";
            }).AddScheme<AuthenticationSchemeOptions, AsJonx>("Test", null));
        }
        public HttpClient Https() => CreateClient(new WebApplicationFactoryClientOptions { BaseAddress = new Uri("https://localhost") });
    }

    /// Every request comes from the GitHub account "jonx", number 1.
    sealed class AsJonx(IOptionsMonitor<AuthenticationSchemeOptions> o, ILoggerFactory l, UrlEncoder e)
        : AuthenticationHandler<AuthenticationSchemeOptions>(o, l, e)
    {
        protected override Task<AuthenticateResult> HandleAuthenticateAsync()
        {
            var who = new ClaimsIdentity([new(ClaimTypes.NameIdentifier, "1"), new(ClaimTypes.Name, "jonx")], "Test");
            return Task.FromResult(AuthenticateResult.Success(new AuthenticationTicket(new ClaimsPrincipal(who), "Test")));
        }
    }

    [Fact]
    public async Task A_search_marks_what_it_matched_and_never_lets_a_package_carry_markup()
    {
        using var f = new Site();
        var page = await f.Https().GetStringAsync("/search?q=sdl");
        Assert.Contains("<mark>sdl</mark>tool", page);                   // in the name
        Assert.Contains("Plays with <mark>SDL</mark>", page);            // and in the description
        Assert.Contains("&amp; &lt;sound&gt;", page);                    // the package's own text stays text
        Assert.DoesNotContain("<sound>", page);
    }

    [Fact]
    public async Task The_guides_are_searchable_and_a_result_leads_to_the_word()
    {
        using var f = new Site();
        var c = f.Https();
        var page = await c.GetStringAsync("/docs?q=keygen");
        Assert.Contains("result", page);
        Assert.Contains("<mark>", page);                                  // the snippet is marked
        Assert.Contains("href=\"/docs/", page);                           // and leads to a guide
        var none = await c.GetStringAsync("/docs?q=zzzznothinglikethis");
        Assert.Contains("0 result", none);
        Assert.Contains("Nothing in the guides holds that", none);
    }

    [Fact]
    public async Task A_maintainer_cannot_suspend_their_own_account()
    {
        using var f = new Site();
        var registry = (Portal.Accounts.Registry)f.Services.GetService(typeof(Portal.Accounts.Registry))!;
        Assert.Null(registry.Register(1, "jonx", "JKN", new string('1', 64), "mine"));
        Assert.Null(registry.Register(2, "jane", "Jane", new string('2', 64), "hers"));
        var c = f.CreateClient(new WebApplicationFactoryClientOptions { BaseAddress = new Uri("https://localhost"), HandleCookies = true });
        // through the form itself, token and all: a post without it is refused, as it should be
        async Task<string> Act(long id, string what)
        {
            var page = await c.GetStringAsync("/admin");
            var at = page.IndexOf("__RequestVerificationToken", StringComparison.Ordinal);
            var from = page.IndexOf("value=\"", at, StringComparison.Ordinal) + 7;
            var token = page[from..page.IndexOf('"', from)];
            var r = await c.PostAsync("/admin", new FormUrlEncodedContent(
                [new("what", what), new("account", id.ToString()), new("__RequestVerificationToken", token)]));
            return await r.Content.ReadAsStringAsync();
        }
        Assert.Contains("You cannot suspend your own account", await Act(1, "suspend"));
        Assert.False(registry.ByAccount(1)!.Suspended);
        Assert.Contains("Suspended", await Act(2, "suspend"));            // another account still can be
        Assert.True(registry.ByAccount(2)!.Suspended);
    }
}
