// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Globalization;
using System.Net;
using System.Threading.RateLimiting;
using Microsoft.AspNetCore.HttpOverrides;
using Microsoft.Extensions.Options;
using Microsoft.Net.Http.Headers;
using Portal;
using Portal.Channels;
using Portal.Push;

// `Portal key <publisher> <channel,channel|*>` prints a new push key and the
// line that configures it, then exits. Only the line goes into the settings.
// Add "files" to let the key upload binaries; without it, it publishes by link only.
if (args.Length is 3 or 4 && args[0] == "key" && (args.Length == 3 || args[3] == "files"))
{
    var (key, config) = PublisherKeys.Create(args[1], args[2], args.Length == 4);
    Console.WriteLine($"key:    {key}");
    Console.WriteLine($"config: {config}");
    Console.WriteLine("summary: give the key to the publisher once; add the config line to Portal:Keys (entries separated by ';')");
    return;
}

// `Portal viewkey <name>`: a key that shows unlisted channels in a browser, and its config line.
if (args is ["viewkey", var viewer])
{
    var k = Convert.ToHexString(System.Security.Cryptography.RandomNumberGenerator.GetBytes(24)).ToLowerInvariant();
    var h = Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(System.Text.Encoding.UTF8.GetBytes(k))).ToLowerInvariant();
    Console.WriteLine($"key (shown once; open <portal>/see/<key> in the browser): {k}\nPortal:ViewKeys entry: {viewer}:{h}");
    return;
}
// `Portal adminkey <name>`: a maintainer's key for /_admin, and its config line.
if (args is ["adminkey", var maintainer])
{
    var (key, config) = Portal.Admin.AdminKeys.Create(maintainer);
    Console.WriteLine($"key:    {key}");
    Console.WriteLine($"config: {config}");
    Console.WriteLine("summary: keep the key (PKG_ADMINKEY); add the config line to Portal:AdminKeys (entries separated by ';')");
    return;
}

// Records are read by machines: numbers never follow the server's locale.
CultureInfo.DefaultThreadCurrentCulture = CultureInfo.DefaultThreadCurrentUICulture = CultureInfo.InvariantCulture;

var builder = WebApplication.CreateBuilder(args);
builder.Services.Configure<PortalOptions>(builder.Configuration.GetSection("Portal"));
builder.Services.AddSingleton<Catalogue>();
builder.Services.AddSingleton<PublisherKeys>();
builder.Services.AddSingleton<SignedPush>();
builder.Services.AddSingleton<Portal.Accounts.Registry>();
builder.Services.AddSingleton<Portal.Channels.Usage>();
builder.Services.AddHostedService(sp => sp.GetRequiredService<Portal.Channels.Usage>());

