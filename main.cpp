#include "HomeWizardDiscovery.hxx"
#include "HttpServer.hxx"
#include <csignal>
#include <cstdlib>
#include <arpa/inet.h>
#include "HueTemperatures.hxx"
#include "HeatingSimulation.hxx"
#include "Logging.hxx"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#ifndef P1SCOPE_HEADLESS
#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"
#endif
#include <algorithm>
#include <cmath>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <vector>
#include <iostream>
#include <string>
#include <thread>
#include <deque>
#include <limits>
#include <cstring>
#include <random>
#include <fstream>
#include <set>
#include <stdexcept>

using namespace std;
using json = nlohmann::ordered_json; // Prevent dump to reorder

static bool printCompleteJson = false;
static bool simulation = false;
static std::string configuredMeterAddress;
static unsigned short configuredMeterPort = 80;

struct CircuitGroup {
    int number;
    std::string name;
    int phase; // 0 means the phase has not been assigned yet.
};

#ifndef P1SCOPE_GROUPS_CONFIG
#define P1SCOPE_GROUPS_CONFIG "groups.json"
#endif
static std::string groupsConfigPath = P1SCOPE_GROUPS_CONFIG;
static std::vector<CircuitGroup> circuitGroups;
static std::string groupsConfigError;

static bool loadGroupsConfig() {
    try {
        std::ifstream file(groupsConfigPath);
        if (!file)
            throw std::runtime_error("Cannot open configuration file");
        const json config = json::parse(file);
        if (!config.is_object() || !config.contains("groups") || !config["groups"].is_array())
            throw std::runtime_error("Expected an object with a 'groups' array");
        std::vector<CircuitGroup> next;
        std::set<int> numbers;
        for (const auto &group : config["groups"]) {
            if (!group.is_object() || !group.contains("number") || !group["number"].is_number_integer() ||
                !group.contains("name") || !group["name"].is_string() || !group.contains("phase"))
                throw std::runtime_error("Each group needs an integer number, a name and a phase");
            if (group["number"] < 1 || group["number"] > std::numeric_limits<int>::max())
                throw std::runtime_error("Group numbers must be positive integers");
            const int number = group["number"].get<int>();
            const std::string name = group["name"].get<std::string>();
            if (name.empty() || name.find_first_not_of(" \t\r\n") == std::string::npos ||
                name.find('\0') != std::string::npos || !numbers.insert(number).second)
                throw std::runtime_error("Group numbers must be unique and names must not be empty");
            const auto &phase = group["phase"];
            if (!phase.is_null() && (!phase.is_number_integer() || phase < 1 || phase > 3))
                throw std::runtime_error("Group phase must be 1, 2, 3 or null (unassigned)");
            next.push_back({number, name, phase.is_null() ? 0 : phase.get<int>()});
        }
        circuitGroups = std::move(next);
        groupsConfigError.clear();
        return true;
    } catch (const std::exception &e) {
        groupsConfigError = e.what();
        logStderr() << "Groups configuration " << groupsConfigPath << ": " << groupsConfigError << '\n';
        return false; // Preserve the previous configuration on a failed reload.
    }
}

struct Sample {
    double time;
    double active_power_w;
    double active_power_l_w[3];
    bool breakBefore = false;
    double totalGasM3 = NAN;
};

static std::deque<Sample> samples;
static std::mutex samplesMutex;
static std::atomic<bool> running{true};
constexpr double HISTORY_SECONDS = 3600.0;
constexpr double MAIN_FUSE_CURRENT_A = 25.0;
constexpr double NOMINAL_PHASE_VOLTAGE_V = 230.0;
// Per-phase active-power reference at unity power factor, not a fuse trip threshold.
constexpr double PHASE_POWER_REFERENCE_W = MAIN_FUSE_CURRENT_A * NOMINAL_PHASE_VOLTAGE_V;

namespace {
    struct Measurement {
        bool valid = false;
        bool requestFailed = false;
        double active_power_w = 0.0;
        double active_power_l_w[3]{};
        double totalGasM3 = NAN;
        std::string gasTimestamp;
        double voltage[3]{};
        double current[3] = {NAN, NAN, NAN};
        std::string activeTariff = "unavailable";
        std::string wifiSsid = "unavailable";
        int wifiStrength = -1;
        std::chrono::system_clock::time_point updated{};
    };
}

static Measurement measurement;
static double firstSampleTime = 0.0;
static std::chrono::system_clock::time_point programStarted;

struct GasSession {
    double firstTotal = NAN;
    double latestTotal = NAN;
    bool meterDecreased = false;

    void update(double total) {
        if (!std::isfinite(total) || total < 0.0)
            return;
        if (!std::isfinite(firstTotal))
            firstTotal = total;
        if (std::isfinite(latestTotal) && total < latestTotal)
            meterDecreased = true;
        latestTotal = total;
    }
};

// Protected by samplesMutex; independent of the rolling plot history.
static GasSession gasSession;

static std::string formatGasTimestamp(const std::string &timestamp) {
    if (timestamp.size() != 12 ||
        timestamp.find_first_not_of("0123456789") != std::string::npos)
        return "unavailable";
    const int year = 2000 + std::stoi(timestamp.substr(0, 2));
    const unsigned month = std::stoi(timestamp.substr(2, 2));
    const unsigned day = std::stoi(timestamp.substr(4, 2));
    const int hour = std::stoi(timestamp.substr(6, 2));
    const int minute = std::stoi(timestamp.substr(8, 2));
    const int second = std::stoi(timestamp.substr(10, 2));
    const std::chrono::year_month_day date{std::chrono::year{year},
        std::chrono::month{month}, std::chrono::day{day}};
    if (!date.ok() || hour > 23 || minute > 59 || second > 59)
        return "unavailable";
    static constexpr const char *months[] = {"January", "February", "March", "April",
        "May", "June", "July", "August", "September", "October", "November", "December"};
    char formatted[64];
    std::snprintf(formatted, sizeof(formatted), "%s %u, %d %02d:%02d:%02d",
        months[month - 1], day, year, hour, minute, second);
    return formatted;
}

