// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Globalization;
using System.Net;
using Microsoft.AspNetCore.HttpOverrides;
using Microsoft.Extensions.Options;
using Microsoft.Net.Http.Headers;
using Portal;
using Portal.Channels;
using Portal.Push;

// `Portal key <publisher> <channel,channel|*>` prints a new push key and the
// line that configures it, then exits. Only the line goes into the settings.
if (args is ["key", var publisher, var scope])
{
    var (key, config) = PublisherKeys.Create(publisher, scope);
    Console.WriteLine($"key:    {key}");
    Console.WriteLine($"config: {config}");
    Console.WriteLine("summary: give the key to the publisher once; add the config line to Portal:Keys (entries separated by ';')");
    return;
}

// Records are read by machines: numbers never follow the server's locale.
CultureInfo.DefaultThreadCurrentCulture = CultureInfo.DefaultThreadCurrentUICulture = CultureInfo.InvariantCulture;

var builder = WebApplication.CreateBuilder(args);
builder.Services.Configure<PortalOptions>(builder.Configuration.GetSection("Portal"));
builder.Services.AddSingleton<Catalogue>();
builder.Services.AddSingleton<PublisherKeys>();
builder.Services.AddSingleton<PkgRunner>();
builder.Services.AddSingleton<PushService>();
builder.Services.AddSingleton<ArchiveChecker>();
builder.Services.AddSingleton<ArchiveStore>();
builder.Services.AddSingleton<Portal.Channels.Downloads>();
builder.Services.AddHostedService(sp => sp.GetRequiredService<Portal.Channels.Downloads>());
builder.Services.AddHttpClient("r2", c => c.Timeout = TimeSpan.FromHours(1));
builder.Services.AddHostedService(sp => sp.GetRequiredService<ArchiveChecker>());
builder.Services.AddRazorPages();
builder.Services.Configure<ForwardedHeadersOptions>(f =>
{
    // App Service terminates TLS in front of the app and says so in X-Forwarded-Proto.
    f.ForwardedHeaders = ForwardedHeaders.XForwardedFor | ForwardedHeaders.XForwardedProto;
    f.KnownIPNetworks.Clear();
    f.KnownProxies.Clear();
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
if (!app.Environment.IsDevelopment()) app.UseExceptionHandler("/Error");

// Unlisted: nothing here is to be indexed. No HTTPS redirect and no HSTS,
// because classic 68k clients speak plain HTTP; signatures carry integrity.
app.Use(async (ctx, next) =>
{
    ctx.Response.Headers["X-Robots-Tag"] = "noindex, nofollow";
    ctx.Response.Headers.XContentTypeOptions = "nosniff";
    ctx.Response.Headers["Referrer-Policy"] = "same-origin";
    await next();
});
app.UseStaticFiles();
// Routing after static files: the channel route would otherwise claim /css/site.css.
app.UseRouting();
app.MapGet("/robots.txt", () => Results.Text("User-agent: *\nDisallow: /\n"));
app.MapGet("/health", (Catalogue c) => Results.Text($"ok: {c.ChannelNames().Count()} channels\n"));
app.MapRazorPages();

// ---- the push API, under each channel ---------------------------------------

var push = app.MapGroup("/{channel}/_push").AddEndpointFilter(async (ctx, next) =>
{
    var http = ctx.HttpContext;
    var channel = (string)http.GetRouteValue("channel")!;
    var loopback = http.Connection.RemoteIpAddress is { } ip && IPAddress.IsLoopback(ip);
    if (!http.Request.IsHttps && !(opts.AllowLoopbackHttpPush && loopback))
        return Results2.Text(Record.Refused(20, "a push needs https: its key must never travel in clear", "use the https address"), 403);
    if (!ChannelPaths.IsChannelName(channel))
        return Results2.Text(Record.Refused(20, $"'{channel}' is not a channel name: lowercase letters, digits and '-'", "check the address"), 400);
    var who = http.RequestServices.GetRequiredService<PublisherKeys>().Find(http.Request.Headers.Authorization);
    if (who is null)
        return Results2.Text(Record.Refused(14, "no push key, or one the portal does not know", "set PKG_PUSHKEY to the key you were given"), 401);
    if (!who.MayPush(channel))
        return Results2.Text(Record.Refused(14, $"the key of {who.Name} may not push to {channel}", "ask for the channel to be added to your key"), 403);
    http.Items["publisher"] = who;
    return await next(ctx);
});

push.MapPost("/plan", async (HttpContext http, string channel, PushService s) =>
    Results2.Text(await s.Plan((Publisher)http.Items["publisher"]!, channel, await ReadBody(http))));

push.MapPut("/files/{**path}", async (HttpContext http, string channel, string path, PushService s) =>
{
    var (answer, status) = await s.PutFile((Publisher)http.Items["publisher"]!, channel, path,
        http.Request.Headers.ContentRange.FirstOrDefault(), http.Request.ContentLength, http.Request.Body, http.RequestAborted);
    return Results2.Text(answer, status);
});

push.MapPost("/commit", async (HttpContext http, string channel, PushService s) =>
    Results2.Text(await s.Commit((Publisher)http.Items["publisher"]!, channel, await ReadBody(http), http.RequestAborted)));

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
        var text = File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "Install", file)).Replace("@@PORTAL@@", site);
        d.Count($"get/pkg/{file}");
        http.Response.Headers.CacheControl = "no-cache";
        return Results.Text(text, "text/plain; charset=utf-8");
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