// "Sign in with GitHub": who a publisher is. The portal reads the account's
// number and login from the public profile, asks for no scope, keeps no token.
{
    var gh = builder.Configuration.GetSection("Portal:GitHub");
    var auth = builder.Services.AddAuthentication(Microsoft.AspNetCore.Authentication.Cookies.CookieAuthenticationDefaults.AuthenticationScheme)
        .AddCookie(c =>
        {
            c.Cookie.Name = "pkg-account";
            c.Cookie.HttpOnly = true;
            c.Cookie.SameSite = Microsoft.AspNetCore.Http.SameSiteMode.Lax;
            c.ExpireTimeSpan = TimeSpan.FromDays(30);
            c.SlidingExpiration = true;
            c.LoginPath = "/account";
            c.AccessDeniedPath = "/account";
        });
    if (!string.IsNullOrEmpty(gh["ClientId"]) && !string.IsNullOrEmpty(gh["ClientSecret"]))
        auth.AddOAuth("GitHub", g =>
        {
            g.ClientId = gh["ClientId"]!;
            g.ClientSecret = gh["ClientSecret"]!;
            g.CallbackPath = "/signin-github";
            g.AuthorizationEndpoint = "https://github.com/login/oauth/authorize";
            g.TokenEndpoint = "https://github.com/login/oauth/access_token";
            g.UserInformationEndpoint = "https://api.github.com/user";
            g.SaveTokens = false;
            g.CorrelationCookie.SameSite = Microsoft.AspNetCore.Http.SameSiteMode.Lax;
            g.Events.OnCreatingTicket = async ctx =>
            {
                using var req = new HttpRequestMessage(HttpMethod.Get, ctx.Options.UserInformationEndpoint);
                req.Headers.Authorization = new System.Net.Http.Headers.AuthenticationHeaderValue("Bearer", ctx.AccessToken);
                req.Headers.UserAgent.ParseAdd("AROS-Packages-portal");
                req.Headers.Accept.ParseAdd("application/vnd.github+json");
                using var res = await ctx.Backchannel.SendAsync(req, ctx.HttpContext.RequestAborted);
                res.EnsureSuccessStatusCode();
                using var user = System.Text.Json.JsonDocument.Parse(await res.Content.ReadAsStringAsync(ctx.HttpContext.RequestAborted));
                var id = user.RootElement.GetProperty("id").GetInt64().ToString(System.Globalization.CultureInfo.InvariantCulture);
                var login = user.RootElement.GetProperty("login").GetString() ?? "";
                // The account's number and its login: all the portal ever keeps of it.
                ctx.Identity!.AddClaim(new System.Security.Claims.Claim(System.Security.Claims.ClaimTypes.NameIdentifier, id));
                ctx.Identity.AddClaim(new System.Security.Claims.Claim(System.Security.Claims.ClaimTypes.Name, login));
            };
        });
    builder.Services.AddAuthorization();
}
builder.Services.AddHttpContextAccessor();
builder.Services.AddSingleton<PkgRunner>();
builder.Services.AddSingleton<PushService>();
builder.Services.AddSingleton<ArchiveChecker>();
builder.Services.AddSingleton<ArchiveStore>();
builder.Services.AddSingleton<Portal.Channels.Publishers>();
builder.Services.AddSingleton<Portal.Channels.Search>();
builder.Services.AddSingleton<Portal.Admin.AdminKeys>();
builder.Services.AddSingleton<Portal.Admin.AdminService>();
builder.Services.AddSingleton<Portal.Channels.Downloads>();
builder.Services.AddHostedService(sp => sp.GetRequiredService<Portal.Channels.Downloads>());
builder.Services.AddHttpClient("r2", c => c.Timeout = TimeSpan.FromHours(1));
builder.Services.AddHostedService(sp => sp.GetRequiredService<ArchiveChecker>());
builder.Services.AddHostedService<Portal.Channels.WithdrawalsAtStart>();
builder.Services.AddRazorPages();
// Per address, so one client cannot drown the site: pushes send one request per
// file (a whole contrib nightly is about 210), admin and search much fewer.
builder.Services.AddRateLimiter(r =>
{
    r.RejectionStatusCode = StatusCodes.Status429TooManyRequests;
    static RateLimitPartition<string> Per(HttpContext c, int permits) =>
        RateLimitPartition.GetFixedWindowLimiter(c.Connection.RemoteIpAddress?.ToString() ?? "?",
            _ => new FixedWindowRateLimiterOptions { PermitLimit = permits, Window = TimeSpan.FromMinutes(1) });
    r.AddPolicy("push", c => Per(c, 1200));
    r.AddPolicy("admin", c => Per(c, 60));
    r.AddPolicy("api", c => Per(c, 300));
    // Reading a channel is counted differently. Pkg asks for one file at a
    // time on one connection, and a whole channel is hundreds of files in a
    // minute, so a count per minute would refuse the honest reader first. What
    // this machine cannot take is many files at once, so the limit is how many
    // an address may have in flight; the rest wait their turn and are served.
    r.AddPolicy("reads", c => RateLimitPartition.GetConcurrencyLimiter(
        c.Connection.RemoteIpAddress?.ToString() ?? "?",
        _ => new ConcurrencyLimiterOptions
        {
            PermitLimit = 8,
            QueueLimit = 64,
            QueueProcessingOrder = QueueProcessingOrder.OldestFirst,
        }));
    r.OnRejected = async (ctx, ct) =>
    {
        ctx.HttpContext.Response.ContentType = "text/plain; charset=utf-8";
        // A machine is told when to come back, in the header and in the record.
        ctx.HttpContext.Response.Headers.RetryAfter = "60";
        await ctx.HttpContext.Response.WriteAsync(Record.Refused(20, "too many requests from this address", "wait a minute and try again").ToString(), ct);
    };
});
builder.Services.Configure<ForwardedHeadersOptions>(f =>
{
    // App Service terminates TLS in front of the app and says so in X-Forwarded-Proto.
    f.ForwardedHeaders = ForwardedHeaders.XForwardedFor | ForwardedHeaders.XForwardedProto;
    f.KnownIPNetworks.Clear();
    f.KnownProxies.Clear();
    // Only the last hop, the one Azure's front end adds; what a client sends before it is ignored.
    f.ForwardLimit = 1;
});
builder.WebHost.ConfigureKestrel((ctx, k) =>
    k.Limits.MaxRequestBodySize = (ctx.Configuration.GetValue<long?>("Portal:MaxPartBytes") ?? 40L * 1024 * 1024) + 1024 * 1024);

