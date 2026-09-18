// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

namespace Portal.Channels;

/// <summary>
/// Pkg's version order, ported from pkg_version_cmp in src/pkg_manifest.c so
/// that the catalogue lists versions in the order the client picks them:
/// 41.7 &lt; 41.7+20260917 &lt; 41.7+20260918 &lt; 41.8.
/// </summary>
public sealed class PkgVersion : IComparer<string>
{
    public static readonly PkgVersion Order = new();

    public int Compare(string? a, string? b)
    {
        a ??= ""; b ??= "";
        int i = 0, j = 0;
        int r = Dotted(a, ref i, b, ref j);
        if (r != 0) return r;
        if (i < a.Length && a[i] == '+') i++;
        if (j < b.Length && b[j] == '+') j++;
        return Dotted(a, ref i, b, ref j);
    }

    static int Dotted(string a, ref int i, string b, ref int j)
    {
        int r = 0;
        while ((i < a.Length && a[i] != '+') || (j < b.Length && b[j] != '+'))
        {
            ulong x = 0, y = 0;
            int si = i, sj = j;
            while (i < a.Length && char.IsAsciiDigit(a[i])) x = x * 10 + (ulong)(a[i++] - '0');
            while (j < b.Length && char.IsAsciiDigit(b[j])) y = y * 10 + (ulong)(b[j++] - '0');
            if (r == 0 && x != y) r = x < y ? -1 : 1;
            if (i < a.Length && a[i] == '.') i++;
            if (j < b.Length && b[j] == '.') j++;
            // Pkg refuses versions with other characters at publish; a hand-made
            // index could still hold one, and it must not stop the loop.
            if (i == si && j == sj)
            {
                if (i < a.Length && a[i] != '+') i++;
                if (j < b.Length && b[j] != '+') j++;
            }
        }
        return r;
    }

    /// The build after '+', such as a nightly date, or null.
    public static string? Build(string version)
    {
        int p = version.IndexOf('+');
        return p < 0 ? null : version[(p + 1)..];
    }

    /// The version before '+'.
    public static string Base(string version)
    {
        int p = version.IndexOf('+');
        return p < 0 ? version : version[..p];
    }
}
