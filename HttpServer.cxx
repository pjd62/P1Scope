#include "HttpServer.hxx"
#include "Logging.hxx"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <map>
#include <sstream>

namespace {
struct Socket {
    int fd;
    ~Socket() { if (fd >= 0) close(fd); }
};
bool sendAll(int fd, const std::string &data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const auto n = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}
void respond(int fd, const char *status, const char *type, const std::string &body) {
    const std::string header = std::string("HTTP/1.1 ") + status +
        "\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
        "Content-Security-Policy: default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
        "connect-src 'self'; img-src 'self' data:; frame-ancestors 'none'; base-uri 'none'\r\n\r\n";
    if (sendAll(fd, header)) sendAll(fd, body);
}
}
int serveDashboard(const std::string &bindAddress, unsigned short port,
    const std::string &webRoot, std::atomic<bool> &running,
    const std::function<bool()> &stopRequested,
    const std::function<std::string()> &snapshot,
    const std::function<std::string()> &reloadGroups) {
    Socket listener{socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    if (listener.fd < 0) { logStderr() << "Cannot create HTTP socket\n"; return 1; }
    int reuse = 1;
    setsockopt(listener.fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, bindAddress.c_str(), &address.sin_addr) != 1 ||
        bind(listener.fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
        listen(listener.fd, 16) != 0) {
        logStderr() << "Cannot listen on " << bindAddress << ':' << port << '\n';
        return 1;
    }
    logStderr() << "Dashboard listening at http://" << bindAddress << ':' << port << '\n';
    const std::map<std::string, std::pair<std::string, const char *>> assets = {
        {"/", {"index.html", "text/html; charset=utf-8"}},
        {"/index.html", {"index.html", "text/html; charset=utf-8"}},
        {"/dashboard.js", {"dashboard.js", "text/javascript; charset=utf-8"}},
        {"/dashboard.css", {"dashboard.css", "text/css; charset=utf-8"}},
        {"/vendor/d3.min.js", {"vendor/d3.min.js", "text/javascript; charset=utf-8"}}
    };
    while (running && !stopRequested()) {
        pollfd wait{listener.fd, POLLIN, 0};
        if (poll(&wait, 1, 200) <= 0) continue;
        Socket client{accept4(listener.fd, nullptr, nullptr, SOCK_CLOEXEC)};
        if (client.fd < 0) continue;
        timeval timeout{2, 0};
        setsockopt(client.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(client.fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        std::string request;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        char buffer[2048];
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192 &&
               std::chrono::steady_clock::now() < deadline && running && !stopRequested()) {
            const auto n = recv(client.fd, buffer, sizeof(buffer), 0);
            if (n <= 0) break;
            request.append(buffer, static_cast<size_t>(n));
        }
        if (request.size() >= 8192 || request.find("\r\n\r\n") == std::string::npos) {
            respond(client.fd, "400 Bad Request", "text/plain", "Invalid request"); continue;
        }
        std::istringstream input(request);
        std::string method, path, protocol;
        input >> method >> path >> protocol;
        if (method == "GET" && path == "/api/state") {
            respond(client.fd, "200 OK", "application/json", snapshot());
        } else if (method == "POST" && path == "/api/groups/reload") {
            // A custom header prevents cross-origin form submissions; no CORS is enabled.
            std::string lower = request;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
            if (lower.find("\r\nx-p1scope-action: reload\r\n") == std::string::npos)
                respond(client.fd, "403 Forbidden", "text/plain", "Missing action header");
            else respond(client.fd, "200 OK", "application/json", reloadGroups());
        } else if (method == "GET") {
            const auto asset = assets.find(path);
            if (asset == assets.end()) { respond(client.fd, "404 Not Found", "text/plain", "Not found"); continue; }
            std::ifstream file(webRoot + '/' + asset->second.first, std::ios::binary);
            if (!file) { respond(client.fd, "503 Service Unavailable", "text/plain", "Dashboard assets missing"); continue; }
            const std::string body((std::istreambuf_iterator<char>(file)), {});
            respond(client.fd, "200 OK", asset->second.second, body);
        } else respond(client.fd, "405 Method Not Allowed", "text/plain", "Method not allowed");
    }
    return 0;
}