static double plotTime() {
    // Display civil time, including time spent suspended and clock corrections.
    return std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static void publishMeasurement(const Measurement &next) {
    const double timestamp = std::chrono::duration<double>(next.updated.time_since_epoch()).count();
    std::lock_guard<std::mutex> lock(samplesMutex);
    const bool clockMovedBack = measurement.valid && next.updated < measurement.updated;
    if (clockMovedBack)
        samples.clear(); // Keep timestamps sorted for plotting and tooltip searches.
    if (!measurement.valid || clockMovedBack)
        firstSampleTime = timestamp;
    const bool breakBefore = measurement.requestFailed ||
        (measurement.valid && std::chrono::duration<double>(next.updated - measurement.updated).count() > 3.0);
    measurement = next;
    gasSession.update(next.totalGasM3);
    samples.push_back({timestamp, next.active_power_w,
        {next.active_power_l_w[0], next.active_power_l_w[1], next.active_power_l_w[2]},
        breakBefore, next.totalGasM3});
    while (!samples.empty() && samples.front().time < timestamp - HISTORY_SECONDS)
        samples.pop_front();
}

static void waitForNextPoll(int seconds = 1) {
    for (int i = 0; i < seconds * 10 && running.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

static size_t writeCallback(const void *contents, const size_t size, size_t nmemb, void *userdata) {
    auto *s = static_cast<std::string *>(userdata);
    try {
        s->append(static_cast<const char *>(contents), size * nmemb);
    } catch (...) {
        return 0; // Never propagate a C++ exception through libcurl.
    }
    return size * nmemb;
}

static int transferProgress(void *, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return running.load() ? 0 : 1;
}

// cURL read P1 data
// curl -s http://<ip address>/api/v1/data
static json readP1(CURL *curl, std::string &error) {
    error.clear();
    std::string response;
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    char details[CURL_ERROR_SIZE]{};
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, details);
    const CURLcode rc = curl_easy_perform(curl);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, nullptr);
    if (rc != CURLE_OK) {
        error = details[0] ? details : curl_easy_strerror(rc);
        return {};
    }
    long status = 0;
    if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status) != CURLE_OK ||
        status < 200 || status >= 300) {
        error = "P1 HTTP request failed: status " + std::to_string(status);
        return {};
    }

    json j;
    try {
        j = json::parse(response);
    }
    catch (const json::exception& e) {
        error = std::string("Failed to parse JSON: ") + e.what();
        return {};
    }

    if (!j.is_object()) {
        error = "Invalid P1 response: expected a JSON object";
        return {};
    }

    // Validate every reading before the worker publishes any of them.
    for (const char *field : {"active_power_w", "active_power_l1_w",
                             "active_power_l2_w", "active_power_l3_w", "active_voltage_l1_v",
                             "active_voltage_l2_v", "active_voltage_l3_v"}) {
        const auto it = j.find(field);
        if (it == j.end() || !it->is_number()) {
            error = std::string("Invalid P1 response: missing or non-numeric field '") + field + "'";
            return {};
        }
    }
    return j;
}

static CURL *initCurl(const string &ip, std::uint16_t port) {
    CURL *curl = curl_easy_init();
    if (!curl)
        return nullptr;

    const string url = "http://" + ip + ":" + std::to_string(port) + "/api/v1/data";
    const auto setOption = [curl](const CURLoption option, auto value) {
        const CURLcode result = curl_easy_setopt(curl, option, value);
        if (result != CURLE_OK)
            logStderr() << "Failed to configure cURL: " << curl_easy_strerror(result) << '\n';
        return result == CURLE_OK;
    };
    if (!setOption(CURLOPT_URL, url.c_str()) ||
        !setOption(CURLOPT_WRITEFUNCTION, writeCallback) ||
        !setOption(CURLOPT_CONNECTTIMEOUT_MS, 2000L) ||
        !setOption(CURLOPT_TIMEOUT_MS, 5000L) ||
        !setOption(CURLOPT_NOSIGNAL, 1L) ||
        !setOption(CURLOPT_NOPROGRESS, 0L) ||
        !setOption(CURLOPT_XFERINFOFUNCTION, transferProgress)) {
        curl_easy_cleanup(curl);
        return nullptr;
    }
    return curl;
}

// Synthetic three-phase readings at unity power factor; no network access.
static json simulatedP1(std::mt19937 &random, size_t tick) {
    std::uniform_real_distribution<double> noise(-150.0, 150.0);
    std::uniform_real_distribution<double> voltage(227.0, 233.0);
    std::uniform_int_distribution<int> signal(75, 100);
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    const int tariff = local.tm_hour >= 6 && local.tm_hour < 23 ? 2 : 1;
    json report = {
        {"meter_model", "Simulated P1"},
        {"wifi_ssid", "Simulation"},
        {"wifi_strength", signal(random)},
        {"active_tariff", tariff}
    };
    double total = 0.0;
    for (size_t phase = 0; phase < 3; ++phase) {
        const std::string suffix = "l" + std::to_string(phase + 1);
        const double volts = std::round(voltage(random) * 10.0) / 10.0;
        const double watts = std::round(600.0 + 3200.0 * std::sin(tick * 0.05 + phase * 1.8) + noise(random));
        report["active_power_" + suffix + "_w"] = watts;
        report["active_voltage_" + suffix + "_v"] = volts;
        report["active_current_" + suffix + "_a"] = watts / volts;
        total += watts;
    }
    // Publish heating-only consumption every five minutes, like a gas meter.
    const double elapsed = HeatingSimulation::elapsed();
    const double reportedSeconds = std::floor(elapsed / 300.0) * 300.0;
    report["total_gas_m3"] = HeatingSimulation::gasTotal(reportedSeconds);
    const std::time_t gasTime = std::chrono::system_clock::to_time_t(
        HeatingSimulation::started + std::chrono::seconds(static_cast<long long>(reportedSeconds)));
    std::tm gasLocal{};
    localtime_r(&gasTime, &gasLocal);
    char gasTimestamp[16];
    std::strftime(gasTimestamp, sizeof(gasTimestamp), "%y%m%d%H%M%S", &gasLocal);
    report["gas_timestamp"] = std::stoll(gasTimestamp);
    report["active_power_w"] = total;
    return report;
}

