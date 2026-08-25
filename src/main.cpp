#include "protocol.hpp"
#include "simulation.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

std::atomic<bool> keep_running{true};

struct Options {
    int grid = 64;
    int simulation_hz = 20;
    int send_hz = 10;
    int pressure_iterations = 12;
    std::string host = "127.0.0.1";
    std::uint16_t port = 7777;
    float run_seconds = 0.0f;
    bool send = true;
    bool self_test = false;
};

void signalHandler(int) {
    keep_running.store(false);
}

void printUsage(const char* executable) {
    std::cout
        << "Cloud simulation server 0.1.0\n\n"
        << "Usage: " << executable << " [options]\n"
        << "  --grid N             Cubic grid size, 16..192 (default 64)\n"
        << "  --hz N               Simulation steps per second (default 20)\n"
        << "  --send-hz N          Density frames per second (default 10)\n"
        << "  --pressure-iters N   Pressure solver iterations (default 12)\n"
        << "  --host IPv4          UDP destination (default 127.0.0.1)\n"
        << "  --port N             UDP destination port (default 7777)\n"
        << "  --seconds N          Stop after N seconds; 0 runs forever\n"
        << "  --no-send            Simulate without UDP transmission\n"
        << "  --self-test          Run deterministic health check and exit\n"
        << "  --help               Show this help\n";
}

int parseInt(const char* value, const std::string& option) {
    try {
        std::size_t consumed = 0;
        const int parsed = std::stoi(value, &consumed);
        if (consumed != std::string(value).size()) {
            throw std::invalid_argument("trailing characters");
        }
        return parsed;
    } catch (...) {
        throw std::invalid_argument("Invalid integer for " + option + ": " + value);
    }
}

float parseFloat(const char* value, const std::string& option) {
    try {
        std::size_t consumed = 0;
        const float parsed = std::stof(value, &consumed);
        if (consumed != std::string(value).size()) {
            throw std::invalid_argument("trailing characters");
        }
        return parsed;
    } catch (...) {
        throw std::invalid_argument("Invalid number for " + option + ": " + value);
    }
}

Options parseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        auto requireValue = [&](const std::string& option) -> const char* {
            if (i + 1 >= argc) {
                throw std::invalid_argument("Missing value for " + option);
            }
            return argv[++i];
        };

        if (argument == "--grid") {
            options.grid = parseInt(requireValue(argument), argument);
        } else if (argument == "--hz") {
            options.simulation_hz = parseInt(requireValue(argument), argument);
        } else if (argument == "--send-hz") {
            options.send_hz = parseInt(requireValue(argument), argument);
        } else if (argument == "--pressure-iters") {
            options.pressure_iterations = parseInt(requireValue(argument), argument);
        } else if (argument == "--host") {
            options.host = requireValue(argument);
        } else if (argument == "--port") {
            const int port = parseInt(requireValue(argument), argument);
            if (port < 1 || port > 65535) {
                throw std::invalid_argument("Port must be between 1 and 65535");
            }
            options.port = static_cast<std::uint16_t>(port);
        } else if (argument == "--seconds") {
            options.run_seconds = parseFloat(requireValue(argument), argument);
        } else if (argument == "--no-send") {
            options.send = false;
        } else if (argument == "--self-test") {
            options.self_test = true;
        } else if (argument == "--help" || argument == "-h") {
            printUsage(argv[0]);
            std::exit(0);
        } else {
            throw std::invalid_argument("Unknown option: " + argument);
        }
    }

    if (options.simulation_hz < 1 || options.simulation_hz > 240) {
        throw std::invalid_argument("--hz must be between 1 and 240");
    }
    if (options.send_hz < 1 || options.send_hz > options.simulation_hz) {
        throw std::invalid_argument("--send-hz must be between 1 and --hz");
    }
    if (options.run_seconds < 0.0f) {
        throw std::invalid_argument("--seconds cannot be negative");
    }
    return options;
}

int runSelfTest() {
    cloud::CloudSimulation simulation(24, 6);
    for (int i = 0; i < 80; ++i) {
        simulation.step(0.05f);
    }

    const auto density = simulation.densityBytes();
    const auto stats = simulation.densityStats();
    const bool size_ok = density.size() == 24U * 24U * 24U;
    const bool cloud_formed = stats.maximum > 0.001f;
    const bool finite = simulation.allFinite();
    const bool protocol_ok = sizeof(cloud::PacketHeader) == 40;

    std::cout << "Self-test: voxels=" << density.size()
              << " cloud_max=" << stats.maximum
              << " cloud_mean=" << stats.mean
              << " finite=" << (finite ? "yes" : "no")
              << " protocol_header=" << sizeof(cloud::PacketHeader) << " bytes\n";

    if (!size_ok || !cloud_formed || !finite || !protocol_ok) {
        std::cerr << "Self-test FAILED\n";
        return 1;
    }
    std::cout << "Self-test PASSED\n";
    return 0;
}

int runServer(const Options& options) {
    cloud::CloudSimulation simulation(options.grid, options.pressure_iterations);
    std::unique_ptr<cloud::UdpSender> sender;
    if (options.send) {
        sender = std::make_unique<cloud::UdpSender>(options.host, options.port);
    }

    std::cout << "Cloud server started: " << options.grid << "^3, "
              << options.simulation_hz << " simulation Hz, "
              << options.send_hz << " send Hz";
    if (sender) {
        std::cout << ", UDP " << options.host << ':' << options.port;
    } else {
        std::cout << ", UDP disabled";
    }
    std::cout << "\nPress Ctrl+C to stop.\n";

    using clock = std::chrono::steady_clock;
    const float dt = 1.0f / static_cast<float>(options.simulation_hz);
    const auto tick_duration = std::chrono::duration<double>(dt);
    const float send_interval = 1.0f / static_cast<float>(options.send_hz);
    float send_accumulator = 0.0f;
    std::uint32_t frame_id = 0;
    auto next_tick = clock::now();
    auto next_log = next_tick + std::chrono::seconds(1);
    const auto start_time = next_tick;

    while (keep_running.load()) {
        const auto now = clock::now();
        if (options.run_seconds > 0.0f &&
            std::chrono::duration<float>(now - start_time).count() >= options.run_seconds) {
            break;
        }

        simulation.step(dt);
        send_accumulator += dt;
        if (sender && send_accumulator + 0.000001f >= send_interval) {
            sender->sendDensity(
                simulation.densityBytes(), options.grid, frame_id++, simulation.time());
            send_accumulator = std::max(0.0f, send_accumulator - send_interval);
        }

        if (now >= next_log) {
            const auto stats = simulation.densityStats();
            std::cout << std::fixed << std::setprecision(4)
                      << "t=" << simulation.time() << "s"
                      << " frame=" << frame_id
                      << " cloud[max=" << stats.maximum << ", mean=" << stats.mean << "]\n";
            next_log = now + std::chrono::seconds(1);
        }

        next_tick += std::chrono::duration_cast<clock::duration>(tick_duration);
        const auto after_step = clock::now();
        if (after_step < next_tick) {
            std::this_thread::sleep_until(next_tick);
        } else if (after_step - next_tick > std::chrono::seconds(1)) {
            next_tick = after_step;
        }
    }

    std::cout << "Cloud server stopped at t=" << simulation.time() << "s.\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (options.self_test) {
            return runSelfTest();
        }

        std::signal(SIGINT, signalHandler);
        std::signal(SIGTERM, signalHandler);
        return runServer(options);
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
