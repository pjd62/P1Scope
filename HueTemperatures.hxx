#pragma once

#include <atomic>
#include <deque>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

struct TemperaturePoint {
    double time;
    double celsius;
};
struct TemperatureSensor {
    std::string id;
    std::string name;
    std::deque<TemperaturePoint> history;
};
struct HueLight {
    std::string id;
    std::string name;
    bool on = false;
    double brightness = -1; // Unavailable for lights without dimming.
    std::string room = "Unassigned";
    std::string roomId;
    bool reachable = true;
};
struct HueSnapshot {
    std::string pendingLight;
    std::string lightActionStatus;
    std::string lightsStatus = "Connecting to Hue...";
    bool lightsAvailable = false;
    std::vector<HueLight> lights;
    std::string status = "Connecting to Hue...";
    std::vector<TemperatureSensor> sensors;
};

class HueTemperatures {
public:
    explicit HueTemperatures(bool simulation);
    ~HueTemperatures();
    HueSnapshot snapshot();
    std::string setLight(const std::string &id, bool on); // Empty string means queued.
    std::string setBrightness(const std::string &id, double brightness);
private:
    std::string queueLight(const std::string &id, std::optional<bool> on, double brightness);
    void poll(bool simulation);
    std::atomic<bool> stopping{false};
    std::mutex mutex;
    HueSnapshot state;
    std::condition_variable wake;
    std::string commandId;
    std::optional<bool> commandOn;
    double commandBrightness = -1;
    std::thread worker;
};
