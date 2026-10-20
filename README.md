# P1Scope

Live P1 energy monitoring, phase by phase.

A Linux desktop dashboard for a HomeWizard P1 meter, written in C++20 with Dear ImGui and ImPlot. It discovers the meter using Avahi, reads its local HTTP API, and plots total grid power and power per phase.

A simulation mode lets you use the dashboard without a meter or network connection.

## Features

- Live grid import/export and the meter's active tariff.
- Separate power curves for Grid (yellow), L1 (blue), L2 (orange), and L3 (green).
- Phase cards showing power, voltage, and measured current relative to 25 A.
- Selectable 1-, 5-, and 10-minute views, with automatic Y-axis scaling.
- Crosshairs and a shared tooltip showing the nearest sample's time and all four power values.
- Optional dashed reference lines at ±5,750 W per phase.
- Meter Wi-Fi name, signal strength, and connection status.
- Stale-data indication and gaps for failed requests or measurement intervals longer than three seconds.
- Timestamped stderr messages, bounded HTTP requests, and retries during outages.
- Responsive layout: phase cards stack vertically in narrow windows.

## Requirements

### Software

- Linux with an X11 display, or XWayland in a Wayland session. The application explicitly selects GLFW's X11 backend.
- A C++20 compiler and standard library supporting `std::osyncstream`.
- CMake **4.3 or newer**, as specified in `CMakeLists.txt`.
- GLFW **3.4 or newer**, built with X11 support.
- OpenGL, libcurl, nlohmann/json, pkg-config, and Avahi client development libraries.
- Dear ImGui sources in `external/imgui` and ImPlot sources in `external/implot`.

For Debian/Ubuntu-based systems, the dependency package names are:

```bash
sudo apt install build-essential cmake pkg-config git \
    libcurl4-openssl-dev nlohmann-json3-dev libglfw3-dev \
    libgl1-mesa-dev libavahi-client-dev avahi-daemon avahi-utils
```

Check that your distribution provides sufficiently recent CMake and GLFW versions:

```bash
cmake --version
pkg-config --modversion glfw3
```

Use the project's existing ImGui/ImPlot checkouts where available. If these directories are missing, obtain the sources:

```bash
git clone https://github.com/ocornut/imgui.git external/imgui
git clone https://github.com/epezent/implot.git external/implot
```

The code uses ImPlot's `ImPlotSpec` API; older ImPlot versions may not compile. The external dependencies are compiled into the application by CMake.

### Meter connection

For live mode, the computer must be able to discover the meter through local mDNS and reach its local HTTP API. Avahi must be running, and local API access must be available on the meter.

Discovery searches for `_hwenergy._tcp` using IPv4. The application uses the advertised address and port to request:

```text
http://<address>:<port>/api/v1/data
```

The current implementation expects a three-phase report containing numeric values for:

- `active_power_w`
- `active_power_l1_w`, `active_power_l2_w`, `active_power_l3_w`
- `active_voltage_l1_v`, `active_voltage_l2_v`, `active_voltage_l3_v`

Phase currents (`active_current_l1_a`, etc.), `active_tariff`, `wifi_ssid`, and `wifi_strength` are optional. Missing optional readings are displayed as unavailable. Reports missing required readings are rejected.

## Build

From the project directory:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The resulting executable is `build/p1scope`.

In CLion, open the project as a CMake project, select a compatible toolchain, and build the `p1scope` target. Enter command-line options in the run configuration's program arguments.

## Run

Connect to the real meter:

```bash
./build/p1scope
```

Run with simulated data:

```bash
./build/p1scope --simulation
```

Print the simulated JSON reports as well:

```bash
./build/p1scope -s -v
```

| Option | Short form | Description |
| --- | --- | --- |
| `--simulation` | `-s` | Generate local P1 reports without Avahi discovery or HTTP requests. |
| `--report` | `-v` | Print each successful live or simulated JSON report to stdout. |
| `--help` | `-h` | Show usage and examples. |

Options can be combined in any order. Use separate short options (`-s -v`); combined forms such as `-sv` are not supported.

`--report` clears and redraws the terminal for each report. It is a live console view, not a JSON-lines export. To save timestamped diagnostics separately:

```bash
./build/p1scope 2>p1-errors.log
```

Rebuild after modifying the source; changes to help text and behavior do not appear in an older executable.