var app = builder.Build();
var opts = app.Services.GetRequiredService<IOptions<PortalOptions>>().Value;
Directory.CreateDirectory(opts.ChannelsDir);
// A zip deployment keeps no mode bits: give the bundled Pkg back its execute bit.
if (!OperatingSystem.IsWindows() && File.Exists(opts.PkgPath))
    File.SetUnixFileMode(opts.PkgPath, File.GetUnixFileMode(opts.PkgPath)
        | UnixFileMode.UserExecute | UnixFileMode.GroupExecute | UnixFileMode.OtherExecute);

app.UseForwardedHeaders();
// Behind App Service, X-Forwarded-Proto comes through as the client sent it, so
// it proves nothing: the front end marks a request that really arrived over TLS
// with X-ARR-SSL. There, that mark alone decides whether a request is https.
if (Environment.GetEnvironmentVariable("WEBSITE_SITE_NAME") is not null)
    app.Use(async (ctx, next) =>
    {
        ctx.Request.Scheme = ctx.Request.Headers.ContainsKey("X-ARR-SSL") ? "https" : "http";
        await next();
    });
// A machine asks in records and is answered in records, whatever happens: an
// unexpected failure says so in Pkg's own form and is never an empty body. What
// went wrong is kept for the maintainers (/_admin/failures), with nothing about
// who asked. The pages keep the error page.
static bool ForMachines(PathString path) =>
    path.StartsWithSegments("/api") || path.Value is { } v
    && (v.Contains("/_push/", StringComparison.Ordinal) || v.StartsWith("/_admin", StringComparison.Ordinal));
app.UseWhen(ctx => ForMachines(ctx.Request.Path), branch => branch.UseExceptionHandler(err => err.Run(async ctx =>
{
    var ex = ctx.Features.Get<Microsoft.AspNetCore.Diagnostics.IExceptionHandlerFeature>()?.Error;
    Portal.Push.Failures.Note(opts.StateDir, $"{ctx.Request.Method} {ctx.Request.Path}", ex);
    ctx.Response.StatusCode = 500;
    ctx.Response.ContentType = "text/plain; charset=utf-8";
    await ctx.Response.WriteAsync(Record.Refused(17,
        $"the portal could not finish this request ({ex?.GetType().Name ?? "unknown"})",
        "try again; if it happens again, tell the portal's maintainers, who can read what went wrong").ToString());
})));
if (!app.Environment.IsDevelopment()) app.UseWhen(ctx => !ForMachines(ctx.Request.Path), b => b.UseExceptionHandler("/Error"));

