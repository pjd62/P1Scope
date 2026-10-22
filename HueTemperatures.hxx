#pragma once

#include <atomic>
#include <deque>
#include <mutex>
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
struct HueSnapshot {
    std::string status = "Connecting to Hue...";
    std::vector<TemperatureSensor> sensors;
};

class HueTemperatures {
public:
    explicit HueTemperatures(bool simulation);
    ~HueTemperatures();
    HueSnapshot snapshot();
private:
    void poll(bool simulation);
    std::atomic<bool> stopping{false};
    std::mutex mutex;
    HueSnapshot state;
    std::thread worker;
};
