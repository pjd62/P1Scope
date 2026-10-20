#ifndef HOMEWIZARD_P1_HOMEWIZARDDISCOVERY_HXX
#define HOMEWIZARD_P1_HOMEWIZARDDISCOVERY_HXX


#pragma once

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-common/simple-watch.h>

#include <atomic>
#include <cstdint>
#include <string>

class HomeWizardDiscovery {
public:
    HomeWizardDiscovery() = default;
    ~HomeWizardDiscovery();

    HomeWizardDiscovery(const HomeWizardDiscovery&) = delete;
    HomeWizardDiscovery& operator=(const HomeWizardDiscovery&) = delete;

    // Wait up to five seconds; stop early when running becomes false.
    bool discover(const std::atomic<bool>& running);

    [[nodiscard]] const std::string& ip() const {
        return _ip;
    }

    [[nodiscard]] uint16_t port() const {
        return _port;
    }

    [[nodiscard]] const std::string& hostname() const {
        return _hostname;
    }

private:
    static void browseCallback(
        AvahiServiceBrowser* browser,
        AvahiIfIndex interface,
        AvahiProtocol protocol,
        AvahiBrowserEvent event,
        const char* name,
        const char* type,
        const char* domain,
        AvahiLookupResultFlags flags,
        void* userdata);

    static void resolveCallback(
        AvahiServiceResolver* resolver,
        AvahiIfIndex interface,
        AvahiProtocol protocol,
        AvahiResolverEvent event,
        const char* name,
        const char* type,
        const char* domain,
        const char* hostName,
        const AvahiAddress* address,
        uint16_t port,
        AvahiStringList* txt,
        AvahiLookupResultFlags flags,
        void* userdata);

    void cleanup();

    AvahiSimplePoll* _simplePoll = nullptr;
    AvahiClient* _client = nullptr;
    AvahiServiceBrowser* _browser = nullptr;

    bool _found = false;
    bool _resolving = false;

    std::string _ip;
    std::string _hostname;
    uint16_t _port = 0;
};

#endif //HOMEWIZARD_P1_HOMEWIZARDDISCOVERY_HXX
