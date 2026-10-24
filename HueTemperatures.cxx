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
    wake.notify_all();
    worker.join();
}
HueSnapshot HueTemperatures::snapshot() {
    std::lock_guard lock(mutex);
    return state;
}
std::string HueTemperatures::setLight(const std::string &id, bool on) {
    return queueLight(id, on, -1);
}
std::string HueTemperatures::setBrightness(const std::string &id, double brightness) {
    if (!std::isfinite(brightness) || brightness < 0 || brightness > 100) return "Brightness must be between 0 and 100";
    return queueLight(id, std::nullopt, brightness);
}
std::string HueTemperatures::queueLight(const std::string &id, std::optional<bool> on, double brightness) {
    std::lock_guard lock(mutex);
    if (!state.lightsAvailable) return "Hue lights unavailable";
    if (!state.pendingLight.empty()) return "Another light change is in progress";
    if (id.empty() || id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") != std::string::npos ||
        std::none_of(state.lights.begin(), state.lights.end(), [&](const auto &light) { return light.id == id; }))
        return "Unknown light";
    if (std::any_of(state.lights.begin(), state.lights.end(), [&](const auto &light) {
        return light.id == id && !light.reachable;
    })) return "Light is unavailable";
    if (!on && std::any_of(state.lights.begin(), state.lights.end(), [&](const auto &light) {
        return light.id == id && light.brightness < 0;
    })) return "This light does not support dimming";
    commandId = id;
    commandOn = on;
    commandBrightness = brightness;
    state.pendingLight = id;
    state.lightActionStatus = "Changing light...";
    wake.notify_all();
    return {};
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
            state.lightsStatus = state.status;
            logStderr() << state.status << '\n';
            return;
        }
    }
    CURL *curl = simulation ? nullptr : curl_easy_init();
    if (!simulation && !curl) {
        std::lock_guard lock(mutex);
        state.status = "Hue HTTP initialization failed";
        state.lightsStatus = state.status;
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
            state.lightsStatus = state.status;
            logStderr() << state.status << '\n';
            return;
        }
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 4000L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        // Same local-bridge certificate handling as the pairing script.
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    auto request = [&](const std::string &resource, const std::string &payload = "") {
        std::string body;
        const auto url = base + resource;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, payload.empty() ? "GET" : "PUT");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.empty() ? nullptr : payload.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        const auto code = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (code != CURLE_OK)
            throw HueRequestError(std::string("request failed: ") + curl_easy_strerror(code));
        if (status == 401 || status == 403)
            throw HueRequestError("pairing rejected; run hue_temperatures.py --pair and restart");
        if (status != 200) throw HueRequestError("HTTP status " + std::to_string(status));
        const auto result = json::parse(body);
        if (!result.at("errors").empty() || !result.at("data").is_array())
            throw HueRequestError("API error");
        return result.at("data");
    };
    while (!stopping) {
        // Keep light failures independent of temperature collection.
        std::vector<HueLight> lights;
        bool lightsAvailable = false;
        std::string lightsStatus;
        try {
            if (simulation) {
                lights = {{"sim-1", "Living room (simulated)", true, 75, "Living room (simulated)", "room-living"},
                          {"sim-2", "Hall (simulated)", false, 40, "Hall (simulated)", "room-hall"},
                          {"sim-3", "Kitchen (simulated)", true, 100, "Kitchen (simulated)", "room-kitchen"}};
            } else {
                const auto rooms = request("room");
                const auto connectivity = request("zigbee_connectivity");
                for (const auto &light : request("light")) {
                    HueLight entry{light.at("id").get<std::string>(),
                        light.at("metadata").at("name").get<std::string>(),
                        light.at("on").at("on").get<bool>()};
                    if (light.contains("dimming")) {
                        const double value = light.at("dimming").at("brightness").get<double>();
                        if (std::isfinite(value) && value >= 0 && value <= 100) entry.brightness = value;
                    }
                    // Hue rooms contain devices; light resources reference their owning device.
                    const auto owner = light.at("owner").at("rid").get<std::string>();
                    entry.reachable = false;
                    for (const auto &connection : connectivity)
                        if (connection.at("owner").at("rid") == owner && connection.value("status", std::string{}) == "connected")
                            entry.reachable = true;
                    for (const auto &room : rooms) {
                        for (const auto &child : room.at("children")) {
                            const auto type = child.value("rtype", std::string{});
                            const auto id = child.at("rid").get<std::string>();
                            if ((type == "device" && id == owner) || (type == "light" && id == entry.id)) {
                                entry.room = room.at("metadata").at("name").get<std::string>();
                                entry.roomId = room.at("id").get<std::string>();
                            }
                        }
                    }
                    lights.push_back(std::move(entry));
                }
            }
            std::sort(lights.begin(), lights.end(), [](const auto &a, const auto &b) {
                if (a.roomId.empty() != b.roomId.empty()) return !a.roomId.empty();
                if (a.room != b.room) return a.room < b.room;
                if (a.roomId != b.roomId) return a.roomId < b.roomId;
                return a.name == b.name ? a.id < b.id : a.name < b.name;
            });
            lightsAvailable = true;
            lightsStatus = simulation ? "Simulated lights" : "Updated every 30 seconds";
        } catch (const HueRequestError &failure) {
            lightsStatus = std::string("Hue lights unavailable: ") + failure.what() + "; retrying";
        } catch (...) {
            lightsStatus = "Hue lights unavailable: invalid bridge response; retrying";
        }
        {
            std::lock_guard lock(mutex);
            state.lightsAvailable = lightsAvailable;
            state.lightsStatus = lightsStatus;
            if (lightsAvailable) {
                if (simulation && !state.lights.empty()) lights = state.lights;
                state.lights = std::move(lights);
            }
        }
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
                const auto devices = request("device");
                const auto temperatures = request("temperature");
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
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        std::unique_lock lock(mutex);
        while (!stopping) {
            wake.wait_until(lock, deadline, [&] { return stopping || !commandId.empty(); });
            if (stopping) break;
            if (commandId.empty()) break;
            const auto id = commandId;
            const auto on = commandOn;
            const double brightness = commandBrightness;
            commandId.clear();
            lock.unlock();
            std::string error;
            try {
                if (!simulation) {
                    json payload;
                    if (on.has_value()) payload = {{"on", {{"on", *on}}}};
                    else payload = {{"dimming", {{"brightness", brightness}}}};
                    const auto updated = request("light/" + id, payload.dump());
                    if (std::none_of(updated.begin(), updated.end(), [&](const auto &item) {
                        return item.value("rid", std::string{}) == id;
                    })) throw HueRequestError("bridge did not acknowledge this light");
                }
            } catch (const HueRequestError &failure) { error = failure.what(); }
            catch (...) { error = "invalid bridge response"; }
            lock.lock();
            if (error.empty()) {
                for (auto &light : state.lights) if (light.id == id) {
                    if (on.has_value()) light.on = *on;
                    else light.brightness = brightness;
                }
                state.lightActionStatus = simulation ? "Simulated light changed" : "Light changed";
            } else state.lightActionStatus = "Could not change light: " + error;
            state.pendingLight.clear();
            if (std::chrono::steady_clock::now() >= deadline) break;
        }
    }
    if (headers) curl_slist_free_all(headers);
    if (curl) curl_easy_cleanup(curl);
}