static void p1Thread() {
    HomeWizardDiscovery discovery;
    CURL *curl = nullptr;
    std::string currentIp = configuredMeterAddress;
    std::uint16_t currentPort = configuredMeterPort;
    if (simulation) {
        logStderr() << "Simulation enabled: generating P1 readings locally\n";
    } else {
        if (currentIp.empty()) {
            if (!discovery.discover(running)) {
                if (running.load()) {
                    logStderr() << "No HomeWizard P1 meter found; set --meter-address or P1SCOPE_METER_ADDRESS to connect directly\n";
                }
                running = false;
                return;
            }
            currentIp = discovery.ip();
            currentPort = discovery.port();
            logStderr() << "HomeWizard P1 found: hostname=" << discovery.hostname()
                        << " address=" << currentIp << " port=" << currentPort << '\n';
        } else {
            logStderr() << "HomeWizard P1: using configured address " << currentIp << ':' << currentPort
                        << " (Avahi discovery skipped)\n";
        }
        curl = initCurl(currentIp, currentPort);
        if (!curl) {
            logStderr() << "Failed to initialize cURL for the P1 meter\n";
            running = false;
            return;
        }

    }
    std::mt19937 random;
    if (simulation)
        random.seed(std::random_device{}());
    size_t simulatedTick = 0;

    int retrySeconds = 1;
    bool recovering = false;
    auto lastErrorLog = std::chrono::steady_clock::time_point{};

    while (running) {
        std::string error;
        json j = simulation ? simulatedP1(random, simulatedTick++) : readP1(curl, error);
        if (!running.load())
            break;
        if (j.empty()) {
            {
                std::lock_guard<std::mutex> lock(samplesMutex);
                measurement.requestFailed = true;
            }
            const auto now = std::chrono::steady_clock::now();
            retrySeconds = std::min(retrySeconds * 2, 30);
            if (!recovering || now - lastErrorLog >= std::chrono::seconds(30)) {
                logStderr() << "P1 " << currentIp << ':' << currentPort << ": " << error
                          << "; retrying in " << retrySeconds << " s\n";
                lastErrorLog = now;
            }
            recovering = true;
            waitForNextPoll(retrySeconds);
            continue;
        }
        if (recovering)
            logStderr() << "P1 connection recovered at " << currentIp << ':' << currentPort << '\n';
        recovering = false;
        retrySeconds = 1;
        Measurement next;
        next.valid = true;
        next.active_power_w = j["active_power_w"].get<double>();
        next.active_power_l_w[0] = j["active_power_l1_w"].get<double>();
        next.active_power_l_w[1] = j["active_power_l2_w"].get<double>();
        next.active_power_l_w[2] = j["active_power_l3_w"].get<double>();
        if (const auto tariff = j.find("active_tariff"); tariff != j.end() && tariff->is_number_integer()) {
            next.activeTariff = tariff->dump();
            if (*tariff == 1)
                next.activeTariff += " (Low/off-peak)";
            else if (*tariff == 2)
                next.activeTariff += " (Normal/peak)";
        }
        next.voltage[0] = j["active_voltage_l1_v"].get<double>();
        next.voltage[1] = j["active_voltage_l2_v"].get<double>();
        next.voltage[2] = j["active_voltage_l3_v"].get<double>();
        if (const auto ssid = j.find("wifi_ssid");
            ssid != j.end() && ssid->is_string() && !ssid->get_ref<const json::string_t&>().empty())
            next.wifiSsid = ssid->get<std::string>();
        if (const auto strength = j.find("wifi_strength");
            strength != j.end() && strength->is_number_integer() && *strength >= 0 && *strength <= 100)
            next.wifiStrength = strength->get<int>();
        const char *currentFields[] = {"active_current_l1_a", "active_current_l2_a", "active_current_l3_a"};
        for (size_t phase = 0; phase < 3; ++phase) {
            const auto value = j.find(currentFields[phase]);
            if (value != j.end() && value->is_number())
                next.current[phase] = value->get<double>();
        }
        if (const auto gas = j.find("total_gas_m3"); gas != j.end() && gas->is_number()) {
            const double value = gas->get<double>();
            if (std::isfinite(value) && value >= 0.0)
                next.totalGasM3 = value;
        }
        if (const auto timestamp = j.find("gas_timestamp");
            timestamp != j.end() && timestamp->is_number_integer())
            next.gasTimestamp = formatGasTimestamp(timestamp->dump());
        next.updated = std::chrono::system_clock::now();
        publishMeasurement(next);

        if (printCompleteJson)
            cout << "\033[2J\033[H" << j.dump(4) << '\n';

        waitForNextPoll();
    }

    if (curl)
        curl_easy_cleanup(curl);
}

#ifndef P1SCOPE_HEADLESS
static double powerScale(const double power) {
    double p = std::abs(power);
    p *= 1.25; // Add some headroom
    return std::max(500.0,std::ceil(p / 500.0) * 500.0); // Round upwards to the next 500 W
}

static int timeFormatter(const double value, char *buff, const int size, void *) {
    auto t = static_cast<std::time_t>(value);
    std::tm tm{};
    localtime_r(&t, &tm);
    return std::snprintf(
        buff,
        size,
        "%02d:%02d:%02d",
        tm.tm_hour,
        tm.tm_min,
        tm.tm_sec);
}

#endif

static void printHelp(const string& programName) {
    string help = R"(
P1Scope - Live P1 Energy Monitor

USAGE: _progname [OPTIONS]

OPTIONS:
    --report, -v   : Print each live or simulated P1 JSON report
    --simulation, --simulated, -s : Generate random P1 reports once per second;
                    no meter, Avahi discovery or HTTP connection required
    --meter-address IPv4[:PORT] : Connect directly to the meter, skipping Avahi;
                    also settable with P1SCOPE_METER_ADDRESS (default port: 80)
    --server      : Serve the web dashboard without a graphical window
    --bind ADDRESS : HTTP IPv4 bind address (default: 127.0.0.1)
    --port PORT   : HTTP port (default: 8080)
    --web-root PATH : Directory containing the web dashboard assets
    --config PATH : Load circuit groups from a JSON file
    --help, -h    : Show this help

Options can be combined in any order.

)";
    help.replace(help.find("_progname"), 9, programName);
    cout << help
         << "EXAMPLES:\n"
         << "    " << programName << " --simulation\n"
         << "    " << programName << " --simulation --report\n\n";
}

