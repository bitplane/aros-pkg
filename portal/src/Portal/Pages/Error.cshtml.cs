// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Diagnostics;
using Microsoft.AspNetCore.Diagnostics;
using Microsoft.AspNetCore.Mvc;
using Microsoft.AspNetCore.Mvc.RazorPages;
using Microsoft.Extensions.Options;

namespace Portal.Pages;

[ResponseCache(Duration = 0, Location = ResponseCacheLocation.None, NoStore = true)]
[IgnoreAntiforgeryToken]
public class ErrorModel(IOptions<PortalOptions> options) : PageModel
{
    public string? RequestId { get; set; }

    public bool ShowRequestId => !string.IsNullOrEmpty(RequestId);

    public void OnGet()
    {
        RequestId = Activity.Current?.Id ?? HttpContext.TraceIdentifier;
        // A page that fails is kept like a machine's failure, under the same
        // request this page shows, so a maintainer can say afterwards what went
        // wrong for whoever saw it. Nothing about who asked, as ever.
        if (HttpContext.Features.Get<IExceptionHandlerFeature>() is { } f)
            Portal.Push.Failures.Note(options.Value.StateDir,
                $"GET {f.Path ?? HttpContext.Request.Path.Value} {RequestId}", f.Error);
    }
}
