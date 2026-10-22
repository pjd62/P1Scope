#include "HueTemperatures.hxx"
#include "HeatingSimulation.hxx"
#include "Logging.hxx"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <stdexcept>

using json = nlohmann::json;
namespace {
struct HueRequestError : std::runtime_error { using std::runtime_error::runtime_error; };
double nowSeconds() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}
size_t receive(char *data, size_t size, size_t count, void *target) {
    try { static_cast<std::string *>(target)->append(data, size * count); }
    catch (...) { return 0; }
    return size * count;
}
}
HueTemperatures::HueTemperatures(bool simulation) : worker([this, simulation] { poll(simulation); }) {}
HueTemperatures::~HueTemperatures() {
    stopping = true;
    worker.join();
}
HueSnapshot HueTemperatures::snapshot() {
    std::lock_guard lock(mutex);
    return state;
}
void HueTemperatures::poll(bool simulation) {
    std::string base, key, address;
    if (!simulation) {
        try {
            std::ifstream file(P1SCOPE_HUE_CREDENTIALS);
            if (!file) throw std::runtime_error("Hue not configured; pair using hue_temperatures.py --pair");
            const auto credentials = json::parse(file);
            address = credentials.at("bridge_ip").get<std::string>();
            key = credentials.at("application_key").get<std::string>();
            // Restrict the credentials to a literal IPv4 host and a single header value.
            if (address.empty() || address.find_first_not_of("0123456789.") != std::string::npos ||
                key.empty() || key.find_first_of("\r\n") != std::string::npos)
                throw std::runtime_error("Invalid Hue credentials");
            base = "https://" + address + "/clip/v2/resource/";
            logStderr() << "Hue: connecting to configured bridge at " << address << '\n';
        } catch (...) {
            std::lock_guard lock(mutex);
            state.status = "Hue unavailable: check .hue-credentials.json; pair using hue_temperatures.py --pair";
            logStderr() << state.status << '\n';
            return;
        }
    }
    CURL *curl = simulation ? nullptr : curl_easy_init();
    if (!simulation && !curl) {
        std::lock_guard lock(mutex);
        state.status = "Hue HTTP initialization failed";
        logStderr() << state.status << '\n';
        return;
    }
    curl_slist *headers = nullptr;
    if (curl) {
        headers = curl_slist_append(nullptr, ("hue-application-key: " + key).c_str());
        if (!headers) {
            curl_easy_cleanup(curl);
            std::lock_guard lock(mutex);
            state.status = "Hue HTTP initialization failed";
            logStderr() << state.status << '\n';
            return;
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 4000L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        // Same local-bridge certificate handling as the pairing script.
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    auto get = [&](const char *resource) {
        std::string body;
        const auto url = base + resource;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        const auto code = curl_easy_perform(curl);
        if (code != CURLE_OK)
            throw HueRequestError(std::string("request failed: ") + curl_easy_strerror(code));
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (status == 401 || status == 403)
            throw HueRequestError("pairing rejected; run hue_temperatures.py --pair and restart");
        if (status != 200) throw HueRequestError("HTTP status " + std::to_string(status));
        const auto result = json::parse(body);
        if (!result.at("errors").empty() || !result.at("data").is_array())
            throw HueRequestError("API error");
        return result.at("data");
    };
    while (!stopping) {
        std::vector<TemperatureSensor> incoming;
        bool success = false;
        std::string error = "invalid bridge response";
        const double now = nowSeconds();
        try {
            if (simulation) {
                const char *names[] = {"Hal (simulated)", "Woonkamer (simulated)", "Badkamer (simulated)"};
                const double elapsed = HeatingSimulation::elapsed();
                for (int i = 0; i < 3; ++i)
                    incoming.push_back({std::to_string(i), names[i],
                        {{now, HeatingSimulation::roomTemperature(i, elapsed)}}});
            } else {
                const auto devices = get("device");
                const auto temperatures = get("temperature");
                for (const auto &sensor : temperatures) {
                    const auto id = sensor.at("id").get<std::string>();
                    const auto owner = sensor.at("owner").at("rid").get<std::string>();
                    std::string name = "Temperature sensor";
                    for (const auto &device : devices)
                        if (device.at("id") == owner) name = device.at("metadata").at("name").get<std::string>();
                    const auto &temperature = sensor.at("temperature");
                    double value = NAN;
                    if (temperature.value("temperature_valid", false) && temperature.contains("temperature") &&
                        temperature["temperature"].is_number()) {
                        const double reading = temperature["temperature"].get<double>();
                        if (std::isfinite(reading)) value = reading;
                    }
                    incoming.push_back({id, name, {{now, value}}});
                }
            }
            success = true;
        } catch (const HueRequestError &failure) {
            error = failure.what(); // Only messages constructed above, never response bodies or keys.
        } catch (...) {
            // Never include request headers, response bodies or credentials in diagnostics.
        }
        {
            std::lock_guard lock(mutex);
            const std::string nextStatus = success ? (simulation ? "Hue: simulated temperatures" :
                "Hue bridge connected at " + address + ": " + std::to_string(incoming.size()) +
                " temperature sensor(s) found (polled every 30 s)") :
                "Hue bridge " + address + " unavailable: " + error + "; retrying";
            if (nextStatus != state.status) {
                logStderr() << nextStatus << '\n';
                if (success && !simulation) {
                    for (const auto &sensor : incoming)
                        logStderr() << "  Hue temperature sensor: " << sensor.name << '\n';
                }
            }
            state.status = nextStatus;
            for (auto &sensor : state.sensors) {
                if (!sensor.history.empty() && now < sensor.history.back().time) sensor.history.clear();
                sensor.history.push_back({now, NAN});
            }
            if (success) for (auto &sensor : incoming) {
                auto found = std::find_if(state.sensors.begin(), state.sensors.end(),
                    [&](const auto &known) { return known.id == sensor.id; });
                if (found == state.sensors.end()) state.sensors.push_back(std::move(sensor));
                else {
                    found->name = sensor.name;
                    found->history.back().celsius = sensor.history.back().celsius;
                }
            }
            for (auto &sensor : state.sensors)
                while (!sensor.history.empty() && sensor.history.front().time < now - 3600.0)
                    sensor.history.pop_front();
        }
        for (int i = 0; i < 300 && !stopping; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (headers) curl_slist_free_all(headers);
    if (curl) curl_easy_cleanup(curl);
}