#ifndef P1SCOPE_HEADLESS
static void drawDashboard(const HueSnapshot &hue, HueTemperatures *controller = nullptr) {
    const char *gasSuffix = simulation ? " (simulated)" : "";
    static bool showReference = false;
    const char *seriesNames[] = {"Grid", "L1", "L2", "L3"};
    const ImVec4 seriesColors[] = {
        ImVec4(0.95f, 0.80f, 0.20f, 1.0f), // Grid: yellow
        ImVec4(0.20f, 0.65f, 1.00f, 1.0f), // L1: blue
        ImVec4(1.00f, 0.40f, 0.25f, 1.0f), // L2: orange
        ImVec4(0.25f, 0.85f, 0.50f, 1.0f), // L3: green
    };

    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    // Window
    ImGui::Begin("P1Scope", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);


    // Copy readings and history together to render one consistent snapshot.
    Measurement displayed;
    GasSession displayedGasSession;
    const double now = plotTime();
    double first;
    std::vector<double> x;
    std::vector<double> gasTotals;
    std::vector<double> y[4]; // Grid total, L1, L2, L3
    {
        std::lock_guard<std::mutex> lock(samplesMutex);
        displayed = measurement;
        displayedGasSession = gasSession;
        first = firstSampleTime;
        while (!samples.empty() && samples.front().time < now - HISTORY_SECONDS)
            samples.pop_front();
        x.reserve(samples.size());
        for (auto &series : y)
            series.reserve(samples.size());
        for (const auto &s: samples) {
            if (s.breakBefore && !x.empty()) {
                x.push_back((x.back() + s.time) * 0.5);
                gasTotals.push_back(NAN);
                for (auto &series : y)
                    series.push_back(std::numeric_limits<double>::quiet_NaN());
            }
            x.push_back(s.time);
            gasTotals.push_back(s.totalGasM3);
            y[0].push_back(s.active_power_w);
            for (size_t phase = 0; phase < 3; ++phase)
                y[phase + 1].push_back(s.active_power_l_w[phase]);
        }
    }

    const double age = displayed.valid ? std::max(0.0, std::chrono::duration<double>(
        std::chrono::system_clock::now() - displayed.updated).count()) : 0.0;
    const bool stale = displayed.valid && (displayed.requestFailed || age > 3.0);
    const std::string wifi = "Wi-Fi: " + displayed.wifiSsid + " | " +
        (displayed.wifiStrength >= 0 ? std::to_string(displayed.wifiStrength) + "%" : "unavailable");
    std::string connection;
    if (stale) connection = "Stale data - " + std::to_string(static_cast<int>(age)) + " s ago";
    else if (displayed.valid) connection = simulation ? "SIMULATION - generated data" : "Connected - live data";
    else connection = displayed.requestFailed ? "Unavailable - retrying" : "Connecting...";
    const std::string connectionAndWifi = connection + " | " + wifi;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f,
        ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(connectionAndWifi.c_str()).x));
    const ImVec4 connectionColor = stale ? ImVec4(1, 0.65f, 0.25f, 1) : displayed.valid ?
        ImVec4(0.3f, 0.85f, 0.55f, 1) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    ImGui::PushStyleColor(ImGuiCol_Text, connectionColor);
    ImGui::TextWrapped("%s", connectionAndWifi.c_str());
    ImGui::PopStyleColor();
    ImGui::Separator();
    if (ImGui::Button("Reload groups"))
        loadGroupsConfig();
    ImGui::SameLine();
    ImGui::TextDisabled("Group configuration");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", groupsConfigPath.c_str());
    if (!groupsConfigError.empty()) {
        ImGui::TextWrapped("Groups: %s. Previous configuration retained, if available.", groupsConfigError.c_str());
    }
    for (const auto &group : circuitGroups) {
        if (group.phase == 0)
            ImGui::TextWrapped("Unassigned: %d - %s", group.number, group.name.c_str());
    }
    static int timeRange = 2;
    const double durations[] = {60.0, 300.0, 600.0, 1800.0, 3600.0};
    ImGui::TextUnformatted("History");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    ImGui::Combo("##History", &timeRange, "1 minute\0" "5 minutes\0" "10 minutes\0" "30 minutes\0" "1 hour\0");
    ImGui::SameLine();
    ImGui::Checkbox("25 A reference", &showReference);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("+/-5750 W per phase at 230 V and power factor 1.\nReference only; not an exact fuse trip threshold.");
    const double duration = durations[timeRange];

    // The startup anchor survives history eviction; time advances during outages.
    const double xmax = displayed.valid
        ? std::max(first + duration, now) : now + duration;
    const double xmin = xmax - duration;

    // Stack phase cards on narrow windows; preserve scrolling at small heights.
    const bool stackedCards = ImGui::GetContentRegionAvail().x < 660.0f;
    const float cardWidth = stackedCards ? ImGui::GetContentRegionAvail().x :
        (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 3.0f;
    const auto &style = ImGui::GetStyle();
    const float textWidth = std::max(1.0f, cardWidth -
        2.0f * (style.WindowPadding.x + style.ChildBorderSize));
    float cardHeights[3]{};
    float cardsHeight = 0.0f;
    for (int phase = 0; phase < 3; ++phase) {
        float groupsHeight = 0.0f;
        for (const auto &group : circuitGroups) {
            if (group.phase == phase + 1) {
                const std::string label = std::to_string(group.number) + " - " + group.name;
                groupsHeight += ImGui::CalcTextSize(label.c_str(), nullptr, false, textWidth).y +
                    style.ItemSpacing.y;
            }
        }
        if (groupsHeight == 0.0f)
            groupsHeight = ImGui::GetTextLineHeightWithSpacing();
        cardHeights[phase] = ImGui::GetTextLineHeightWithSpacing() * 5.0f +
            ImGui::GetFrameHeight() + 2.0f * style.WindowPadding.y +
            2.0f * style.ChildBorderSize + groupsHeight;
    }
    const float cardHeight = *std::max_element(std::begin(cardHeights), std::end(cardHeights));
    static bool showPhaseCards = true;
    cardsHeight = showPhaseCards ? (stackedCards ? 3.0f : 1.0f) * (cardHeight + style.ItemSpacing.y) : 0.0f;
    cardsHeight += ImGui::GetFrameHeightWithSpacing(); // Keep the toggle visible when cards are hidden.
    static bool showGas = true;
    const float availablePlotHeight = ImGui::GetContentRegionAvail().y - cardsHeight -
        ImGui::GetTextLineHeightWithSpacing() - ImGui::GetFrameHeightWithSpacing();
    const float gasPlotHeight = showGas ? std::max(200.0f, availablePlotHeight * 0.4f) : 0.0f;
    const ImVec2 plotSize(-1, std::max(160.0f, availablePlotHeight -
        (showGas ? gasPlotHeight + style.ItemSpacing.y + 4.0f * ImGui::GetTextLineHeightWithSpacing() : 0.0f)));

    double peakPower = showReference ? PHASE_POWER_REFERENCE_W : 0.0;
    for (const auto &series : y)
        for (size_t i = 0; i < x.size(); ++i)
            if (x[i] >= xmin && x[i] <= xmax && std::isfinite(series[i]))
                peakPower = std::max(peakPower, std::abs(series[i]));

    const double scale = powerScale(peakPower);
    ImPlot::SetNextAxisLimits(ImAxis_Y1, -scale, scale, ImGuiCond_Always);

    if (ImPlot::BeginPlot("Grid Power", plotSize, ImPlotFlags_Crosshairs|ImPlotFlags_NoMouseText)) {
        ImPlot::SetupLegend(ImPlotLocation_NorthEast);
        ImPlot::SetupAxis(ImAxis_X1, "Time");
        ImPlot::SetupAxis(ImAxis_Y1, "Power [W]");
        ImPlot::SetupAxisFormat(ImAxis_X1, timeFormatter);

        ImPlot::SetupAxisLimits(ImAxis_X1, xmin, xmax, ImGuiCond_Always);

        if (!x.empty()) {
            for (size_t series = 0; series < 4; ++series) {
                ImPlotSpec style;
                style.LineColor = seriesColors[series];
                ImPlot::PlotLine(seriesNames[series], x.data(), y[series].data(),
                                 static_cast<int>(x.size()), style);
            }
        }

        // Zero line
        double zeroX[2] = {
            xmin,
            xmax
        };
        double zeroY[2] = {0, 0};

        ImPlot::PlotLine("Zero", zeroX, zeroY, 2);

        const double phaseLimits[] = {PHASE_POWER_REFERENCE_W, -PHASE_POWER_REFERENCE_W};
        ImPlotSpec limitStyle;
        limitStyle.LineColor = ImVec4(0.85f, 0.45f, 0.5f, 0.65f);
        limitStyle.LineWeight = 1.0f;
        limitStyle.Flags = ImPlotInfLinesFlags_Horizontal;
        if (showReference) {
            // Render a dashed reference using independent line segments.
            std::vector<double> dashX, dashY;
            const float pixelWidth = ImPlot::GetPlotSize().x;
            for (double limit : phaseLimits) {
                for (float px = 0; px < pixelWidth; px += 12.0f) {
                    dashX.push_back(xmin + (xmax - xmin) * px / pixelWidth);
                    dashX.push_back(xmin + (xmax - xmin) * std::min(px + 6.0f, pixelWidth) / pixelWidth);
                    dashY.push_back(limit);
                    dashY.push_back(limit);
                }
            }
            limitStyle.Flags = ImPlotLineFlags_Segments;
            ImPlot::PlotLine("25 A / phase*", dashX.data(), dashY.data(), static_cast<int>(dashX.size()), limitStyle);
        }

        if (ImPlot::IsPlotHovered() && !x.empty()) {
            const double mouseTime = ImPlot::GetPlotMousePos().x;
            const auto after = std::lower_bound(x.begin(), x.end(), mouseTime);
            size_t selected = after == x.end() ? x.size() - 1 : static_cast<size_t>(after - x.begin());
            if (selected > 0 && mouseTime - x[selected - 1] < x[selected] - mouseTime)
                --selected;
            // Do not invent values inside missing-data intervals or after the last reading.
            bool missing = !std::isfinite(y[0][selected]) || mouseTime < x.front() || mouseTime > x.back();
            if (after != x.begin() && after != x.end()) {
                const size_t right = static_cast<size_t>(after - x.begin());
                missing |= !std::isfinite(y[0][right - 1]) || !std::isfinite(y[0][right]);
            }
            ImGui::BeginTooltip();
            if (missing) {
                ImGui::TextUnformatted("No measurement at this time");
            } else {
                char time[16];
                timeFormatter(x[selected], time, sizeof(time), nullptr);
                ImGui::TextUnformatted(time);
                ImGui::Separator();
                for (size_t series = 0; series < 4; ++series)
                    ImGui::TextColored(seriesColors[series], "%s: %.1f W", seriesNames[series], y[series][selected]);
            }
            ImGui::EndTooltip();
        }

        ImPlot::EndPlot();
    }
    if (displayed.valid) {
        ImGui::Text("Tariff: %s", displayed.activeTariff.c_str());
        ImGui::SameLine();
        ImGui::TextColored(seriesColors[0], "| Grid %s: %.0f W", displayed.active_power_w >= 0 ? "import" : "export",
                    std::abs(displayed.active_power_w));
    } else {
        ImGui::TextWrapped("Waiting for meter data...");
    }
    if (showGas) {
        if (std::isfinite(displayed.totalGasM3))
            ImGui::TextWrapped("Gas meter%s: %.3f m3 | Updated: %s%s",
                gasSuffix, displayed.totalGasM3, displayed.gasTimestamp.empty() ? "unavailable" : displayed.gasTimestamp.c_str(),
                stale ? " | stale connection" : "");
        else
            ImGui::TextDisabled("Gas%s: reading unavailable", gasSuffix);
        const auto elapsedSeconds = std::max(0LL, static_cast<long long>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now() - programStarted).count()));
        char sessionDuration[64];
        std::snprintf(sessionDuration, sizeof(sessionDuration), "%lldh %02lldm %02llds",
            elapsedSeconds / 3600, (elapsedSeconds / 60) % 60, elapsedSeconds % 60);
        if (displayedGasSession.meterDecreased)
            ImGui::TextWrapped("Gas used since startup%s (%s): unavailable (meter reading decreased)", gasSuffix, sessionDuration);
        else if (std::isfinite(displayedGasSession.firstTotal))
            ImGui::TextWrapped("Gas used since startup%s (%s): %.3f m3 (first to latest received reading)",
                gasSuffix, sessionDuration, displayedGasSession.latestTotal - displayedGasSession.firstTotal);
        else
            ImGui::TextDisabled("Gas used since startup%s (%s): waiting for first gas reading", gasSuffix, sessionDuration);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Gas readings arrive about every 5 minutes.\nUsage is relative to the first gas reading received this session;\nconsumption after the latest meter update is not included yet.");
    }
    ImGui::Checkbox("Gas & temperatures", &showGas);
    ImGui::SameLine();
    if (ImGui::Button("Hue lights")) ImGui::OpenPopup("Hue lights");
    ImGui::SetNextWindowSize(ImVec2(560, 400), ImGuiCond_FirstUseEver);
    bool lightsOpen = true;
    if (ImGui::BeginPopupModal("Hue lights", &lightsOpen)) {
        ImGui::TextWrapped("%s", hue.lightsStatus.c_str());
        if (!hue.lightActionStatus.empty()) ImGui::TextWrapped("%s", hue.lightActionStatus.c_str());
        if (!hue.lightsAvailable && !hue.lights.empty())
            ImGui::TextWrapped("Showing last known lights; states may be out of date.");
        if (hue.lightsAvailable && hue.lights.empty()) ImGui::TextUnformatted("No lights found on this bridge.");
        static bool showUnavailableLights = false;
        ImGui::Checkbox("Show unavailable lights", &showUnavailableLights);
        const auto hiddenLights = std::count_if(hue.lights.begin(), hue.lights.end(),
            [](const auto &light) { return !light.reachable; });
        ImGui::Text("%zu light(s), %zu unavailable", hue.lights.size(), static_cast<size_t>(hiddenLights));
        if (ImGui::BeginTable("Lights", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_ScrollY, ImVec2(0, -45))) {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Control");
            ImGui::TableSetupColumn("Brightness");
            ImGui::TableHeadersRow();
            std::string previousRoom;
            bool firstRoom = true;
            for (const auto &light : hue.lights) {
                if (!showUnavailableLights && !light.reachable) continue;
                if (firstRoom || light.roomId != previousRoom) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
                    ImGui::TextWrapped("%s", light.room.c_str());
                    ImGui::PopStyleColor();
                    previousRoom = light.roomId;
                    firstRoom = false;
                }
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextWrapped("%s%s", light.name.c_str(), light.reachable ? "" : " (unavailable)");
                ImGui::TableNextColumn();
                ImGui::PushID(light.id.c_str());
                ImGui::BeginDisabled(!controller || !hue.lightsAvailable || !light.reachable || !hue.pendingLight.empty());
                const ImVec4 toggleColor = light.on ? ImVec4(0.12f, 0.43f, 0.23f, 1) : ImVec4(0.65f, 0.16f, 0.18f, 1);
                ImGui::PushStyleColor(ImGuiCol_Button, toggleColor);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, light.on ? ImVec4(0.16f, 0.53f, 0.29f, 1) : ImVec4(0.76f, 0.21f, 0.23f, 1));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, light.on ? ImVec4(0.09f, 0.35f, 0.18f, 1) : ImVec4(0.54f, 0.12f, 0.14f, 1));
                if (ImGui::Button(light.on ? "Turn off" : "Turn on")) controller->setLight(light.id, !light.on);
                ImGui::PopStyleColor(3);
                ImGui::EndDisabled();
                ImGui::TableNextColumn();
                if (light.brightness >= 0) {
                    static std::string editingLight;
                    static float editingBrightness = 0;
                    float value = editingLight == light.id ? editingBrightness : static_cast<float>(light.brightness);
                    ImGui::BeginDisabled(!controller || !hue.lightsAvailable || !light.reachable || !hue.pendingLight.empty());
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::SliderFloat("##brightness", &value, 0, 100, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
                        editingLight = light.id;
                        editingBrightness = value;
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        controller->setBrightness(light.id, value);
                        editingLight.clear();
                    }
                    ImGui::EndDisabled();
                } else ImGui::TextUnformatted("Unavailable");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (showGas) {
        const ImVec4 temperatureColors[] = {
            ImVec4(1.0f, 0.55f, 0.15f, 1.0f), ImVec4(0.2f, 0.8f, 0.9f, 1.0f),
            ImVec4(0.4f, 0.9f, 0.4f, 1.0f), ImVec4(1.0f, 0.4f, 0.6f, 1.0f)};
        std::vector<double> gasUsage(x.size(), NAN);
        double baseline = NAN;
        double lastTotal = NAN;
        double peakGas = 0.001;
        bool reset = false;
        for (size_t i = 0; i < x.size(); ++i) {
            if (x[i] < xmin || x[i] > xmax || !std::isfinite(gasTotals[i]))
                continue;
            if (!std::isfinite(baseline))
                baseline = gasTotals[i];
            if (std::isfinite(lastTotal) && gasTotals[i] < lastTotal)
                reset = true; // Do not report negative usage after a meter reset/replacement.
            lastTotal = gasTotals[i];
            if (!reset) {
                gasUsage[i] = gasTotals[i] - baseline;
                peakGas = std::max(peakGas, gasUsage[i]);
            }
        }
        // Center the regression times to avoid loss of precision with epoch timestamps.
        std::vector<double> gasTrend(x.size(), NAN);
        double trendRate = NAN;
        double sumTime = 0.0, sumUsage = 0.0;
        size_t trendCount = 0;
        for (size_t i = 0; i < x.size(); ++i) {
            if (!std::isfinite(gasUsage[i])) continue;
            sumTime += x[i] - xmin;
            sumUsage += gasUsage[i];
            ++trendCount;
        }
        if (trendCount >= 2) {
            const double meanTime = sumTime / trendCount;
            const double meanUsage = sumUsage / trendCount;
            double covariance = 0.0, variance = 0.0;
            for (size_t i = 0; i < x.size(); ++i) {
                if (!std::isfinite(gasUsage[i])) continue;
                const double delta = (x[i] - xmin) - meanTime;
                covariance += delta * (gasUsage[i] - meanUsage);
                variance += delta * delta;
            }
            if (variance > 0.0) {
                const double slope = covariance / variance;
                trendRate = slope * 3600.0;
                for (size_t i = 0; i < x.size(); ++i) {
                    if (!std::isfinite(gasUsage[i])) continue;
                    gasTrend[i] = meanUsage + slope * ((x[i] - xmin) - meanTime);
                    peakGas = std::max(peakGas, gasTrend[i]);
                }
            }
        }

        if (ImPlot::BeginPlot("Gas usage & room temperatures", ImVec2(-1, gasPlotHeight), ImPlotFlags_Crosshairs | ImPlotFlags_NoMouseText)) {
            ImPlot::SetupLegend(ImPlotLocation_NorthEast);
            ImPlot::SetupAxis(ImAxis_X1, "Time (received)");
            ImPlot::SetupAxis(ImAxis_Y1, "Gas [m3]");
            ImPlot::SetupAxis(ImAxis_Y2, "Temperature [C]", ImPlotAxisFlags_Opposite);
            ImPlot::SetupAxisFormat(ImAxis_Y2, "%.1f");
            double minTemp = 15.0, maxTemp = 25.0;
            for (const auto &sensor : hue.sensors)
                for (const auto &point : sensor.history)
                    if (point.time >= xmin && point.time <= xmax && std::isfinite(point.celsius)) {
                        minTemp = std::min(minTemp, point.celsius - 1.0);
                        maxTemp = std::max(maxTemp, point.celsius + 1.0);
                    }
            ImPlot::SetupAxisLimits(ImAxis_Y2, minTemp, maxTemp, ImGuiCond_Always);
            ImPlot::SetupAxisFormat(ImAxis_X1, timeFormatter);
            // Keep sub-litre tick intervals distinct when the visible gas range is small.
            ImPlot::SetupAxisFormat(ImAxis_Y1, peakGas < 0.01 ? "%.4f" : "%.3f");
            ImPlot::SetupAxisLimits(ImAxis_X1, xmin, xmax, ImGuiCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, peakGas * 1.15, ImGuiCond_Always);
            if (!x.empty()) {
                ImPlotSpec gasStyle;
                gasStyle.LineColor = ImVec4(0.75f, 0.5f, 1.0f, 1.0f);
                ImPlot::PlotStairs(simulation ? "Gas (simulated)" : "Gas", x.data(), gasUsage.data(), static_cast<int>(x.size()), gasStyle);
                if (std::isfinite(trendRate)) {
                    ImPlotSpec trendStyle;
                    trendStyle.LineColor = ImVec4(1.0f, 0.85f, 0.3f, 1.0f);
                    trendStyle.Marker = ImPlotMarker_Circle;
                    trendStyle.MarkerSize = 1.5f;
                    trendStyle.MarkerFillColor = trendStyle.LineColor;
                    trendStyle.MarkerLineColor = trendStyle.LineColor;
                    std::vector<double> dotTimes, dotValues;
                    const float width = ImPlot::GetPlotSize().x;
                    // Space dots in pixels so the pattern stays consistent at every history scale.
                    for (float pixel = 0.0f; pixel < width; pixel += 6.0f) {
                        const double time = xmin + (xmax - xmin) * pixel / width;
                        const auto after = std::lower_bound(x.begin(), x.end(), time);
                        if (after == x.begin() || after == x.end()) continue;
                        const size_t right = static_cast<size_t>(after - x.begin());
                        if (!std::isfinite(gasTrend[right - 1]) || !std::isfinite(gasTrend[right])) continue;
                        dotTimes.push_back(time);
                        dotValues.push_back(gasTrend[right - 1] +
                            (time - x[right - 1]) * trendRate / 3600.0);
                    }
                    ImPlot::PlotScatter(simulation ? "Gas trend (simulated)" : "Gas trend", dotTimes.data(), dotValues.data(),
                        static_cast<int>(dotTimes.size()), trendStyle);
                }
            }
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
            for (size_t i = 0; i < hue.sensors.size(); ++i) {
                const auto &sensor = hue.sensors[i];
                std::vector<double> times, values;
                for (const auto &point : sensor.history) {
                    times.push_back(point.time);
                    values.push_back(point.celsius);
                }
                // End a disconnected series instead of extending the last reading to now.
                if (!times.empty()) {
                    ImPlotSpec temperatureStyle;
                    temperatureStyle.LineColor = temperatureColors[i % 4];
                    temperatureStyle.Marker = ImPlotMarker_Circle;
                    temperatureStyle.MarkerSize = 2.0f;
                    const std::string label = sensor.name + "###" + sensor.id;
                    ImPlot::PlotLine(label.c_str(), times.data(), values.data(),
                        static_cast<int>(times.size()), temperatureStyle);
                }
            }
            if (ImPlot::IsPlotHovered()) {
                const double mouseTime = ImPlot::GetPlotMousePos(ImAxis_X1, ImAxis_Y1).x;
                char hoveredTime[16];
                timeFormatter(mouseTime, hoveredTime, sizeof(hoveredTime), nullptr);
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(hoveredTime);
                ImGui::Separator();
                // Gas is a staircase: use the reading at or immediately before the cursor.
                const auto gasAfter = std::upper_bound(x.begin(), x.end(), mouseTime);
                bool gasAvailable = !x.empty() && mouseTime >= x.front() && mouseTime <= x.back();
                size_t gasIndex = 0;
                if (gasAvailable) {
                    gasIndex = static_cast<size_t>(gasAfter - x.begin() - 1);
                    gasAvailable = std::isfinite(gasUsage[gasIndex]);
                    // Missing-data intervals must not imply a known consumption value.
                    if (gasAfter != x.end() && mouseTime > x[gasIndex])
                        gasAvailable &= std::isfinite(gasUsage[gasIndex + 1]);
                }
                const ImVec4 gasColor(0.75f, 0.5f, 1.0f, 1.0f);
                if (gasAvailable) {
                    ImGui::TextColored(gasColor, "Gas usage%s: %.3f m3", gasSuffix, gasUsage[gasIndex]);
                    ImGui::TextColored(gasColor, "Gas meter%s: %.3f m3", gasSuffix, gasTotals[gasIndex]);
                    ImGui::TextDisabled("Usage since first visible reading");
                    if (std::isfinite(trendRate))
                        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f),
                            "Gas trend%s: %.3f m3/h (visible-window estimate)", gasSuffix, trendRate);
                } else {
                    ImGui::TextColored(gasColor, "Gas%s: no measurement at this time", gasSuffix);
                }
                for (size_t i = 0; i < hue.sensors.size(); ++i) {
                    const auto &sensor = hue.sensors[i];
                    const auto &history = sensor.history;
                    const auto after = std::lower_bound(history.begin(), history.end(), mouseTime,
                        [](const TemperaturePoint &point, double time) { return point.time < time; });
                    bool available = !history.empty() && mouseTime >= history.front().time &&
                        mouseTime <= history.back().time;
                    size_t selected = 0;
                    if (available) {
                        selected = after == history.end() ? history.size() - 1 :
                            static_cast<size_t>(after - history.begin());
                        if (selected > 0 && mouseTime - history[selected - 1].time < history[selected].time - mouseTime)
                            --selected;
                        available = std::isfinite(history[selected].celsius);
                        if (after != history.begin() && after != history.end() && after->time != mouseTime)
                            available &= std::isfinite((after - 1)->celsius) && std::isfinite(after->celsius);
                    }
                    if (available) {
                        char sampledTime[16];
                        timeFormatter(history[selected].time, sampledTime, sizeof(sampledTime), nullptr);
                        ImGui::TextColored(temperatureColors[i % 4], "%s: %.2f C (%s)",
                            sensor.name.c_str(), history[selected].celsius, sampledTime);
                    } else {
                        ImGui::TextColored(temperatureColors[i % 4], "%s: no measurement at this time", sensor.name.c_str());
                    }
                }
                ImGui::EndTooltip();
            }
            ImPlot::EndPlot();
        }
        if (reset)
            ImGui::TextWrapped("Gas meter reading decreased; usage after the decrease is unavailable in this window.");
        const float readoutRight = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        for (size_t i = 0; i < hue.sensors.size(); ++i) {
            const auto &sensor = hue.sensors[i];
            std::string readout = sensor.name + ": ";
            if (!sensor.history.empty() && std::isfinite(sensor.history.back().celsius)) {
                char reading[32];
                std::snprintf(reading, sizeof(reading), "%.2f C", sensor.history.back().celsius);
                readout += reading;
            } else {
                readout += "unavailable";
            }
            if (i > 0 && ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x +
                ImGui::CalcTextSize(readout.c_str()).x <= readoutRight)
                ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, temperatureColors[i % 4]);
            ImGui::TextWrapped("%s", readout.c_str());
            ImGui::PopStyleColor();
        }
    }
    ImGui::Checkbox("Phase cards", &showPhaseCards);
    if (showPhaseCards) for (size_t phase = 0; phase < 3; ++phase) {
        if (phase > 0 && !stackedCards)
            ImGui::SameLine();
        ImGui::PushID(static_cast<int>(phase));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.09f, 0.11f, 0.15f, 1));
        if (ImGui::BeginChild("Phase", ImVec2(cardWidth, cardHeight), ImGuiChildFlags_Borders)) {
            ImGui::TextColored(seriesColors[phase + 1], "%s%s", seriesNames[phase + 1], stale ? " - stale" : "");
            if (displayed.valid) {
                ImGui::Text("%.1f W (%.1f V)", displayed.active_power_l_w[phase], displayed.voltage[phase]);
                ImGui::TextDisabled(displayed.active_power_l_w[phase] >= 0 ? "Import" : "Export");
                if (std::isfinite(displayed.current[phase])) {
                    const double amps = std::abs(displayed.current[phase]);
                    char load[64];
                    std::snprintf(load, sizeof(load), "%.1f / 25 A (%.0f%%)", amps, amps / MAIN_FUSE_CURRENT_A * 100);
                    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, seriesColors[phase + 1]);
                    ImGui::ProgressBar(static_cast<float>(std::clamp(amps / MAIN_FUSE_CURRENT_A, 0.0, 1.0)), ImVec2(-1, 0), load);
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Measured current magnitude; reference: 25 A per phase.");
                } else {
                    ImGui::TextDisabled("Current unavailable");
                }
            } else {
                ImGui::TextDisabled("Waiting for data...");
            }
            ImGui::Separator();
            ImGui::TextDisabled("Groups");
            bool hasGroups = false;
            for (const auto &group : circuitGroups) {
                if (group.phase == static_cast<int>(phase + 1)) {
                    ImGui::TextWrapped("%d - %s", group.number, group.name.c_str());
                    hasGroups = true;
                }
            }
            if (!hasGroups)
                ImGui::TextDisabled("No groups assigned");
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopID();
    }

    ImGui::End();
}