// Unlisted: nothing here is to be indexed. No HTTPS redirect and no HSTS,
// because classic 68k clients speak plain HTTP; signatures carry integrity.
app.Use(async (ctx, next) =>
{
    ctx.Response.Headers["X-Robots-Tag"] = "noindex, nofollow";
    ctx.Response.Headers.XContentTypeOptions = "nosniff";
    ctx.Response.Headers["Referrer-Policy"] = "same-origin";
    // Pages run only this site's own script, and no other site may frame them.
    ctx.Response.Headers.ContentSecurityPolicy =
        "default-src 'self'; img-src 'self' data:; style-src 'self' 'unsafe-inline'; script-src 'self'; " +
        "object-src 'none'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'";
    ctx.Response.Headers.XFrameOptions = "DENY";
    await next();
});
// AROS speaks https since Pkg 1.6, so plain http is left open for one thing:
// letting a Pkg from before it, which has no TLS, reach the version that does.
// That is the channel Pkg comes from and the scripts that install it; anything
// else asked over http is sent to this portal's https address. A Pkg too old to
// follow is told so in its own form rather than left with a broken connection.
// Only a portal that knows its own https address does any of this: one served
// over plain http alone, on a desk or a LAN, is left exactly as it is.
if (opts.PublicUrl.StartsWith("https:", StringComparison.OrdinalIgnoreCase))
{
    var home = opts.PublicUrl.TrimEnd('/');
    var pkgChannel = opts.Pinned.Split(',', ';')[0].Split('/')[0].Trim();
    bool TheWayUp(PathString path) =>
        opts.Policy.PlainHttp
        && (path.StartsWithSegments("/" + pkgChannel) || path.StartsWithSegments("/get/" + pkgChannel)
            || path.Value is "/Get-Pkg" or "/install" or "/install.sh" or "/install.ps1" or "/health" or "/robots.txt");
    app.Use(async (ctx, next) =>
    {
        // What carries a key refuses on its own, and says so: sending it on to
        // https would not unsay the key that already travelled in the clear.
        var carriesAKey = ctx.Request.Path.Value is { } p2
            && (p2.Contains("/_push/", StringComparison.Ordinal) || p2.StartsWith("/_admin", StringComparison.Ordinal));
        if (ctx.Request.IsHttps || carriesAKey || TheWayUp(ctx.Request.Path)) { await next(); return; }
        var ua = ctx.Request.Headers.UserAgent.ToString();
        var version = ua.StartsWith("Pkg/", StringComparison.Ordinal) ? ua[4..].Split(' ')[0] : ua.StartsWith("Pkg", StringComparison.Ordinal) ? "0" : null;
        if (version is null || Portal.Channels.PkgVersion.Order.Compare(version, "1.6") >= 0)
        {
            // A browser, and every Pkg that speaks https, simply goes there.
            ctx.Response.Redirect($"{home}{ctx.Request.PathBase}{ctx.Request.Path}{ctx.Request.QueryString}", permanent: true, preserveMethod: true);
            return;
        }
        var plain = "http://" + home[(home.IndexOf("://", StringComparison.Ordinal) + 3)..];
        ctx.Response.StatusCode = 426;
        ctx.Response.ContentType = "text/plain; charset=utf-8";
        await ctx.Response.WriteAsync(Record.Refused(20,
            $"this portal serves its channels over https, and this is {(version == "0" ? "a Pkg older than 1.5" : "Pkg " + version)}, which has no way to speak it",
            $"upgrade first, which plain http stays open for: Pkg UPGRADE pkg ROOT SYS: CHANNEL {plain}/{pkgChannel}, "
            + $"then read this channel at {home}").ToString());
    });
}
// Portal:Policy:MinPkg: an older Pkg is told to update, in its own record form.
if (opts.Policy.MinPkg.Trim().Length > 0)
    app.Use(async (ctx, next) =>
    {
        var min = opts.Policy.MinPkg.Trim();
        var path = ctx.Request.Path.Value ?? "/";
        var ua = ctx.Request.Headers.UserAgent.ToString().Trim();
        var pkgChannel = "/" + opts.Pinned.Split(',', ';')[0].Split('/')[0].Trim() + "/";
        var isPush = path.Contains("/_push/", StringComparison.Ordinal);
        var isPkg = ua == "Pkg" || ua.StartsWith("Pkg/", StringComparison.Ordinal);
        var version = ua.StartsWith("Pkg/", StringComparison.Ordinal) ? ua[4..].Split(' ')[0] : "0";
        var old = Portal.Channels.PkgVersion.Order.Compare(version, min) < 0;
        var judged = isPush ? !ua.StartsWith("Pkg-tools/", StringComparison.Ordinal) && (!isPkg || old)
                            : opts.Policy.MinPkgReads && isPkg && old
                              && !path.StartsWith(pkgChannel, StringComparison.OrdinalIgnoreCase);
        if (!judged) { await next(); return; }
        var site = opts.PublicUrl.Length > 0 ? opts.PublicUrl.TrimEnd('/') : $"{ctx.Request.Scheme}://{ctx.Request.Host}";
        var said = isPkg ? (version == "0" ? "a Pkg older than 1.5" : $"Pkg {version}") : "a program that does not say it is Pkg";
        var r = opts.Policy.Refuse("MinPkg", min, $"this portal works with Pkg {min} or later, and this is {said}",
            $"update Pkg: pkg UPGRADE pkg ROOT <root> CHANNEL {site}{pkgChannel.TrimEnd('/')} (http:// on AROS), or {site}/downloads");
        ctx.Response.StatusCode = 426;
        ctx.Response.ContentType = "text/plain; charset=utf-8";
        await ctx.Response.WriteAsync(r.ToString());
    });
app.UseStaticFiles();
// Routing after static files: the channel route would otherwise claim /css/site.css.
app.UseRouting();
app.UseRateLimiter();
app.UseAuthentication();
// Development only, from this machine only: ?dev-as=<login>&dev-id=<n> makes this one request a
// signed-in one, so that tools/shot.sh can look at what a signed-in person sees.
if (app.Environment.IsDevelopment())
    app.Use(async (ctx, next) =>
    {
        if (ctx.Request.Query["dev-as"].ToString() is { Length: > 0 } login && long.TryParse(ctx.Request.Query["dev-id"], out var id)
            && ctx.Connection.RemoteIpAddress is { } ip && IPAddress.IsLoopback(ip))
            ctx.User = new System.Security.Claims.ClaimsPrincipal(new System.Security.Claims.ClaimsIdentity(
                [new(System.Security.Claims.ClaimTypes.NameIdentifier, id.ToString(System.Globalization.CultureInfo.InvariantCulture)),
                 new(System.Security.Claims.ClaimTypes.Name, login)], "dev"));
        await next();
    });
