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

    /// Accept pushes over plain HTTP from the loopback address, for tests
    /// and a local instance. Never over the network.
    public bool AllowLoopbackHttpPush { get; set; }

    /// Names for signing keys, "hex=Name;hex2=Name2"; they win over names learnt at push time.
    public string SignerNames { get; set; } = "";

    /// Packages shown first, as "channel/name" separated by commas, e.g.
    /// "pkg/pkg,contrib-nightly/regina". Pkg itself belongs here: people
    /// need it before anything else.
    public string Pinned { get; set; } = "pkg/pkg";

    /// The public base URL shown in commands, e.g. https://aros-pkg.azurewebsites.net.
    /// Empty: taken from the request.
    public string PublicUrl { get; set; } = "";

    /// Largest request body, one part of a large file included.
    public long MaxPartBytes { get; set; } = 40L * 1024 * 1024;

    /// Staging not committed for this long is removed.
    public TimeSpan StagingLifetime { get; set; } = TimeSpan.FromHours(24);

    /// How long a commit waits for Pkg's check before refusing.
    public TimeSpan CheckTimeout { get; set; } = TimeSpan.FromSeconds(150);

    /// Where checked source archives are kept (Cloudflare R2); off when empty.
    public Push.R2Options R2 { get; set; } = new();

    public string ChannelsDir => Path.Combine(DataDir, "channels");
    public string StagingDir => Path.Combine(DataDir, "staging");
    public string StateDir => Path.Combine(DataDir, "state");
}
