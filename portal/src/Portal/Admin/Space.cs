// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using Amazon.S3;
using Amazon.S3.Model;
using Amazon.Runtime;
using Microsoft.Extensions.Options;
using Portal.Push;

namespace Portal.Admin;

/// What a set of files takes.
public sealed record Held(long Bytes, int Files)
{
    public static Held operator +(Held a, Held b) => new(a.Bytes + b.Bytes, a.Files + b.Files);
    public static readonly Held None = new(0, 0);
}

/// One channel's share of the disk, by what the files are.
public sealed record ChannelSpace(string Name, Held Objects, Held Archives, Held Bootstraps, Held Rest, int ArchivesElsewhere)
{
    public Held All => Objects + Archives + Bootstraps + Rest;
}

/// What R2 holds under this portal's prefix, or why it could not be read.
public sealed record R2Space(string Bucket, string Prefix, long Bytes, int Objects);

public sealed record SpaceReport(
    DateTime Taken,
    IReadOnlyList<ChannelSpace> Channels,
    Held State,
    Held Staging,
    long VolumeTotal,
    long VolumeFree,
    R2Space? R2,
    string? R2Note)
{
    public Held Channelled => Channels.Aggregate(Held.None, (a, c) => a + c.All);
    public Held OnDisk => Channelled + State + Staging;
    /// What moving every archive to R2 would take off the disk.
    public Held Movable => Channels.Aggregate(Held.None, (a, c) => a + c.Archives);
}

/// <summary>
/// How much room the portal takes, on the machine it runs on and in R2.
/// Reading it walks the channels, which on a web app's storage is a network
/// share and slow, so a report is kept and only made again when it is old or
/// when a maintainer asks for a fresh one.
/// </summary>
public sealed class Space(IOptions<PortalOptions> options, ILogger<Space> log)
{
    readonly PortalOptions o = options.Value;
    readonly SemaphoreSlim gate = new(1, 1);
    SpaceReport? last;

    public static readonly TimeSpan Keep = TimeSpan.FromMinutes(15);

    public SpaceReport? Last => last;

    public async Task<SpaceReport> Get(bool fresh, CancellationToken ct)
    {
        if (!fresh && last is { } have && DateTime.UtcNow - have.Taken < Keep) return have;
        await gate.WaitAsync(ct);
        try
        {
            if (!fresh && last is { } again && DateTime.UtcNow - again.Taken < Keep) return again;
            return last = await Measure(ct);
        }
        finally { gate.Release(); }
    }

    async Task<SpaceReport> Measure(CancellationToken ct)
    {
        var channels = new List<ChannelSpace>();
        if (Directory.Exists(o.ChannelsDir))
            foreach (var dir in Directory.EnumerateDirectories(o.ChannelsDir).OrderBy(d => d, StringComparer.Ordinal))
                channels.Add(OneChannel(dir));
        var (r2, note) = await InR2(ct);
        var (total, free) = Volume(o.DataDir);
        return new SpaceReport(DateTime.UtcNow, channels, Walk(o.StateDir), Walk(o.StagingDir), total, free, r2, note);
    }

    static ChannelSpace OneChannel(string dir)
    {
        var objects = Walk(Path.Combine(dir, "objects"));
        var archives = Held.None;
        var elsewhere = 0;
        var archiveDir = Path.Combine(dir, "archives");
        if (Directory.Exists(archiveDir))
            foreach (var f in new DirectoryInfo(archiveDir).EnumerateFiles("*", SearchOption.AllDirectories))
            {
                // An archive already in R2 leaves a .url and a .sha256 behind,
                // bytes the portal keeps whatever happens.
                if (f.Name.EndsWith(".url", StringComparison.Ordinal)) elsewhere++;
                archives += new Held(f.Length, 1);
            }
        var boot = Walk(Path.Combine(dir, "Bootstrap"));
        // Everything else the channel holds: the index, the withdrawals list,
        // Install-Pkg, ReadMe, and anything a push left at the root.
        var rest = Held.None;
        if (Directory.Exists(dir))
            foreach (var f in new DirectoryInfo(dir).EnumerateFiles("*", SearchOption.TopDirectoryOnly))
                rest += new Held(f.Length, 1);
        return new ChannelSpace(Path.GetFileName(dir), objects, archives, boot, rest, elsewhere);
    }

