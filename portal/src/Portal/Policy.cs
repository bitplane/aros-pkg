// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using Portal.Push;

namespace Portal;

/// <summary>
/// What this portal instance accepts, set by its operators (Portal:Policy:*).
/// Every refusal a rule causes names the rule, its value and the operators'
/// note, so a publisher reads why and whom to ask; /api/policy and /trust show
/// the same rules to everyone.
/// </summary>
public sealed class PortalPolicy
{
    /// Uploads at all. Off: a read-only mirror of its channels.
    public bool Push { get; set; } = true;

    /// "keys": binaries (payloads, archives, bootstrap programs) from keys with
    /// the files right. "off": from nobody; every publisher links to its files.
    public string Binaries { get; set; } = "keys";

    /// Hosts an Archive: address may name, comma-separated; empty: any https host.
    /// A host matches itself and its subdomains (github.com covers objects.github.com).
    public string LinkHosts { get; set; } = "";

    /// Whether a push may create a channel that does not exist yet.
    public bool NewChannels { get; set; } = true;

    /// The maintainers' API, /_admin.
    public bool Admin { get; set; } = true;

    /// Channels also answer over plain http (machines without TLS).
    /// Off: every http request is sent to https.
    public bool PlainHttp { get; set; } = true;

    /// Signed pushes: requests signed with the publisher's key, which may come
    /// over plain http since nothing secret travels. Off: https and a push key only.
    public bool SignedPush { get; set; } = true;

    /// The operators' own sentence, added to every refusal and shown on /trust.
    public string Note { get; set; } = "";

    /// Whom to ask about the rules: an address or a web page.
    public string Contact { get; set; } = "";

    public bool BinariesAllowed => !Binaries.Equals("off", StringComparison.OrdinalIgnoreCase);

    public IReadOnlyList<string> Hosts =>
        LinkHosts.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).Select(h => h.ToLowerInvariant()).ToList();

    public bool HostAllowed(string host) =>
        Hosts.Count == 0 || Hosts.Any(h => host.Equals(h, StringComparison.OrdinalIgnoreCase) || host.EndsWith("." + h, StringComparison.OrdinalIgnoreCase));

    /// A refusal caused by a rule of this portal, saying which and why.
    public Record Refuse(string rule, string value, string reason, string next)
    {
        var r = Record.Refused(20, reason, next).Add("policy", $"{rule}={value}");
        if (Note.Length > 0) r.Add("note", Note);
        if (Contact.Length > 0) r.Add("contact", Contact);
        return r;
    }

    /// The refusal line a commit or plan item carries, with the rule named.
    public string Why(string rule, string value, string reason) =>
        $"{reason} (this portal's rule {rule}={value}{(Note.Length > 0 ? $"; {Note}" : "")})";
}