#endif

static volatile std::sig_atomic_t serverStopRequested = 0;
static void stopServer(int) { serverStopRequested = 1; }

static json serverSnapshot(HueTemperatures &hue) {
    const auto temperatures = hue.snapshot();
    json result = {{"simulation", simulation}, {"now", plotTime()},
        {"started", std::chrono::duration<double>(programStarted.time_since_epoch()).count()},
        {"historySeconds", HISTORY_SECONDS}, {"groups", json::array()},
        {"groupsError", groupsConfigError}, {"samples", json::array()},
        {"hue", {{"status", temperatures.status}, {"sensors", json::array()},
            {"pendingLight", temperatures.pendingLight}, {"lightActionStatus", temperatures.lightActionStatus},
            {"lightsStatus", temperatures.lightsStatus}, {"lightsAvailable", temperatures.lightsAvailable},
            {"lights", json::array()}}}};
    for (const auto &group : circuitGroups)
        result["groups"].push_back({{"number", group.number}, {"name", group.name},
            {"phase", group.phase == 0 ? json(nullptr) : json(group.phase)}});
    {
        std::lock_guard lock(samplesMutex);
        const auto finite = [](double value) -> json { return std::isfinite(value) ? json(value) : json(nullptr); };
        result["measurement"] = {{"valid", measurement.valid}, {"requestFailed", measurement.requestFailed},
            {"updated", std::chrono::duration<double>(measurement.updated.time_since_epoch()).count()},
            {"grid", measurement.active_power_w}, {"phases", measurement.active_power_l_w},
            {"voltage", measurement.voltage}, {"current", json::array()},
            {"gas", finite(measurement.totalGasM3)}, {"gasTimestamp", measurement.gasTimestamp},
            {"tariff", measurement.activeTariff}, {"wifiSsid", measurement.wifiSsid},
            {"wifiStrength", measurement.wifiStrength}};
        for (double current : measurement.current) result["measurement"]["current"].push_back(finite(current));
        result["gasSession"] = {{"used", std::isfinite(gasSession.firstTotal) && !gasSession.meterDecreased ?
            finite(gasSession.latestTotal - gasSession.firstTotal) : json(nullptr)},
            {"meterDecreased", gasSession.meterDecreased}};
        for (const auto &sample : samples) {
            if (sample.time < result["now"].get<double>() - HISTORY_SECONDS) continue;
            result["samples"].push_back({{"t", sample.time}, {"grid", sample.active_power_w},
                {"l1", sample.active_power_l_w[0]}, {"l2", sample.active_power_l_w[1]},
                {"l3", sample.active_power_l_w[2]}, {"gas", finite(sample.totalGasM3)},
                {"breakBefore", sample.breakBefore}});
        }
    }
    for (const auto &light : temperatures.lights)
        result["hue"]["lights"].push_back({{"id", light.id}, {"name", light.name},
            {"room", light.room}, {"roomId", light.roomId}, {"reachable", light.reachable},
            {"on", light.on}, {"brightness", light.brightness >= 0 ? json(light.brightness) : json(nullptr)}});
    for (const auto &sensor : temperatures.sensors) {
        json history = json::array();
        for (const auto &point : sensor.history)
            history.push_back({{"t", point.time}, {"value", std::isfinite(point.celsius) ? json(point.celsius) : json(nullptr)}});
        result["hue"]["sensors"].push_back({{"id", sensor.id}, {"name", sensor.name}, {"history", history}});
    }
    return result;
}

