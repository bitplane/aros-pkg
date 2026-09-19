// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using Microsoft.Extensions.Options;
using Portal.Channels;

namespace Portal.Tests;

public class PublishersTests
{
    const string A = "a974a917b19cfc46eb462510fa21f95013932bfbda7c8f343e06a3e988f7bde7";
    const string J = "43c550967bc18dfec7cf3a7cd01297d09450fd364a0e34d8e58aef623cef3077";

    static Publishers With(string publishers, string owners)
    {
        var o = Options.Create(new PortalOptions
        {
            DataDir = Directory.CreateTempSubdirectory("portal-pub-").FullName,
            Publishers = publishers, Owners = owners, SignerNames = $"{A}=an older name",
        });
        return new Publishers(new Catalogue(o), o);
    }

    [Fact]
    public void Profiles_carry_optional_url_and_contact_and_win_over_older_names()
    {
        var p = With($"{A}|aros-development-team|https://github.com/aros-development-team|; {J}|JKN", "");
        var profiles = p.Profiles();
        Assert.Equal(("aros-development-team", "https://github.com/aros-development-team", (string?)null),
                     (profiles[A].Name, profiles[A].Url, profiles[A].Contact));
        Assert.Equal(("JKN", (string?)null), (profiles[J].Name, profiles[J].Url));
        Assert.Equal("aros-development-team", p.Names()[A]);
    }

    [Fact]
    public void A_transfer_names_the_key_of_that_package_only()
    {
        var p = With("", $"pkg/pkg={J}; broken/entry=nothex");
        Assert.Equal(J, p.OwnerOf("pkg", "pkg", A));
        Assert.Equal(A, p.OwnerOf("contrib-nightly", "regina", A));
        Assert.Equal(A, p.OwnerOf("broken", "entry", A));
        Assert.Null(p.OwnerOf("pkg", "new", null));
    }
}
