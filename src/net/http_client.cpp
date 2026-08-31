#include "src/net/http_client.h"

#include <windows.h>
#include <winhttp.h>

#include <cstring>

namespace hftarb {

namespace {

void parseBase(const std::string& baseUrl, std::string& host, std::string& basePath,
               unsigned short& port, bool& useTls) {
    std::string s = baseUrl;
    useTls = true;
    const std::string httpsScheme = "https://";
    const std::string httpScheme = "http://";
    if (s.rfind(httpsScheme, 0) == 0) {
        s = s.substr(httpsScheme.size());
        useTls = true;
    } else if (s.rfind(httpScheme, 0) == 0) {
        s = s.substr(httpScheme.size());
        useTls = false;
    }
    const std::string pathPrefix = "/";
    auto slash = s.find('/');
    if (slash != std::string::npos) {
        basePath = s.substr(slash);
        if (!basePath.empty() && basePath.back() == '/') basePath.pop_back();
        s = s.substr(0, slash);
    }
    port = useTls ? 443 : 80;
    auto colon = s.find(':');
    if (colon != std::string::npos) {
        port = static_cast<unsigned short>(std::stoi(s.substr(colon + 1)));
        s = s.substr(0, colon);
    }
    host = s;
}

std::wstring toWide(const std::string& s) {
    std::wstring w;
    w.reserve(s.size());
    for (unsigned char c : s) w.push_back(static_cast<wchar_t>(c));
    return w;
}

std::string lastWinError(const char* what) {
    DWORD code = GetLastError();
    return std::string(what) + " (winerr " + std::to_string(code) + ")";
}

}  // namespace

HttpClient::HttpClient(std::string baseUrl) {
    parseBase(std::move(baseUrl), host_, basePath_, port_, useTls_);
}

HttpResponse HttpClient::get(const std::string& path, const HeaderList& headers) const {
    return request("GET", path, "", "", headers);
}

HttpResponse HttpClient::post(const std::string& path, const std::string& contentType,
                              const std::string& body, const HeaderList& headers) const {
    return request("POST", path, contentType, body, headers);
}

HttpResponse HttpClient::del(const std::string& path, const HeaderList& headers) const {
    return request("DELETE", path, "", "", headers);
}

HttpResponse HttpClient::request(const char* verb, const std::string& path,
                                 const std::string& contentType, const std::string& body,
                                 const HeaderList& headers) const {
    HttpResponse resp;

    HINTERNET session = WinHttpOpen(L"HFT-Arbitrage/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        resp.error = lastWinError("WinHttpOpen");
        return resp;
    }
    WinHttpSetTimeouts(session, 10000, 10000, 10000, 15000);

    HINTERNET conn = WinHttpConnect(session, toWide(host_).c_str(), port_, 0);
    if (!conn) {
        resp.error = lastWinError("WinHttpConnect");
        WinHttpCloseHandle(session);
        return resp;
    }

    std::string objectName = basePath_ + path;
    HINTERNET req = WinHttpOpenRequest(conn, toWide(verb).c_str(), toWide(objectName).c_str(),
                                       nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       useTls_ ? WINHTTP_FLAG_SECURE : 0);
    if (!req) {
        resp.error = lastWinError("WinHttpOpenRequest");
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return resp;
    }

    std::wstring headerBlock;
    if (!contentType.empty()) headerBlock += L"Content-Type: " + toWide(contentType) + L"\r\n";
    for (const auto& h : headers) {
        headerBlock += toWide(h.first) + L": " + toWide(h.second) + L"\r\n";
    }

    const char* bodyPtr = body.empty() ? nullptr : body.data();
    BOOL sent = WinHttpSendRequest(req,
                                   headerBlock.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                                       : headerBlock.c_str(),
                                   headerBlock.empty() ? 0 : (DWORD)-1,
                                   const_cast<char*>(bodyPtr),
                                   body.empty() ? 0 : (DWORD)body.size(),
                                   body.empty() ? 0 : (DWORD)body.size(),
                                   0);
    if (!sent) {
        resp.error = lastWinError("WinHttpSendRequest");
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return resp;
    }

    if (!WinHttpReceiveResponse(req, nullptr)) {
        resp.error = lastWinError("WinHttpReceiveResponse");
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return resp;
    }

    DWORD status = 0;
    DWORD statusLen = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusLen,
                        WINHTTP_NO_HEADER_INDEX);
    resp.status = static_cast<long>(status);

    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
        std::string chunk;
        chunk.resize(avail);
        DWORD read = 0;
        if (!WinHttpReadData(req, &chunk[0], avail, &read)) break;
        chunk.resize(read);
        resp.body += chunk;
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return resp;
}

}  // namespace hftarb