app.UseAuthorization();
// A page that fails, on demand and from this machine only, so that
// tests/failures.sh can see what a failure leaves behind for the maintainers.
// The address exists only when the portal is started with PORTAL_TEST_FAILURE=1,
// which a deployed one never is.
if (Environment.GetEnvironmentVariable("PORTAL_TEST_FAILURE") == "1")
    app.MapGet("/_test/fail", (HttpContext http) =>
        http.Connection.RemoteIpAddress is { } ip && IPAddress.IsLoopback(ip)
            ? throw new InvalidOperationException("a failure this portal was asked for")
            : Results.NotFound());
app.MapGet("/robots.txt", () => Results.Text("User-agent: *\nDisallow: /\n"));
app.MapGet("/health", (Catalogue c) => Results.Text($"ok: {c.ChannelNames().Count()} channels\n"));
app.MapRazorPages();

// ---- the maintainers' API -----------------------------------------------------

var admin = app.MapGroup("/_admin").RequireRateLimiting("admin").AddEndpointFilter(async (ctx, next) =>
{
    var http = ctx.HttpContext;
    if (!opts.Policy.Admin)
        return Results2.Text(opts.Policy.Refuse("Admin", "off", "the maintainers' API is switched off on this portal", "change it on the server, where the portal's settings are"), 403);
    var loopback = http.Connection.RemoteIpAddress is { } ip && IPAddress.IsLoopback(ip);
    if (!http.Request.IsHttps && !(opts.AllowLoopbackHttpPush && loopback))
        return Results2.Text(Record.Refused(20, "the admin API needs https: its key must never travel in clear", "use the https address"), 403);
    var who = http.RequestServices.GetRequiredService<Portal.Admin.AdminKeys>().Find(http.Request.Headers.Authorization);
    if (who is null)
        return Results2.Text(Record.Refused(14, "no admin key, or one the portal does not know", "set PKG_ADMINKEY to a maintainer's key"), 401);
    http.Items["admin"] = who;
    return await next(ctx);
});

admin.MapPost("/channels/{channel}/remove", async (HttpContext http, string channel, Portal.Admin.AdminService s) =>
    !ChannelPaths.IsChannelName(channel)
        ? Results2.Text(Record.Refused(20, $"'{channel}' is not a channel name", "check the address"), 400)
        : Results2.Text(await s.Remove((string)http.Items["admin"]!, channel, await ReadBody(http),
        http.Request.Query["dryrun"] is var d && (d == "1" || d == "true"), http.RequestAborted)));

foreach (var (verb, listed) in new[] { ("unlist", false), ("list", true) })
    admin.MapPost($"/channels/{{channel}}/{verb}", async (HttpContext http, string channel, Portal.Admin.AdminService s) =>
        !ChannelPaths.IsChannelName(channel)
            ? Results2.Text(Record.Refused(20, $"'{channel}' is not a channel name", "check the address"), 400)
            : Results2.Text(await s.SetListed((string)http.Items["admin"]!, channel, listed, http.RequestAborted)));

admin.MapPost("/restore/{stamp}", async (HttpContext http, string stamp, Portal.Admin.AdminService s) =>
    Results2.Text(await s.Restore((string)http.Items["admin"]!, stamp, http.RequestAborted)));

admin.MapGet("/log", (Portal.Admin.AdminService s) => Results2.Text(s.ReadLog()));

// What the portal failed to answer, and why: for whoever has to find out.
admin.MapGet("/failures", () => Results2.Text(Portal.Push.Failures.Read(opts.StateDir)));

// ---- the push API, under each channel ---------------------------------------

// Who may push is decided by people, and a refusal says where to ask them.
string SiteOf(HttpContext http) => opts.PublicUrl.Length > 0 ? opts.PublicUrl.TrimEnd('/') : $"{http.Request.Scheme}://{http.Request.Host}";
string Ask(HttpContext http) => opts.GitHub.On
    ? $"A publisher registers their signing key at {SiteOf(http)}/account, signed in with GitHub: {SiteOf(http)}/publishers says how"
    : $"Publishers are registered by this portal's maintainers, and nobody can register themselves yet: {SiteOf(http)}/publishers says how to ask";

