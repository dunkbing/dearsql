#include "database/s3_client.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <format>
#include <miniocpp/client.h>
#include <random>

namespace {
    // the provider must outlive the client, which keeps a pointer to it
    struct Session {
        minio::creds::StaticProvider provider;
        minio::s3::BaseUrl url;
        minio::s3::Client client;

        explicit Session(const S3Config& c)
            : provider(c.accessKey, c.secretKey),
              url(c.endpoint.empty() ? "s3.amazonaws.com" : c.endpoint, c.https,
                  c.region.empty() ? "us-east-1" : c.region),
              client(checked(url), &provider) {}

        static minio::s3::BaseUrl& checked(minio::s3::BaseUrl& url) {
            if (!url)
                throw S3Error("Invalid endpoint: " + url.Error().String());
            return url;
        }
    };

    template <typename R> R check(R resp) {
        if (!resp) {
            std::string msg = resp.Error().String();
            if (msg.empty())
                msg = resp.message.empty() ? "S3 request failed" : resp.message;
            throw S3Error(msg);
        }
        return resp;
    }

    // 2026-11-07T10:20:30.000Z -> 2026-11-07 10:20:30
    std::string formatTime(const minio::utils::UtcTime& t) {
        std::string s = t.ToISO8601UTC();
        if (s.size() >= 19) {
            s.resize(19);
            s[10] = ' ';
        }
        return s;
    }

    minio::http::ProgressFunction progressFn(const S3Client::Progress& progress, bool upload) {
        if (!progress)
            return nullptr;
        return [progress, upload](minio::http::ProgressFunctionArgs a) {
            return upload ? progress(a.uploaded_bytes, a.upload_total_bytes)
                          : progress(a.downloaded_bytes, a.download_total_bytes);
        };
    }
} // namespace

S3Client::S3Client(S3Config config) : config_(std::move(config)) {
    // a pasted url: keep the host, take https from the scheme
    for (const auto* scheme : {"https://", "http://"}) {
        if (config_.endpoint.starts_with(scheme)) {
            config_.https = std::string_view(scheme) == "https://";
            config_.endpoint.erase(0, std::strlen(scheme));
        }
    }
    while (config_.endpoint.ends_with('/'))
        config_.endpoint.pop_back();
}

std::vector<S3Bucket> S3Client::listBuckets() const {
    Session s(config_);
    auto resp = check(s.client.ListBuckets());
    std::vector<S3Bucket> out;
    for (const auto& b : resp.buckets)
        out.push_back({b.name, formatTime(b.creation_date)});
    return out;
}

void S3Client::createBucket(const std::string& bucket) const {
    Session s(config_);
    minio::s3::MakeBucketArgs args;
    args.bucket = bucket;
    args.region = s.url.region;
    check(s.client.MakeBucket(args));
}

void S3Client::deleteBucket(const std::string& bucket) const {
    Session s(config_);
    minio::s3::RemoveBucketArgs args;
    args.bucket = bucket;
    check(s.client.RemoveBucket(args));
}

S3Listing S3Client::listObjects(const std::string& bucket, const std::string& prefix,
                                const std::string& token) const {
    Session s(config_);
    minio::s3::ListObjectsArgs base;
    base.bucket = bucket;
    base.prefix = prefix;
    base.delimiter = "/";
    base.use_url_encoding_type = false;
    minio::s3::ListObjectsV2Args args(base);
    args.continuation_token = token;
    auto resp = check(s.client.ListObjectsV2(args));

    S3Listing out;
    for (const auto& item : resp.contents) {
        if (item.is_prefix)
            out.prefixes.push_back(item.name);
        else if (item.name != prefix) // the folder's own marker object
            out.objects.push_back({item.name, item.size, formatTime(item.last_modified)});
    }
    if (resp.is_truncated)
        out.nextToken = resp.next_continuation_token;
    return out;
}

std::vector<std::pair<std::string, std::string>> S3Client::stat(const std::string& bucket,
                                                                const std::string& key) const {
    Session s(config_);
    minio::s3::StatObjectArgs args;
    args.bucket = bucket;
    args.object = key;
    auto resp = check(s.client.StatObject(args));
    std::vector<std::pair<std::string, std::string>> out = {
        {"Size", std::to_string(resp.size) + " bytes"},
        {"Last modified", formatTime(resp.last_modified)},
        {"ETag", resp.etag},
    };
    if (auto type = resp.headers.GetFront("content-type"); !type.empty())
        out.emplace_back("Content type", type);
    if (!resp.version_id.empty())
        out.emplace_back("Version", resp.version_id);
    for (const auto& header : resp.user_metadata.ToHttpHeaders()) {
        const auto colon = header.find(": ");
        if (colon != std::string::npos)
            out.emplace_back(header.substr(0, colon), header.substr(colon + 2));
    }
    return out;
}

void S3Client::download(const std::string& bucket, const std::string& key, const std::string& path,
                        const Progress& progress) const {
    Session s(config_);
    minio::s3::DownloadObjectArgs args;
    args.bucket = bucket;
    args.object = key;
    args.filename = path;
    args.overwrite = true;
    args.progressfunc = progressFn(progress, false);
    check(s.client.DownloadObject(args));
}

void S3Client::upload(const std::string& bucket, const std::string& key, const std::string& path,
                      const Progress& progress) const {
    Session s(config_);
    minio::s3::UploadObjectArgs args;
    args.bucket = bucket;
    args.object = key;
    args.filename = path;
    args.progressfunc = progressFn(progress, true);
    check(s.client.UploadObject(args));
}

