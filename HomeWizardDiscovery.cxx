#include "HomeWizardDiscovery.hxx"
#include "Logging.hxx"
#include <avahi-common/address.h>
#include <avahi-common/error.h>
#include <chrono>
#include <iostream>

// First try to find the homewizard P1 meter using avahi
// Via commandline: avahi-browse -art | grep -i -A10 -B2 homewizard

HomeWizardDiscovery::~HomeWizardDiscovery()
{
    cleanup();
}

bool HomeWizardDiscovery::discover(const std::atomic<bool>& running)
{
    cleanup();

    _found = false;
    _resolving = false;
    _ip.clear();
    _hostname.clear();
    _port = 0;

    if (!running.load())
        return false;

    _simplePoll = avahi_simple_poll_new();

    if (!_simplePoll) {
        logStderr() << "Could not create Avahi poll object\n";
        return false;
    }

    int error = 0;

    _client = avahi_client_new(
        avahi_simple_poll_get(_simplePoll),
        AVAHI_CLIENT_NO_FAIL,
        nullptr,
        nullptr,
        &error);

    if (!_client) {
        logStderr() << "Could not create Avahi client: "
                  << avahi_strerror(error) << '\n';

        cleanup();
        return false;
    }

    _browser = avahi_service_browser_new(
        _client,
        AVAHI_IF_UNSPEC,
        AVAHI_PROTO_INET,       // IPv4 only
        "_hwenergy._tcp",
        nullptr,
        static_cast<AvahiLookupFlags>(0),
        browseCallback,
        this);

    if (!_browser) {
        logStderr() << "Could not create Avahi service browser\n";
        cleanup();
        return false;
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);

    while (!_found && running.load() &&
           std::chrono::steady_clock::now() < deadline) {
        // Short waits let application shutdown interrupt discovery promptly.
        const int result = avahi_simple_poll_iterate(_simplePoll, 100);
        if (result < 0) {
            logStderr() << "Avahi polling failed\n";
            break;
        }
        if (result > 0)
            break;
    }

    cleanup();

    return _found;
}

void HomeWizardDiscovery::browseCallback(
    AvahiServiceBrowser* browser,
    AvahiIfIndex interface,
    AvahiProtocol protocol,
    AvahiBrowserEvent event,
    const char* name,
    const char* type,
    const char* domain,
    AvahiLookupResultFlags flags,
    void* userdata)
{
    auto* self = static_cast<HomeWizardDiscovery*>(userdata);

    if (event != AVAHI_BROWSER_NEW)
        return;

    //
    // Prevent multiple resolvers being started for the same device.
    //
    if (self->_found || self->_resolving)
        return;

    self->_resolving = true;

    AvahiClient* client =
        avahi_service_browser_get_client(browser);

    AvahiServiceResolver* resolver =
        avahi_service_resolver_new(
            client,
            interface,
            protocol,
            name,
            type,
            domain,
            AVAHI_PROTO_INET,
            static_cast<AvahiLookupFlags>(0),
            resolveCallback,
            self);

    if (!resolver) {
        self->_resolving = false;
    }
}

void HomeWizardDiscovery::resolveCallback(
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
    void* userdata)
{
    auto* self = static_cast<HomeWizardDiscovery*>(userdata);

    if (event == AVAHI_RESOLVER_FOUND) {
        char ip[AVAHI_ADDRESS_STR_MAX];

        avahi_address_snprint(
            ip,
            sizeof(ip),
            address);

        self->_ip = ip;
        self->_hostname = hostName;
        self->_port = port;
        self->_found = true;

        //
        // We have what we need. Stop discover().
        //
        avahi_simple_poll_quit(self->_simplePoll);
    }
    else {
        self->_resolving = false;
    }

    avahi_service_resolver_free(resolver);
}

void HomeWizardDiscovery::cleanup()
{
    if (_browser) {
        avahi_service_browser_free(_browser);
        _browser = nullptr;
    }

    if (_client) {
        avahi_client_free(_client);
        _client = nullptr;
    }

    if (_simplePoll) {
        avahi_simple_poll_free(_simplePoll);
        _simplePoll = nullptr;
    }
}