var push = app.MapGroup("/{channel}/_push").RequireRateLimiting("push").AddEndpointFilter(async (ctx, next) =>
{
    var http = ctx.HttpContext;
    var channel = (string)http.GetRouteValue("channel")!;
    if (!opts.Policy.Push)
        return Results2.Text(opts.Policy.Refuse("Push", "off", "this portal accepts no uploads: it serves its channels read-only",
            "ask the portal's operators where to publish"), 403);
    if (!ChannelPaths.IsChannelName(channel))
        return Results2.Text(Record.Refused(20, $"'{channel}' is not a channel name: lowercase letters, digits and '-'", "check the address"), 400);
    var signed = http.RequestServices.GetRequiredService<SignedPush>();
    var opening = http.Request.Path.Value!.EndsWith("/_push/session", StringComparison.Ordinal);
    Publisher? who;
    if (opening || SignedPush.IsSigned(http.Request.Headers.Authorization))
    {
        // Signed requests carry nothing secret, so they may come over plain http.
        if (!opts.Policy.SignedPush)
            return Results2.Text(opts.Policy.Refuse("SignedPush", "off", "this portal takes no signed pushes", "push over https with the key the portal gave you"), 403);
        if (opening) return await next(ctx);
        var (publisher, refusal, body) = await signed.Check(http, Ask(http), http.RequestAborted);
        if (refusal is not null) return Results2.Text(refusal, 401);
        http.Response.RegisterForDisposeAsync(body!);
        http.Request.Body = body!;
        who = publisher;
    }
    else
    {
        var loopback = http.Connection.RemoteIpAddress is { } ip && IPAddress.IsLoopback(ip);
        if (!http.Request.IsHttps && !(opts.AllowLoopbackHttpPush && loopback))
            return Results2.Text(Record.Refused(20, "a push key must never travel in clear: over http, sign the push with your own key instead (pkg PUSH ... SIGN <keyfile>)", "use the https address, or a signed push"), 403);
        who = http.RequestServices.GetRequiredService<PublisherKeys>().Find(http.Request.Headers.Authorization);
    }
    if (who is null)
        return Results2.Text(Record.Refused(14, "no push key, or one the portal does not know. " + Ask(http), "ask the maintainers; once registered, sign the push with SIGN <keyfile>, or set PKG_PUSHKEY to the key you were given"), 401);
    if (!who.MayPush(channel))
        return Results2.Text(Record.Refused(14, $"the key of {who.Name} may not push to {channel}. " + Ask(http), "ask the maintainers for the channel to be added to your key"), 403);
    if (!opts.Policy.NewChannels && !Directory.Exists(Path.Combine(opts.ChannelsDir, channel)))
        return Results2.Text(opts.Policy.Refuse("NewChannels", "off", $"there is no channel {channel}, and this portal does not create channels on a push",
            "push to an existing channel, or ask the operators to create this one"), 403);
    http.Items["publisher"] = who;
    return await next(ctx);
});

push.MapPost("/session", async (HttpContext http, SignedPush s) =>
{
    var answer = s.Open(await ReadBody(http), Ask(http));
    return Results2.Text(answer, answer.Get("result") == "session" ? 200 : 401);
});

push.MapPost("/plan", async (HttpContext http, string channel, PushService s) =>
    Results2.Text(await s.Plan((Publisher)http.Items["publisher"]!, channel, await ReadBody(http))));

push.MapPut("/files/{**path}", async (HttpContext http, string channel, string path, PushService s) =>
{
    var (answer, status) = await s.PutFile((Publisher)http.Items["publisher"]!, channel, path,
        http.Request.Headers.ContentRange.FirstOrDefault(), http.Request.ContentLength, http.Request.Body, http.RequestAborted);
    return Results2.Text(answer, status);
});

push.MapPost("/commit", async (HttpContext http, string channel, PushService s, Portal.Channels.Usage usage) =>
{
    usage.Note(http.Request.Headers.UserAgent, Portal.Channels.Usage.Pushes);
    return Results2.Text(await s.Commit((Publisher)http.Items["publisher"]!, channel, await ReadBody(http), http.RequestAborted));
});

// ---- for tools, agents and feed readers --------------------------------------
Portal.Api.Endpoints.MapPortalApi(app);

// ---- getting Pkg before one has it ------------------------------------------

// The drawer for one AROS CPU, zipped for the machine next to it.
app.MapGet("/get/{channel}/pkg-{cpu}.zip", async (HttpContext http, string channel, string cpu, Catalogue c) =>
{
    var ch = c.Get(channel);
    var dir = Path.Combine(opts.ChannelsDir, channel);
    if (ch is null || !Portal.Channels.Bootstrap.ArosCpus(dir).Contains(cpu)) return Results.NotFound();
    http.RequestServices.GetRequiredService<Portal.Channels.Downloads>().Count($"get/{channel}/pkg-{cpu}.zip");
    http.Response.ContentType = "application/zip";
    http.Response.Headers.ContentDisposition = $"attachment; filename=\"Pkg-{cpu}.zip\"";
    await Portal.Channels.Bootstrap.WriteZip(http.Response.Body, ch, dir, cpu, http.RequestAborted);
    return Results.Empty;
});

