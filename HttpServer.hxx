#pragma once
#include <atomic>
#include <functional>
#include <string>

// Small LAN HTTP server. Only explicitly listed assets and API routes are exposed.
int serveDashboard(const std::string &bindAddress, unsigned short port,
    const std::string &webRoot, std::atomic<bool> &running,
    const std::function<bool()> &stopRequested,
    const std::function<std::string()> &snapshot,
    const std::function<std::string()> &reloadGroups);