app.MapMethods("/{channel}", ["GET", "HEAD"], (string channel) =>
    ChannelPaths.IsChannelName(channel) ? Results.Redirect($"/channels/{channel}") : Results.NotFound());

app.MapMethods("/{channel}/{**path}", ["GET", "HEAD"], async (HttpContext http, string channel, string? path) =>
{
    if (!ChannelPaths.IsChannelName(channel)) return Results.NotFound();
    if (string.IsNullOrEmpty(path)) return Results.Redirect($"/channels/{channel}");
    var kind = ChannelPaths.Classify(path);
    var full = Path.Combine(opts.ChannelsDir, channel, path);
    if (kind == ChannelPaths.Kind.None) return Results.NotFound();
    // An archive kept elsewhere (a GitHub release asset, R2): the client
    // checks every file it takes out against the signed manifest.
    if (kind == ChannelPaths.Kind.Archive && File.Exists(full + ".url") && !File.Exists(full))
    {
        var at = File.ReadAllText(full + ".url").Trim();
        // https clients go straight there; r2.dev answers only https, so a
        // plain-http client (68k, no TLS) gets the bytes through the portal.
        if (http.Request.IsHttps || !at.StartsWith("https:", StringComparison.Ordinal)) return Results.Redirect(at);
        return await Relay(http, at);
    }
    if (!File.Exists(full)) return Results.NotFound();
    var info = new FileInfo(full);
    // A payload fetched from its first byte is one download; resumed parts are not counted again.
    if (kind == ChannelPaths.Kind.Object && path.EndsWith(".pkg", StringComparison.Ordinal) && HttpMethods.IsGet(http.Request.Method)
        && (http.Request.Headers.Range.Count == 0 || http.Request.Headers.Range.ToString().StartsWith("bytes=0-", StringComparison.Ordinal)))
        http.RequestServices.GetRequiredService<Portal.Channels.Downloads>().Count($"payload/{channel}/{path[8..72]}");
    var immutable = kind is ChannelPaths.Kind.Object or ChannelPaths.Kind.Archive or ChannelPaths.Kind.ArchiveDigest;
    http.Response.Headers.CacheControl = immutable ? "public, max-age=31536000, immutable" : "no-cache";
    var etag = new EntityTagHeaderValue($"\"{info.Length:x}-{info.LastWriteTimeUtc.Ticks:x}\"");
    return Results.File(full, ChannelPaths.ContentType(path), lastModified: info.LastWriteTimeUtc,
        entityTag: etag, enableRangeProcessing: true);
});

app.Run();

// Relay an archive held elsewhere, Range included, for a client that cannot follow https.
static async Task<IResult> Relay(HttpContext http, string url)
{
    var req = new HttpRequestMessage(HttpMethods.IsHead(http.Request.Method) ? HttpMethod.Head : HttpMethod.Get, url);
    if (http.Request.Headers.Range.Count > 0) req.Headers.TryAddWithoutValidation("Range", http.Request.Headers.Range.ToString());
    var resp = await http.RequestServices.GetRequiredService<IHttpClientFactory>().CreateClient("r2")
        .SendAsync(req, HttpCompletionOption.ResponseHeadersRead, http.RequestAborted);
    http.Response.RegisterForDispose(resp);
    http.Response.StatusCode = (int)resp.StatusCode;
    foreach (var h in new[] { "Content-Length", "Content-Range", "Accept-Ranges", "ETag", "Last-Modified" })
        if (resp.Headers.TryGetValues(h, out var v) || resp.Content.Headers.TryGetValues(h, out v))
            http.Response.Headers[h] = v.ToArray();
    http.Response.ContentType = resp.Content.Headers.ContentType?.ToString() ?? "application/octet-stream";
    http.Response.Headers.CacheControl = "public, max-age=31536000, immutable";
    if (!HttpMethods.IsHead(http.Request.Method))
        await (await resp.Content.ReadAsStreamAsync(http.RequestAborted)).CopyToAsync(http.Response.Body, http.RequestAborted);
    return Results.Empty;
}

static async Task<string> ReadBody(HttpContext http)
{
    using var reader = new StreamReader(http.Request.Body);
    return await reader.ReadToEndAsync(http.RequestAborted);
}

public partial class Program;