// The installers: curl -fsSL <site>/install | sh, and irm <site>/install.ps1 | iex.
// Plain scripts, readable at the same address, with this site's address in them.
foreach (var (route, file) in new[] { ("/install", "install.sh"), ("/install.sh", "install.sh"), ("/install.ps1", "install.ps1") })
    app.MapGet(route, (HttpContext http, Portal.Channels.Downloads d) =>
    {
        var site = opts.PublicUrl.Length > 0 ? opts.PublicUrl.TrimEnd('/') : $"{http.Request.Scheme}://{http.Request.Host}";
        var text = File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "Install", file))
            .Replace("@@PORTAL@@", site).Replace("@@SSHKEY@@", opts.BootstrapKey.Trim());
        d.Count($"get/pkg/{file}");
        http.Response.Headers.CacheControl = "no-cache";
        return Results.Text(text, "text/plain; charset=utf-8");
    });

// Sign in and out. The return address is always this site's own account page.
app.MapGet("/signin", (HttpContext http) => opts.GitHub.On
    ? Results.Challenge(new Microsoft.AspNetCore.Authentication.AuthenticationProperties { RedirectUri = "/account" }, ["GitHub"])
    : Results.NotFound()).RequireRateLimiting("admin");
app.MapPost("/signout", async (HttpContext http) =>
{
    await Microsoft.AspNetCore.Authentication.AuthenticationHttpContextExtensions.SignOutAsync(http);
    return Results.Redirect("/");
}).DisableAntiforgery();

// A view key in this browser: its holder sees the unlisted channels too. Rate-limited
// like the admin API; the key is shown once by `Portal viewkey <name>`.
app.MapGet("/see/{key}", (HttpContext http, string key, Portal.Channels.Catalogue c) =>
{
    if (key == "off") { http.Response.Cookies.Delete(Portal.Channels.Catalogue.ViewCookie); return Results.Redirect("/"); }
    if (!c.IsViewKey(key)) return Results.NotFound();
    http.Response.Cookies.Append(Portal.Channels.Catalogue.ViewCookie, key, new CookieOptions
        { HttpOnly = true, Secure = http.Request.IsHttps, SameSite = Microsoft.AspNetCore.Http.SameSiteMode.Lax, MaxAge = TimeSpan.FromDays(365), IsEssential = true });
    return Results.Redirect("/");
}).RequireRateLimiting("admin");

// Get-Pkg: the same for an AROS machine with a network and wget, over plain
// http, the one way up for a Pkg older than 1.6. Latin-1, like the Shell.
app.MapGet("/Get-Pkg", (HttpContext http, Portal.Channels.Catalogue c, Portal.Channels.Downloads d) =>
{
    var channel = opts.Pinned.Split(',', ';')[0].Split('/')[0].Trim();
    var site = opts.PublicUrl.Length > 0 ? opts.PublicUrl.TrimEnd('/') : $"{http.Request.Scheme}://{http.Request.Host}";
    var plain = "http://" + site[(site.IndexOf("://", StringComparison.Ordinal) + 3)..];
    var cpus = c.Get(channel) is null ? [] : Portal.Channels.Bootstrap.ArosCpus(Path.Combine(opts.ChannelsDir, channel));
    if (cpus.Count == 0) return Results.NotFound();
    // A native AROS with a network is a PC far more often than not: try that build first.
    cpus = cpus.OrderBy(x => x == "x86_64" ? 0 : 1).ToList();
    var probes = string.Concat(cpus.Select(cpu => $"""
        If "$pkgboot" EQ ""
            $pkgwget -q -O RAM:Pkg-bootstrap {plain}/{channel}/Bootstrap/{cpu}/Pkg
            If EXISTS RAM:Pkg-bootstrap
                Protect RAM:Pkg-bootstrap +e >NIL:
                RAM:Pkg-bootstrap HELP >RAM:pkgboot.out
                Search RAM:pkgboot.out "usage" QUIET >NIL:
                If NOT WARN
                    Set pkgboot "{cpu}"
                    Echo "This machine runs the {cpu} build."
                EndIf
            EndIf
        EndIf

        """));
    var text = File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "Install", "Get-Pkg"))
        .Replace("@@PROBES@@\n", probes).Replace("@@HTTP@@", plain).Replace("@@CHANNEL@@", channel)
        .Replace("@@CPUS@@", string.Concat(cpus.Select(x => " " + x)));
    d.Count("get/pkg/Get-Pkg");
    http.Response.Headers.CacheControl = "no-cache";
    return Results.Text(text, "text/plain; charset=iso-8859-1");
});

