#include "HomeWizardDiscovery.hxx"
#include "Logging.hxx"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"
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

using namespace std;
using json = nlohmann::ordered_json; // Prevent dump to reorder

static bool printCompleteJson = false;
static bool simulation = false;
static string p1_ip_address = "";

struct Sample {
    double time;
    double active_power_w;
    double active_power_l_w[3];
    bool breakBefore = false;
};

static std::deque<Sample> samples;
static std::mutex samplesMutex;
static std::atomic<bool> running{true};
constexpr double HISTORY_SECONDS = 600.0;
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
    samples.push_back({timestamp, next.active_power_w,
        {next.active_power_l_w[0], next.active_power_l_w[1], next.active_power_l_w[2]},
        breakBefore});
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
    report["active_power_w"] = total;
    return report;
}

static void p1Thread() {
    HomeWizardDiscovery discovery;
    CURL *curl = nullptr;
    if (simulation) {
        logStderr() << "Simulation enabled: generating P1 readings locally\n";
    } else {
        if (!discovery.discover(running)) {
            if (running.load())
                logStderr() << "No HomeWizard P1 meter found\n";
            running = false;
            return;
        }


        std::cout << "HomeWizard P1 found\n"
                << "  hostname: " << discovery.hostname() << '\n'
                << "  address : " << discovery.ip() << '\n'
                << "  port    : " << discovery.port() << '\n';

        curl = initCurl(discovery.ip(), discovery.port());
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
    const std::string currentIp = discovery.ip();
    const std::uint16_t currentPort = discovery.port();

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
        next.updated = std::chrono::system_clock::now();
        publishMeasurement(next);

        if (printCompleteJson)
            cout << "\033[2J\033[H" << j.dump(4) << '\n';

        waitForNextPoll();
    }

    if (curl)
        curl_easy_cleanup(curl);
}

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

static void printHelp(const string& programName) {
    string help = R"(
P1Scope - Live P1 Energy Monitor

USAGE: _progname [OPTIONS]

OPTIONS:
    --report, -v   : Print each live or simulated P1 JSON report
    --simulation, -s : Generate random P1 reports once per second;
                    no meter, Avahi discovery or HTTP connection required
    --help, -h    : Show this help

Options can be combined in any order.

)";
    help.replace(help.find("_progname"), 9, programName);
    cout << help
         << "EXAMPLES:\n"
         << "    " << programName << " --simulation\n"
         << "    " << programName << " --simulation --report\n\n";
}

static void drawDashboard() {
    static bool showReference = true;
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
    const double now = plotTime();
    double first;
    std::vector<double> x;
    std::vector<double> y[4]; // Grid total, L1, L2, L3
    {
        std::lock_guard<std::mutex> lock(samplesMutex);
        displayed = measurement;
        first = firstSampleTime;
        while (!samples.empty() && samples.front().time < now - HISTORY_SECONDS)
            samples.pop_front();
        x.reserve(samples.size());
        for (auto &series : y)
            series.reserve(samples.size());
        for (const auto &s: samples) {
            if (s.breakBefore && !x.empty()) {
                x.push_back((x.back() + s.time) * 0.5);
                for (auto &series : y)
                    series.push_back(std::numeric_limits<double>::quiet_NaN());
            }
            x.push_back(s.time);
            y[0].push_back(s.active_power_w);
            for (size_t phase = 0; phase < 3; ++phase)
                y[phase + 1].push_back(s.active_power_l_w[phase]);
        }
    }

    const double age = displayed.valid ? std::max(0.0, std::chrono::duration<double>(
        std::chrono::system_clock::now() - displayed.updated).count()) : 0.0;
    const bool stale = displayed.valid && (displayed.requestFailed || age > 3.0);
    if (ImGui::BeginTable("Header", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Power", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Connection", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableNextColumn();
        if (displayed.valid) {
            ImGui::Text("Grid %s: %.0f W", displayed.active_power_w >= 0 ? "import" : "export",
                        std::abs(displayed.active_power_w));
            ImGui::TextWrapped("Tariff: %s", displayed.activeTariff.c_str());
        } else {
            ImGui::TextWrapped("Waiting for meter data...");
        }
        ImGui::TableNextColumn();
        const std::string wifi = "Wi-Fi: " + displayed.wifiSsid + " | " +
            (displayed.wifiStrength >= 0 ? std::to_string(displayed.wifiStrength) + "%" : "unavailable");
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f,
            ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(wifi.c_str()).x));
        ImGui::TextWrapped("%s", wifi.c_str());
        if (stale)
            ImGui::TextColored(ImVec4(1, 0.65f, 0.25f, 1), "Stale data - %.0f s ago", age);
        else if (displayed.valid)
            ImGui::TextColored(ImVec4(0.3f, 0.85f, 0.55f, 1), simulation ? "SIMULATION - generated data" : "Connected - live data");
        else
            ImGui::TextDisabled(displayed.requestFailed ? "Unavailable - retrying" : "Connecting...");
        ImGui::EndTable();
    }
    ImGui::Separator();
    static int timeRange = 2;
    const double durations[] = {60.0, 300.0, 600.0};
    ImGui::TextUnformatted("History");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    ImGui::Combo("##History", &timeRange, "1 minute\0" "5 minutes\0" "10 minutes\0");
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
    const float cardHeight = ImGui::GetTextLineHeightWithSpacing() * 4.0f +
        ImGui::GetFrameHeight() + 2.0f * ImGui::GetStyle().WindowPadding.y;
    const float cardsHeight = (stackedCards ? 3 : 1) *
        (cardHeight + ImGui::GetStyle().ItemSpacing.y);
    const ImVec2 plotSize(-1, std::max(160.0f, ImGui::GetContentRegionAvail().y - cardsHeight));

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
    const float cardWidth = stackedCards ? ImGui::GetContentRegionAvail().x :
        (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 3.0f;
    for (size_t phase = 0; phase < 3; ++phase) {
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
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopID();
    }

    ImGui::End();
}

int main(int argc, char **argv) {

    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--help" || option == "-h") {
            printHelp(argv[0]);
            return 0;
        }
        if (option == "--report" || option == "-v")
            printCompleteJson = true;
        else if (option == "--simulation" || option == "-s")
            simulation = true;
        else {
            logStderr() << "Unknown option: " << option << '\n';
            printHelp(argv[0]);
            return 1;
        }
    }

    logStderr() << "P1Scope started\n";

    if (!simulation && curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        logStderr() << "Failed to initialize cURL globally\n";
        return 1;
    }
    struct CurlCleanup {
        ~CurlCleanup() { if (!simulation) curl_global_cleanup(); }
    } curlCleanup;
    std::thread reader(p1Thread);

    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);

    if (!glfwInit()) {
        running = false;
        reader.join();
        return 1;
    }

    // Create the main application window.
    GLFWwindow *window = glfwCreateWindow(1000, 600, simulation ? "P1Scope [SIMULATION]" : "P1Scope", nullptr, nullptr);
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

        drawDashboard();
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

    return 0;
}
