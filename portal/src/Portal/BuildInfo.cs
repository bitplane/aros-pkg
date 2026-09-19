// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Reflection;

namespace Portal;

/// Which portal this is, for the footer: version, build number (the count of
/// commits), commit and the time it was built, stamped by the project file.
public static class BuildInfo
{
    static string Meta(string key) => typeof(BuildInfo).Assembly.GetCustomAttributes<AssemblyMetadataAttribute>()
        .FirstOrDefault(a => a.Key == key)?.Value is { Length: > 0 } v ? v : "?";
    public static readonly string Version = typeof(BuildInfo).Assembly.GetName().Version is { } v ? $"{v.Major}.{v.Minor}" : "?";
    public static readonly string Line = $"Portal {Version}, build {Meta("Build")} ({Meta("Commit")}), {Meta("Built")} UTC";
}