// The second opinion on a signature, from the repository's tools/, as /trust uses it.
app.MapGet("/verify-manifest.py", () =>
    Results.Text(File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "Install", "verify-manifest.py")), "text/plain; charset=utf-8"));

// Pkg for a host, by platform, for curl and PowerShell one-liners.
app.MapGet("/get/{channel}/{platform}", (string channel, string platform, Portal.Channels.Downloads d) =>
{
    if (!ChannelPaths.IsChannelName(channel) || !ChannelPaths.HostBootstraps.TryGetValue(platform, out var rel)
        || !File.Exists(Path.Combine(opts.ChannelsDir, channel, rel)))
        return Results.NotFound();
    d.Count($"get/{channel}/{platform}");
    return Results.Redirect($"/{channel}/{rel}");
});

// ---- the channel itself, byte for byte as a directory channel --------------

// A channel's address in a browser shows its page; a word that is no channel is simply not found.
app.MapMethods("/{channel}", ["GET", "HEAD"], (string channel, Catalogue c) =>
    ChannelPaths.IsChannelName(channel) && c.Get(channel) is not null ? Results.Redirect($"/channels/{channel}") : Results.NotFound());

app.MapMethods("/{channel}/{**path}", ["GET", "HEAD"], async (HttpContext http, string channel, string? path) =>
{
    if (!ChannelPaths.IsChannelName(channel)) return Results.NotFound();
    if (string.IsNullOrEmpty(path))
        return http.RequestServices.GetRequiredService<Catalogue>().Get(channel) is not null ? Results.Redirect($"/channels/{channel}") : Results.NotFound();
    var kind = ChannelPaths.Classify(path);
    var full = Path.Combine(opts.ChannelsDir, channel, path);
    if (kind == ChannelPaths.Kind.None) return Results.NotFound();
    // An archive kept elsewhere (a GitHub release asset, R2): the client
    // checks every file it takes out against the signed manifest.
    if (kind == ChannelPaths.Kind.Archive && File.Exists(full + ".url") && !File.Exists(full))
    {
        // Straight to where its maker publishes it: every Pkg speaks https, and
        // what comes back is checked against the signed manifest either way.
        return Results.Redirect(File.ReadAllText(full + ".url").Trim());
    }
    if (!File.Exists(full)) return Results.NotFound();
    // How Pkg spreads: a channel read, and below a package taken. Three words
    // from the request, nothing about who asked. /privacy says it in full.
    if (path == "index" && HttpMethods.IsGet(http.Request.Method))
        http.RequestServices.GetRequiredService<Portal.Channels.Usage>().Note(http.Request.Headers.UserAgent, Portal.Channels.Usage.Reads);
    var info = new FileInfo(full);
    // A payload fetched from its first byte is one download; resumed parts are not counted again.
    if (kind == ChannelPaths.Kind.Object && path.EndsWith(".pkg", StringComparison.Ordinal) && HttpMethods.IsGet(http.Request.Method)
        && (http.Request.Headers.Range.Count == 0 || http.Request.Headers.Range.ToString().StartsWith("bytes=0-", StringComparison.Ordinal)))
    {
        http.RequestServices.GetRequiredService<Portal.Channels.Downloads>().Count($"payload/{channel}/{path[8..72]}");
        http.RequestServices.GetRequiredService<Portal.Channels.Usage>().Note(http.Request.Headers.UserAgent, Portal.Channels.Usage.Downloads);
    }
    var immutable = kind is ChannelPaths.Kind.Object or ChannelPaths.Kind.Archive or ChannelPaths.Kind.ArchiveDigest;
    http.Response.Headers.CacheControl = immutable ? "public, max-age=31536000, immutable" : "no-cache";
    var etag = new EntityTagHeaderValue($"\"{info.Length:x}-{info.LastWriteTimeUtc.Ticks:x}\"");
    return Results.File(full, ChannelPaths.ContentType(path), lastModified: info.LastWriteTimeUtc,
        entityTag: etag, enableRangeProcessing: true);
}).RequireRateLimiting("reads");

app.Run();


static async Task<string> ReadBody(HttpContext http)
{
    using var reader = new StreamReader(http.Request.Body);
    return await reader.ReadToEndAsync(http.RequestAborted);
}

public partial class Program;