int main(int argc, char **argv) {
    programStarted = std::chrono::system_clock::now();
#ifdef P1SCOPE_HEADLESS
    bool serverMode = true;
#else
    bool serverMode = false;
#endif
    std::string bindAddress = "127.0.0.1";
    unsigned short httpPort = 8080;
    std::string webRoot = P1SCOPE_WEB_ROOT;
    std::string meterEndpoint;
    if (const char *value = std::getenv("P1SCOPE_METER_ADDRESS")) meterEndpoint = value;

    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--help" || option == "-h") {
            printHelp(argv[0]);
            return 0;
        }
        if (option == "--report" || option == "-v")
            printCompleteJson = true;
        else if (option == "--simulation" || option == "--simulated" || option == "-s")
            simulation = true;
        else if (option == "--server")
            serverMode = true;
        else if (option == "--meter-address") {
            if (i + 1 >= argc) { logStderr() << option << " requires IPv4[:PORT]\n"; return 1; }
            meterEndpoint = argv[++i];
        }
        else if (option == "--bind" || option == "--port" || option == "--web-root") {
            if (i + 1 >= argc) { logStderr() << option << " requires a value\n"; return 1; }
            const std::string value = argv[++i];
            if (option == "--bind") bindAddress = value;
            else if (option == "--web-root") webRoot = value;
            else {
                try {
                    size_t consumed = 0;
                    const int port = std::stoi(value, &consumed);
                    if (consumed != value.size() || port < 1 || port > 65535) throw std::out_of_range("port");
                    httpPort = static_cast<unsigned short>(port);
                } catch (...) { logStderr() << "Invalid HTTP port\n"; return 1; }
            }
        }
        else if (option == "--config") {
            if (i + 1 >= argc || std::string(argv[i + 1]).starts_with("--")) {
                logStderr() << "--config requires a file path\n";
                return 1;
            }
            groupsConfigPath = argv[++i];
        } else {
            logStderr() << "Unknown option: " << option << '\n';
            printHelp(argv[0]);
            return 1;
        }
    }

    if (!meterEndpoint.empty()) {
        const auto colon = meterEndpoint.find(':');
        configuredMeterAddress = meterEndpoint.substr(0, colon);
        in_addr address{};
        if (inet_pton(AF_INET, configuredMeterAddress.c_str(), &address) != 1) {
            logStderr() << "Invalid meter address: expected IPv4[:PORT]\n"; return 1;
        }
        if (colon != std::string::npos) {
            const auto value = meterEndpoint.substr(colon + 1);
            try {
                if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
                    throw std::invalid_argument("port");
                const auto port = std::stoul(value);
                if (port < 1 || port > 65535) throw std::out_of_range("port");
                configuredMeterPort = static_cast<unsigned short>(port);
            } catch (...) { logStderr() << "Invalid meter port: expected 1-65535\n"; return 1; }
        }
    }
    loadGroupsConfig();

    logStderr() << "P1Scope started\n";

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        logStderr() << "Failed to initialize cURL globally\n";
        return 1;
    }
    struct CurlCleanup {
        ~CurlCleanup() { curl_global_cleanup(); }
    } curlCleanup;
    HueTemperatures hue(simulation);
    std::thread reader(p1Thread);

    if (serverMode) {
        std::signal(SIGINT, stopServer);
        std::signal(SIGTERM, stopServer);
        const int result = serveDashboard(bindAddress, httpPort, webRoot, running,
            [] { return serverStopRequested != 0; },
            [&] { return serverSnapshot(hue).dump(); },
            [] { const bool ok = loadGroupsConfig(); return json{{"ok", ok}, {"error", groupsConfigError}}.dump(); },
            [&](const std::string &id, const std::string &action) {
                const auto error = action.starts_with("brightness-") ? hue.setBrightness(id, std::stoi(action.substr(11))) :
                    hue.setLight(id, action == "on");
                return json{{"ok", error.empty()}, {"error", error}}.dump();
            });
        running = false;
        reader.join();
        return result;
    }
#ifndef P1SCOPE_HEADLESS
    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);

    if (!glfwInit()) {
        running = false;
        reader.join();
        return 1;
    }

    // Create the main application window.
    GLFWwindow *window = glfwCreateWindow(1400, 1000, simulation ? "P1Scope [SIMULATION]" : "P1Scope", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        running = false;
        reader.join();
        return 1;
    }

    // Make the window's OpenGL context current and enable vertical synchronization.
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    // Initialize ImGui and ImPlot.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowPadding = ImVec2(14, 12);
    ImGui::GetStyle().ItemSpacing = ImVec2(12, 8);
    ImGui::GetStyle().ChildRounding = 8.0f;
    ImGui::GetStyle().FrameRounding = 4.0f;
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    // GUI loop

    while (running.load() && !glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        drawDashboard(hue.snapshot(), &hue);
        ImGui::Render();


        int width;
        int height;
        glfwGetFramebufferSize(window, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }
    // Shutdown
    running = false;
    reader.join();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();

    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
#endif
    return 0;
}