std::string S3Client::readHead(const std::string& bucket, const std::string& key,
                               std::size_t maxBytes) const {
    Session s(config_);
    minio::s3::GetObjectArgs args;
    args.bucket = bucket;
    args.object = key;
    std::size_t offset = 0;
    std::size_t length = maxBytes;
    args.offset = &offset;
    args.length = &length;
    std::string out;
    args.datafunc = [&](minio::http::DataFunctionArgs a) {
        out += a.datachunk;
        return true;
    };
    auto resp = s.client.GetObject(args);
    // an empty object has no byte range to return
    if (!resp && resp.code != "InvalidRange")
        check(std::move(resp));
    if (out.size() > maxBytes)
        out.resize(maxBytes);
    return out;
}

void S3Client::deleteObject(const std::string& bucket, const std::string& key) const {
    Session s(config_);
    minio::s3::RemoveObjectArgs args;
    args.bucket = bucket;
    args.object = key;
    check(s.client.RemoveObject(args));
}

void S3Client::copyObject(const std::string& srcBucket, const std::string& srcKey,
                          const std::string& dstBucket, const std::string& dstKey) const {
    Session s(config_);
    minio::s3::CopyObjectArgs args;
    args.bucket = dstBucket;
    args.object = dstKey;
    args.source.bucket = srcBucket;
    args.source.object = srcKey;
    auto resp = s.client.CopyObject(args);
    if (resp)
        return;
    // minio-cpp percent-encodes the slashes of x-amz-copy-source; aws decodes them, some
    // gateways (versitygw) do not and reject the source. copy through a temp file there
    std::string msg = resp.Error().String();
    std::ranges::transform(msg, msg.begin(), [](unsigned char c) { return std::tolower(c); });
    if (msg.find("copy source") == std::string::npos)
        check(std::move(resp));
    const auto tmp = std::filesystem::temp_directory_path() /
                     std::format("dearsql-s3-copy-{}-{}", std::random_device{}(),
                                 std::hash<std::string>{}(srcKey));
    struct Remove {
        std::filesystem::path p;
        ~Remove() {
            std::error_code ec;
            std::filesystem::remove(p, ec);
        }
    } cleanup{tmp};
    download(srcBucket, srcKey, tmp.string());
    upload(dstBucket, dstKey, tmp.string());
}

int S3Client::copyPrefix(const std::string& srcBucket, const std::string& srcPrefix,
                         const std::string& dstBucket, const std::string& dstPrefix) const {
    Session s(config_);
    minio::s3::ListObjectsArgs list;
    list.bucket = srcBucket;
    list.prefix = srcPrefix;
    list.recursive = true;
    list.use_url_encoding_type = false;
    std::vector<std::string> keys;
    for (auto it = s.client.ListObjects(list); it; it++) {
        const auto& item = *it;
        if (!item)
            throw S3Error(item.Error().String());
        keys.push_back(item.name);
    }
    int copied = 0;
    for (const auto& key : keys) {
        // folder markers come back on their own on file-backed gateways; copy files only
        if (key.ends_with('/'))
            continue;
        copyObject(srcBucket, key, dstBucket, dstPrefix + key.substr(srcPrefix.size()));
        ++copied;
    }
    return copied;
}

int S3Client::deletePrefix(const std::string& bucket, const std::string& prefix) const {
    Session s(config_);
    minio::s3::ListObjectsArgs list;
    list.bucket = bucket;
    list.prefix = prefix;
    list.recursive = true;
    list.use_url_encoding_type = false; // minio-cpp leaves "+" for a space undecoded
    std::vector<std::string> keys;
    for (auto it = s.client.ListObjects(list); it; it++) {
        const auto& item = *it;
        if (!item)
            throw S3Error(item.Error().String());
        keys.push_back(item.name);
    }

    // folder markers ("dir/" objects, or real directories on file-backed gateways such as
    // VersityGW) go after the files, deepest first: a gateway refuses a non-empty dir and
    // may run a batch in any order
    std::vector<std::string> files;
    std::vector<std::string> markers;
    for (auto& k : keys)
        (k.ends_with('/') ? markers : files).push_back(std::move(k));
    if (prefix.ends_with('/') && std::ranges::find(markers, prefix) == markers.end())
        markers.push_back(prefix); // deleting a missing key succeeds
    std::ranges::sort(markers, std::greater{});

    if (!files.empty()) {
        minio::s3::RemoveObjectsArgs args;
        args.bucket = bucket;
        std::size_t next = 0;
        args.func = [&](minio::s3::DeleteObject& obj) {
            if (next >= files.size())
                return false;
            obj.name = files[next++];
            return true;
        };
        // the result yields one entry per failed key
        for (auto it = s.client.RemoveObjects(args); it; it++) {
            const auto& err = *it;
            throw S3Error(err.Error() ? err.Error().String()
                                      : err.object_name + ": " + err.message);
        }
    }
    for (const auto& marker : markers) {
        minio::s3::RemoveObjectArgs args;
        args.bucket = bucket;
        args.object = marker;
        check(s.client.RemoveObject(args));
    }
    return static_cast<int>(files.size());
}

std::string S3Client::presignGet(const std::string& bucket, const std::string& key,
                                 unsigned expiresSeconds) const {
    Session s(config_);
    minio::s3::GetPresignedObjectUrlArgs args;
    args.bucket = bucket;
    args.object = key;
    args.method = minio::http::Method::kGet;
    args.expiry_seconds = expiresSeconds;
    return check(s.client.GetPresignedObjectUrl(args)).url;
}
