#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

// S3 API through minio-cpp: AWS, MinIO, Cloudflare R2, Wasabi, VersityGW, ...
// synchronous; every call builds its own minio client, so it is safe across threads.
// errors throw S3Error
struct S3Config {
    std::string endpoint; // host[:port]; empty = AWS (s3.amazonaws.com)
    bool https = true;
    std::string region; // empty = us-east-1 (R2: auto)
    std::string accessKey;
    std::string secretKey;
};

struct S3Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct S3Bucket {
    std::string name;
    std::string created;
};

struct S3Object {
    std::string key;
    std::uint64_t size = 0;
    std::string lastModified;
};

struct S3Listing {
    std::vector<std::string> prefixes; // "folders": full prefixes ending in '/'
    std::vector<S3Object> objects;
    std::string nextToken; // empty = last page
};

class S3Client {
public:
    explicit S3Client(S3Config config);

    std::vector<S3Bucket> listBuckets() const;
    void createBucket(const std::string& bucket) const;
    void deleteBucket(const std::string& bucket) const;

    // one page under prefix, split at '/' into folders and objects
    S3Listing listObjects(const std::string& bucket, const std::string& prefix,
                          const std::string& token = {}) const;
    // size, modified, etag, content type and user metadata, for the details panel
    std::vector<std::pair<std::string, std::string>> stat(const std::string& bucket,
                                                          const std::string& key) const;

    // done / total bytes; return false to abort
    using Progress = std::function<bool(double done, double total)>;
    void download(const std::string& bucket, const std::string& key, const std::string& path,
                  const Progress& progress = {}) const;
    // multipart for big files
    void upload(const std::string& bucket, const std::string& key, const std::string& path,
                const Progress& progress = {}) const;
    void deleteObject(const std::string& bucket, const std::string& key) const;
    // every object under prefix (a "folder"); returns how many went
    int deletePrefix(const std::string& bucket, const std::string& prefix) const;

    // a GET link anyone can open until it expires (at most 7 days)
    std::string presignGet(const std::string& bucket, const std::string& key,
                           unsigned expiresSeconds) const;

private:
    S3Config config_;
};
