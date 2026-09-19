// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Security.Cryptography;
using Amazon.Runtime;
using Amazon.S3;
using Amazon.S3.Model;
using Amazon.S3.Transfer;
using Microsoft.Extensions.Options;

namespace Portal.Push;

public sealed class R2Options
{
    public string AccountId { get; set; } = "";
    public string AccessKeyId { get; set; } = "";
    public string SecretAccessKey { get; set; } = "";
    public string Bucket { get; set; } = "";
    /// The bucket's public address, e.g. https://pub-….r2.dev.
    public string PublicUrl { get; set; } = "";
    /// Put before every key, e.g. "test/" for a local instance, so that a test
    /// never writes where a real channel's archives live.
    public string Prefix { get; set; } = "";

    public bool Enabled => AccountId.Length > 0 && AccessKeyId.Length > 0 && SecretAccessKey.Length > 0
                           && Bucket.Length > 0 && PublicUrl.Length > 0;
}

/// <summary>
/// Source archives kept in Cloudflare R2 instead of the web app's disk (the
/// owner's hosting rule). An archive goes there only after Pkg has checked
/// it, and leaves the disk only once the copy in R2 reads back with the same
/// SHA-256 through its public address. What remains on the portal is
/// archives/&lt;name&gt;.url and archives/&lt;name&gt;.sha256.
/// </summary>
public sealed class ArchiveStore(IOptions<PortalOptions> options, IHttpClientFactory http, ILogger<ArchiveStore> log)
{
    readonly R2Options r2 = options.Value.R2;
    readonly PortalOptions o = options.Value;

    public bool Enabled => r2.Enabled;

    public string PublicUrl(string channel, string archive) =>
        $"{r2.PublicUrl.TrimEnd('/')}/{string.Join('/', Key(channel, archive).Split('/').Select(Uri.EscapeDataString))}";

    /// The object's key, raw; only the public address is escaped.
    string Key(string channel, string archive) => $"{r2.Prefix}{channel}/archives/{archive}";

    IAmazonS3 Client() => new AmazonS3Client(new BasicAWSCredentials(r2.AccessKeyId, r2.SecretAccessKey),
        new AmazonS3Config
        {
            ServiceURL = $"https://{r2.AccountId}.r2.cloudflarestorage.com",
            AuthenticationRegion = "auto",
            ForcePathStyle = true,
            RequestChecksumCalculation = RequestChecksumCalculation.WHEN_REQUIRED,
            ResponseChecksumValidation = ResponseChecksumValidation.WHEN_REQUIRED,
        });

    /// Upload, read back, then leave only the .url on the portal. Returns why
    /// not, or null on success; the local file stays whenever anything fails.
    public async Task<string?> Offload(string channel, string archive, CancellationToken ct)
    {
        var local = Path.Combine(o.ChannelsDir, channel, "archives", archive);
        var shaFile = local + ".sha256";
        if (!File.Exists(local)) return "the archive is not on the portal's disk";
        if (!File.Exists(shaFile)) return "the archive has no .sha256";
        var want = (await File.ReadAllTextAsync(shaFile, ct)).Split(' ')[0].Trim();
        var size = new FileInfo(local).Length;
        var key = Key(channel, archive);
        using var s3 = Client();
        try
        {
            await new TransferUtility(s3).UploadAsync(new TransferUtilityUploadRequest
            {
                BucketName = r2.Bucket, Key = key, FilePath = local, PartSize = 32L * 1024 * 1024,
                ContentType = Channels.ChannelPaths.ContentType("archives/" + archive),
                DisablePayloadSigning = true,
            }, ct);
            var head = await s3.GetObjectMetadataAsync(r2.Bucket, key, ct);
            if (head.ContentLength != size)
                return $"R2 holds {head.ContentLength} bytes where the archive has {size}";
        }
        catch (AmazonServiceException e)
        {
            return $"R2 refused the upload: {e.Message}";
        }

        // Read it back as a client will, through the public address.
        var url = PublicUrl(channel, archive);
        using (var resp = await http.CreateClient("r2").GetAsync(url, HttpCompletionOption.ResponseHeadersRead, ct))
        {
            if (!resp.IsSuccessStatusCode) return $"the public address answered {(int)resp.StatusCode}";
            await using var body = await resp.Content.ReadAsStreamAsync(ct);
            var got = Convert.ToHexString(await SHA256.HashDataAsync(body, ct)).ToLowerInvariant();
            if (got != want) return $"the copy in R2 reads back as {got}, not {want}";
        }
        var tmp = local + ".url.tmp";
        await File.WriteAllTextAsync(tmp, url + "\n", ct);
        File.Move(tmp, local + ".url", overwrite: true);
        File.Delete(local);
        log.LogInformation("archive {Channel}/{Archive} now in R2 at {Url}; {Size} bytes freed on disk", channel, archive, url, size);
        return null;
    }
}