    static Held Walk(string dir)
    {
        if (!Directory.Exists(dir)) return Held.None;
        var held = Held.None;
        foreach (var f in new DirectoryInfo(dir).EnumerateFiles("*", SearchOption.AllDirectories))
            held += new Held(f.Length, 1);
        return held;
    }

    static (long Total, long Free) Volume(string path)
    {
        try
        {
            var full = Path.GetFullPath(path);
            var drive = DriveInfo.GetDrives()
                .Where(d => d.IsReady && full.StartsWith(d.RootDirectory.FullName, StringComparison.Ordinal))
                .OrderByDescending(d => d.RootDirectory.FullName.Length).FirstOrDefault();
            return drive is null ? (0, 0) : (drive.TotalSize, drive.AvailableFreeSpace);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return (0, 0);
        }
    }

    async Task<(R2Space?, string?)> InR2(CancellationToken ct)
    {
        var r2 = o.R2;
        if (!r2.Enabled) return (null, "R2 is not configured on this portal: no account, bucket or key is set.");
        try
        {
            using var s3 = new AmazonS3Client(new BasicAWSCredentials(r2.AccessKeyId, r2.SecretAccessKey),
                new AmazonS3Config
                {
                    ServiceURL = $"https://{r2.AccountId}.r2.cloudflarestorage.com",
                    AuthenticationRegion = "auto",
                    ForcePathStyle = true,
                    RequestChecksumCalculation = RequestChecksumCalculation.WHEN_REQUIRED,
                    ResponseChecksumValidation = ResponseChecksumValidation.WHEN_REQUIRED,
                });
            long bytes = 0;
            var count = 0;
            string? token = null;
            do
            {
                var page = await s3.ListObjectsV2Async(new ListObjectsV2Request
                {
                    BucketName = r2.Bucket, Prefix = r2.Prefix, ContinuationToken = token, MaxKeys = 1000,
                }, ct);
                foreach (var obj in page.S3Objects ?? []) { bytes += obj.Size ?? 0; count++; }
                token = page.IsTruncated == true ? page.NextContinuationToken : null;
            } while (token is not null);
            return (new R2Space(r2.Bucket, r2.Prefix, bytes, count), null);
        }
        catch (AmazonServiceException e)
        {
            log.LogWarning(e, "reading how much R2 holds");
            return (null, $"R2 did not answer: {e.Message}");
        }
    }

    /// The same report for a machine, in Pkg's own form.
    public static Record AsRecord(SpaceReport r)
    {
        var rec = new Record().Add("result", "shown").Add("taken", r.Taken.ToString("O"));
        foreach (var c in r.Channels)
            rec.Add("channel", $"{c.Name} {c.All.Bytes} bytes in {c.All.Files} files, objects {c.Objects.Bytes}, archives {c.Archives.Bytes}, bootstraps {c.Bootstraps.Bytes}");
        rec.Add("state", $"{r.State.Bytes} bytes in {r.State.Files} files");
        rec.Add("staging", $"{r.Staging.Bytes} bytes in {r.Staging.Files} files");
        rec.Add("disk", $"{r.OnDisk.Bytes} bytes in {r.OnDisk.Files} files");
        if (r.VolumeTotal > 0) rec.Add("volume", $"{r.VolumeFree} bytes free of {r.VolumeTotal}");
        rec.Add("movable", $"{r.Movable.Bytes} bytes in {r.Movable.Files} files would go to R2");
        if (r.R2 is { } two) rec.Add("r2", $"{two.Bytes} bytes in {two.Objects} objects, bucket {two.Bucket}, prefix {(two.Prefix.Length == 0 ? "-" : two.Prefix)}");
        else if (r.R2Note is { } why) rec.Add("r2", why);
        return rec.Add("summary", $"{r.OnDisk.Bytes} bytes on this machine, {(r.R2 is { } t ? t.Bytes + " in R2" : "nothing in R2")}");
    }
}
