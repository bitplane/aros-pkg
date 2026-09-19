// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

namespace Portal;

public sealed class PortalOptions
{
    /// Where channels, staging and the portal's own state live. On App
    /// Service this is under /home, the only persistent disk.
    public string DataDir { get; set; } = "data";

    /// The Pkg executable that checks every push: build/pkg on the Mac,
    /// the static pkg-linux-x86_64 on the server.
    public string PkgPath { get; set; } = "pkg";

    /// Publisher keys as "name:sha256-of-key:channel,channel;name2:...".
    /// Only the hash is configured; the key itself is given to the publisher once.
    public string Keys { get; set; } = "";

    /// Maintainers' keys for /_admin, "name:sha256-of-key;..."; only the hash is
    /// deployed. They never push, and push keys never reach /_admin.
    public string AdminKeys { get; set; } = "";

    /// Accept pushes over plain HTTP from the loopback address, for tests
    /// and a local instance. Never over the network.
    public bool AllowLoopbackHttpPush { get; set; }

    /// Names for signing keys, "hex=Name;hex2=Name2"; they win over names learnt at push time.
    public string SignerNames { get; set; } = "";

    /// Publisher profiles set by the maintainers, "hex|name|url|contact;...";
    /// url and contact are optional. They win over SignerNames.
    public string Publishers { get; set; } = "";

    /// Packages moved to another key by the maintainers, "channel/name=hex;...":
    /// from then on a push of that package must be signed by that key, whatever
    /// signed its first version. Machines that pinned the old key refuse the new
    /// one once, and their owners decide with ACCEPTKEY.
    /// Publishers known by the public half of their signing key, for a push
    /// whose requests are signed instead of carrying a key (plain http, AROS):
    /// "name:publickey:channels[:files];...". Nothing here is secret.
    public string SignedKeys { get; set; } = "";

    public string Owners { get; set; } = "";

    /// Channels served to whoever has the address but shown nowhere: not on the
    /// home page, in search, statistics, feeds or publishers. "a;b". The admin
    /// API adds to this list without a restart (state/<channel>/unlisted).
    public string Unlisted { get; set; } = "";

    /// Packages shown first, as "channel/name" separated by commas, e.g.
    /// "pkg/pkg,contrib-nightly/regina". Pkg itself belongs here: people
    /// need it before anything else.
    public string Pinned { get; set; } = "pkg/pkg";

    /// The public base URL shown in commands, e.g. https://aros-pkg.azurewebsites.net.
    /// Empty: taken from the request.
    public string PublicUrl { get; set; } = "";

    /// Largest request body, one part of a large file included.
    public long MaxPartBytes { get; set; } = 40L * 1024 * 1024;

    /// The most a key may hold in staging at once, so no key can fill the disk.
    /// Link-only keys send manifests and signatures, so they get far less.
    public long MaxStagingBytes { get; set; } = 2L * 1024 * 1024 * 1024;
    public long MaxLinkOnlyStagingBytes { get; set; } = 16L * 1024 * 1024;

    /// Staging not committed for this long is removed.
    public TimeSpan StagingLifetime { get; set; } = TimeSpan.FromHours(24);

    /// How long a commit waits for Pkg's check before refusing.
    public TimeSpan CheckTimeout { get; set; } = TimeSpan.FromSeconds(150);

    /// Where checked source archives are kept (Cloudflare R2); off when empty.
    public Push.R2Options R2 { get; set; } = new();

    /// The key that signs Bootstrap/SHA256SUMS of the pkg channel, in OpenSSH
    /// form ("ssh-ed25519 AAAA… name"). Set, the installers verify what they
    /// download with ssh-keygen -Y verify before installing it.
    public string BootstrapKey { get; set; } = "";

    /// What this instance accepts: see Policy.cs.
    public PortalPolicy Policy { get; set; } = new();

    public string ChannelsDir => Path.Combine(DataDir, "channels");
    public string StagingDir => Path.Combine(DataDir, "staging");
    public string StateDir => Path.Combine(DataDir, "state");
}