## Dashboard controls

- **History:** choose 1, 5, or 10 minutes. Up to ten minutes of data are retained in memory, so switching views does not discard the other samples.
- **25 A reference:** show or hide the ±5,750 W reference lines. Hiding them allows the Y-axis to zoom in on smaller variations.
- **Hover:** move over the plot to see the nearest sample's timestamp and power values. Missing-data intervals are identified rather than interpolated in the tooltip.
- **Legend:** click a series' color icon to toggle its curve. Right-click the plot and open **Legend** to change its position or orientation. The default is the top-right corner.
- **Resize:** the plot follows the available window size; small windows can scroll to reach the phase cards.

The initial time window grows from the first reading, so an empty area to the right is normal during startup. After the selected duration has elapsed, the window scrolls with time.

Positive phase power represents import and negative phase power represents export. The grid header shows the total's magnitude with an import/export label. Live total and phase values are taken independently from the meter; the application does not recalculate the reported total.

The current bars use the magnitude of measured phase current, not an estimate calculated from active power. The ±5,750 W lines are based on **25 A × 230 V at power factor 1**, apply to each phase individually, and are not exact fuse trip thresholds. Reference values are configured in `main.cpp` using `MAIN_FUSE_CURRENT_A` and `NOMINAL_PHASE_VOLTAGE_V`.

## Simulation

Simulation generates a report approximately every second and feeds it through the same measurement and display path as live data. The window title and connection status explicitly identify simulated data.

Generated phase powers follow offset sine waves with random noise. Voltage varies between 227 and 233 V, Wi-Fi strength varies between 75 and 100%, and total power equals the sum of the three phases. Simulated currents assume power factor 1. This is demonstration data, not a model of a specific appliance.

The simulated tariff follows the computer's local time every day:

- **06:00–23:00:** tariff 2, normal/peak.
- **23:00–06:00:** tariff 1, low/off-peak.

Live mode always uses the tariff reported by the meter.

## Connection behavior and troubleshooting

### No meter found

Startup discovery waits approximately five seconds. If it fails, the application logs the failure and shuts down. Check the meter's connectivity, Avahi, and whether mDNS can pass between the computer and meter:

```bash
avahi-browse -rt _hwenergy._tcp
```

Simulation mode does not require discovery or a connected meter.

### Timeouts or connection failures

HTTP requests have a two-second connection timeout and a five-second total timeout. After failures, retry delays increase through 2, 4, 8, 16, and 30 seconds. During an outage, application request errors are logged at most once every 30 seconds. Successful readings reset the retry delay and produce a recovery message.

Retries use the originally discovered address and port; there is no automatic rediscovery. If the meter's address changes, restart the application. Otherwise, check connectivity and the meter's HTTP service. Detailed cURL errors are included in stderr.

Displayed readings are marked stale after a failed request or when the last successful reading is over three seconds old. Closing the window cancels retry waits and requests shutdown of active transfers.

### Incorrect time

Plot timestamps, tooltips, and diagnostic timestamps use the computer's local time. Check its clock and timezone if the display differs by a fixed number of hours. System-clock timestamps account for time spent asleep. A backward clock correction clears plot history to keep samples ordered.

### Wayland or display errors

The program explicitly selects X11 to avoid the previously observed Wayland/GDK protocol error. Under Wayland, XWayland must be available. Simulation still needs a graphical display; it is not a headless mode.

### CMake or dependency errors

Confirm the CMake and GLFW minimum versions above. If CMake fails with a Snap/AppArmor error before configuration starts, repair that installation or select a working CMake executable in your shell or CLion toolchain.

## Project files

| File or directory | Purpose |
| --- | --- |
| `main.cpp` | Command-line options, HTTP polling, simulation, measurements, and dashboard. |
| `HomeWizardDiscovery.hxx` / `.cxx` | Avahi discovery and cancellation. |
| `Logging.hxx` | Thread-safe stderr messages with local timestamps. |
| `CMakeLists.txt` | Build configuration and dependencies. |
| `P1.json` | Example meter response. |
| `external/imgui` | Dear ImGui sources and GLFW/OpenGL backends. |
| `external/implot` | ImPlot sources. |

History is kept in memory only and is lost when the program exits. The application does not currently record sessions or export measurements.
