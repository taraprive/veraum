#pragma once

#include <string>
#include <utility>
#include <vector>

namespace hftarb {

struct HttpResponse {
    long status = 0;
    std::string body;
    std::string error;
    bool ok() const { return status >= 200 && status < 300; }
};

using HeaderList = std::vector<std::pair<std::string, std::string>>;

// Minimal blocking HTTPS client over Windows WinHTTP (Schannel TLS, no
// third-party library). One WinHTTP session is opened per request, so a single
// instance is safe to use from one thread at a time; the exchange adapters
// serialize their calls internally.
class HttpClient {
public:
    explicit HttpClient(std::string baseUrl);

    HttpResponse get(const std::string& path, const HeaderList& headers = {}) const;
    HttpResponse post(const std::string& path, const std::string& contentType,
                      const std::string& body, const HeaderList& headers = {}) const;
    HttpResponse del(const std::string& path, const HeaderList& headers = {}) const;

private:
    HttpResponse request(const char* verb, const std::string& path,
                         const std::string& contentType, const std::string& body,
                         const HeaderList& headers) const;

    std::string host_;
    std::string basePath_;
    unsigned short port_ = 443;
    bool useTls_ = true;
};

}  // namespace hftarb
