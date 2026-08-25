#include "control.hpp"
#include "environment.hpp"
#include "protocol.hpp"
#include "simulation.hpp"
#include "sky_control.hpp"
#include "sky_protocol.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

volatile std::sig_atomic_t keep_running = 1;

constexpr std::uint16_t kAllV2Fields =
    (1U << (static_cast<unsigned>(cloud::FieldId::Density) - 1U)) |
    (1U << (static_cast<unsigned>(cloud::FieldId::Velocity) - 1U)) |
    (1U << (static_cast<unsigned>(cloud::FieldId::Temperature) - 1U)) |
    (1U << (static_cast<unsigned>(cloud::FieldId::Vapor) - 1U)) |
    (1U << (static_cast<unsigned>(cloud::FieldId::Occupancy) - 1U));

struct Options {
  int grid = 64;
  int simulation_hz = 20;
  int send_hz = 10;
  int pressure_iterations = 12;
  int protocol = 2;
  int occupancy_brick = 4;
  std::uint16_t field_mask = kAllV2Fields;
  cloud::CompressionMode compression = cloud::CompressionMode::Auto;
  std::string host = "127.0.0.1";
  std::uint16_t port = 7777;
  std::string control_host = "127.0.0.1";
  std::uint16_t control_port = 7778;
  std::string sky_host;
  std::uint16_t sky_port = 7779;
  int sky_send_hz = 5;
  std::string sky_control_host = "127.0.0.1";
  std::uint16_t sky_control_port = 7780;
  double utc_unix_seconds = std::numeric_limits<double>::quiet_NaN();
  double latitude_degrees = 37.5665;
  double longitude_degrees = 126.9780;
  double elevation_m = 38.0;
  double time_scale = 1.0;
  double domain_width_m = 20000.0;
  double domain_height_m = 12000.0;
  cloud::WeatherPreset weather_preset = cloud::WeatherPreset::Natural;
  std::uint32_t weather_seed = 1U;
  int max_interactors = 8;
  double run_seconds = 0.0;
  double warmup_seconds = 2.0;
  bool fields_explicit = false;
  bool send = true;
  bool control = true;
  bool sky_send = true;
  bool sky_control = true;
  bool self_test = false;
};

void signalHandler(int) { keep_running = 0; }

void printUsage(const char *executable) {
  std::cout
      << "Sky simulation server 0.4.0\n\n"
      << "Usage: " << executable << " [options]\n"
      << "  --grid N             Cubic grid size, 16..192 (default 64)\n"
      << "  --hz N               Simulation steps per second (default 20)\n"
      << "  --send-hz N          Volume frames per second (default 10)\n"
      << "  --pressure-iters N   Pressure solver iterations (default 12)\n"
      << "  --protocol N         UDP protocol 1 or 2 (default 2)\n"
      << "  --fields LIST        v2 fields: "
         "density,velocity,temperature,vapor,occupancy\n"
      << "  --compression MODE   v2 compression: auto, none, rle (default "
         "auto)\n"
      << "  --occupancy-brick N  Occupancy cell width: 2, 4, or 8 (default 4)\n"
      << "  --host IPv4          UDP destination (default 127.0.0.1)\n"
      << "  --port N             UDP destination port (default 7777)\n"
      << "  --control-host IPv4  Interactor bind address (default 127.0.0.1)\n"
      << "  --control-port N     Unreal-to-server interactor port (default "
         "7778)\n"
      << "  --sky-host IPv4      SKS1 destination; defaults to --host\n"
      << "  --sky-port N         SKS1 environment state port (default 7779)\n"
      << "  --sky-hz N           SKS1 state snapshots per second (default 5)\n"
      << "  --sky-control-host IP  SKC1 bind address (default 127.0.0.1)\n"
      << "  --sky-control-port N SKC1 weather control port (default 7780)\n"
      << "  --utc VALUE          UTC Unix seconds or YYYY-MM-DDTHH:MM:SSZ\n"
      << "  --latitude N         Volume-center latitude degrees (default 37.5665)\n"
      << "  --longitude N        Volume-center longitude degrees (default 126.978)\n"
      << "  --elevation-m N      Volume floor elevation AMSL (default 38)\n"
      << "  --time-scale N       UTC seconds per wall second; 0 pauses (default 1)\n"
      << "  --weather NAME       natural,clear,cumulus,overcast,rain,storm,snow,fog\n"
      << "                       (default natural)\n"
      << "  --weather-seed N     Deterministic natural-weather seed (default 1)\n"
      << "  --domain-width-m N   Horizontal volume width in metres (default 20000)\n"
      << "  --domain-height-m N  Vertical volume height in metres (default 12000)\n"
      << "  --max-interactors N  Maximum simultaneous objects, 1..64 (default "
         "8)\n"
      << "  --warmup-seconds N   Simulate before the first transmitted frame "
         "(default 2)\n"
      << "  --seconds N          Stop after N wall-clock seconds; 0 runs "
         "forever\n"
      << "  --no-send            Disable CLD1/CLD2 volume transmission\n"
      << "  --no-control         Disable dynamic object interaction input\n"
      << "  --no-sky             Disable SKS1 state transmission\n"
      << "  --no-sky-control     Disable SKC1 weather control input\n"
      << "  --self-test          Run deterministic health and codec checks\n"
      << "  --help               Show this help\n";
}

int parseInt(const char *value, const std::string &option) {
  try {
    std::size_t consumed = 0;
    const std::string text = value;
    const int parsed = std::stoi(text, &consumed);
    if (consumed != text.size()) {
      throw std::invalid_argument("trailing characters");
    }
    return parsed;
  } catch (...) {
    throw std::invalid_argument("Invalid integer for " + option + ": " + value);
  }
}

double parseDouble(const char *value, const std::string &option) {
  try {
    std::size_t consumed = 0;
    const std::string text = value;
    const double parsed = std::stod(text, &consumed);
    if (consumed != text.size() || !std::isfinite(parsed)) {
      throw std::invalid_argument("not finite or has trailing characters");
    }
    return parsed;
  } catch (...) {
    throw std::invalid_argument("Invalid finite number for " + option + ": " +
                                value);
  }
}

std::uint32_t parseUint32(const char *value, const std::string &option) {
  try {
    const std::string text = value;
    std::size_t consumed = 0;
    const unsigned long long parsed = std::stoull(text, &consumed);
    if (consumed != text.size() || parsed == 0ULL ||
        parsed > std::numeric_limits<std::uint32_t>::max()) {
      throw std::invalid_argument("outside uint32 range");
    }
    return static_cast<std::uint32_t>(parsed);
  } catch (...) {
    throw std::invalid_argument("Invalid nonzero uint32 for " + option +
                                ": " + value);
  }
}

bool leapYear(int year) {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

std::int64_t daysFromCivil(int year, unsigned month, unsigned day) {
  year -= month <= 2U;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
  const unsigned day_of_year =
      (153U * (month > 2U ? month - 3U : month + 9U) + 2U) / 5U + day -
      1U;
  const unsigned day_of_era =
      year_of_era * 365U + year_of_era / 4U - year_of_era / 100U +
      day_of_year;
  return static_cast<std::int64_t>(era) * 146097LL +
         static_cast<std::int64_t>(day_of_era) - 719468LL;
}

double parseUtc(const char *value, const std::string &option) {
  const std::string text = value;
  try {
    std::size_t consumed = 0;
    const double parsed = std::stod(text, &consumed);
    if (consumed == text.size() && std::isfinite(parsed)) {
      return parsed;
    }
  } catch (...) {
  }

  if (text.size() != 20U || text[4] != '-' || text[7] != '-' ||
      text[10] != 'T' || text[13] != ':' || text[16] != ':' ||
      text[19] != 'Z') {
    throw std::invalid_argument(
        "Invalid UTC for " + option +
        "; use Unix seconds or YYYY-MM-DDTHH:MM:SSZ");
  }
  auto component = [&](std::size_t offset, std::size_t length) {
    for (std::size_t index = offset; index < offset + length; ++index) {
      if (text[index] < '0' || text[index] > '9') {
        throw std::invalid_argument("UTC contains a non-digit component");
      }
    }
    return std::stoi(text.substr(offset, length));
  };
  try {
    const int year = component(0, 4);
    const int month = component(5, 2);
    const int day = component(8, 2);
    const int hour = component(11, 2);
    const int minute = component(14, 2);
    const int second = component(17, 2);
    constexpr int month_days[] = {31, 28, 31, 30, 31, 30,
                                  31, 31, 30, 31, 30, 31};
    if (year < 1900 || year > 2100 || month < 1 || month > 12 || day < 1 ||
        day > month_days[month - 1] +
                  ((month == 2 && leapYear(year)) ? 1 : 0) ||
        hour > 23 || minute > 59 || second > 59) {
      throw std::invalid_argument("UTC component is outside its range");
    }
    return static_cast<double>(daysFromCivil(
               year, static_cast<unsigned>(month), static_cast<unsigned>(day))) *
               86400.0 +
           static_cast<double>(hour * 3600 + minute * 60 + second);
  } catch (...) {
    throw std::invalid_argument("Invalid UTC for " + option + ": " + text);
  }
}

std::uint16_t parseFieldMask(const std::string &value) {
  if (value == "all" || value == "render") {
    return kAllV2Fields;
  }

  std::uint16_t mask = 0;
  std::size_t start = 0;
  while (start <= value.size()) {
    const std::size_t comma = value.find(',', start);
    const std::string token = value.substr(start, comma - start);
    cloud::FieldId field;
    if (token == "density") {
      field = cloud::FieldId::Density;
    } else if (token == "velocity") {
      field = cloud::FieldId::Velocity;
    } else if (token == "temperature") {
      field = cloud::FieldId::Temperature;
    } else if (token == "vapor") {
      field = cloud::FieldId::Vapor;
    } else if (token == "occupancy") {
      field = cloud::FieldId::Occupancy;
    } else {
      throw std::invalid_argument("Unknown field in --fields: " + token);
    }
    const std::uint16_t bit = cloud::fieldBit(field);
    if ((mask & bit) != 0U) {
      throw std::invalid_argument("Duplicate field in --fields: " + token);
    }
    mask = static_cast<std::uint16_t>(mask | bit);
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }
  return mask;
}

cloud::CompressionMode parseCompression(const std::string &value) {
  if (value == "auto") {
    return cloud::CompressionMode::Auto;
  }
  if (value == "none") {
    return cloud::CompressionMode::None;
  }
  if (value == "rle") {
    return cloud::CompressionMode::Rle;
  }
  throw std::invalid_argument("--compression must be auto, none, or rle");
}

const char *compressionName(cloud::CompressionMode mode) {
  switch (mode) {
  case cloud::CompressionMode::None:
    return "none";
  case cloud::CompressionMode::Rle:
    return "rle";
  case cloud::CompressionMode::Auto:
    return "auto";
  }
  return "unknown";
}

std::string fieldMaskName(std::uint16_t mask) {
  struct NamedField {
    cloud::FieldId id;
    const char *name;
  };
  constexpr NamedField fields[] = {
      {cloud::FieldId::Density, "density"},
      {cloud::FieldId::Velocity, "velocity"},
      {cloud::FieldId::Temperature, "temperature"},
      {cloud::FieldId::Vapor, "vapor"},
      {cloud::FieldId::Occupancy, "occupancy"},
  };
  std::string result;
  for (const NamedField &field : fields) {
    if ((mask & cloud::fieldBit(field.id)) == 0U) {
      continue;
    }
    if (!result.empty()) {
      result += ',';
    }
    result += field.name;
  }
  return result;
}

Options parseOptions(int argc, char **argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    auto requireValue = [&](const std::string &option) -> const char * {
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
    } else if (argument == "--protocol") {
      options.protocol = parseInt(requireValue(argument), argument);
    } else if (argument == "--fields") {
      options.field_mask = parseFieldMask(requireValue(argument));
      options.fields_explicit = true;
    } else if (argument == "--compression") {
      options.compression = parseCompression(requireValue(argument));
    } else if (argument == "--occupancy-brick") {
      options.occupancy_brick = parseInt(requireValue(argument), argument);
    } else if (argument == "--host") {
      options.host = requireValue(argument);
    } else if (argument == "--port") {
      const int port = parseInt(requireValue(argument), argument);
      if (port < 1 || port > 65535) {
        throw std::invalid_argument("Port must be between 1 and 65535");
      }
      options.port = static_cast<std::uint16_t>(port);
    } else if (argument == "--control-host") {
      options.control_host = requireValue(argument);
    } else if (argument == "--control-port") {
      const int port = parseInt(requireValue(argument), argument);
      if (port < 1 || port > 65535) {
        throw std::invalid_argument("Control port must be between 1 and 65535");
      }
      options.control_port = static_cast<std::uint16_t>(port);
    } else if (argument == "--sky-host") {
      options.sky_host = requireValue(argument);
    } else if (argument == "--sky-port") {
      const int port = parseInt(requireValue(argument), argument);
      if (port < 1 || port > 65535) {
        throw std::invalid_argument("Sky port must be between 1 and 65535");
      }
      options.sky_port = static_cast<std::uint16_t>(port);
    } else if (argument == "--sky-hz") {
      options.sky_send_hz = parseInt(requireValue(argument), argument);
    } else if (argument == "--sky-control-host") {
      options.sky_control_host = requireValue(argument);
    } else if (argument == "--sky-control-port") {
      const int port = parseInt(requireValue(argument), argument);
      if (port < 1 || port > 65535) {
        throw std::invalid_argument(
            "Sky control port must be between 1 and 65535");
      }
      options.sky_control_port = static_cast<std::uint16_t>(port);
    } else if (argument == "--utc") {
      options.utc_unix_seconds = parseUtc(requireValue(argument), argument);
    } else if (argument == "--latitude") {
      options.latitude_degrees = parseDouble(requireValue(argument), argument);
    } else if (argument == "--longitude") {
      options.longitude_degrees = parseDouble(requireValue(argument), argument);
    } else if (argument == "--elevation-m") {
      options.elevation_m = parseDouble(requireValue(argument), argument);
    } else if (argument == "--time-scale") {
      options.time_scale = parseDouble(requireValue(argument), argument);
    } else if (argument == "--weather") {
      options.weather_preset =
          cloud::parseWeatherPreset(requireValue(argument));
    } else if (argument == "--weather-seed") {
      options.weather_seed = parseUint32(requireValue(argument), argument);
    } else if (argument == "--domain-width-m") {
      options.domain_width_m = parseDouble(requireValue(argument), argument);
    } else if (argument == "--domain-height-m") {
      options.domain_height_m = parseDouble(requireValue(argument), argument);
    } else if (argument == "--max-interactors") {
      options.max_interactors = parseInt(requireValue(argument), argument);
    } else if (argument == "--seconds") {
      options.run_seconds = parseDouble(requireValue(argument), argument);
    } else if (argument == "--warmup-seconds") {
      options.warmup_seconds = parseDouble(requireValue(argument), argument);
    } else if (argument == "--no-send") {
      options.send = false;
    } else if (argument == "--no-control") {
      options.control = false;
    } else if (argument == "--no-sky") {
      options.sky_send = false;
    } else if (argument == "--no-sky-control") {
      options.sky_control = false;
    } else if (argument == "--self-test") {
      options.self_test = true;
    } else if (argument == "--help" || argument == "-h") {
      printUsage(argv[0]);
      std::exit(0);
    } else {
      throw std::invalid_argument("Unknown option: " + argument);
    }
  }

  if (options.grid < 16 || options.grid > 192) {
    throw std::invalid_argument("--grid must be between 16 and 192");
  }
  if (options.pressure_iterations < 1 || options.pressure_iterations > 80) {
    throw std::invalid_argument("--pressure-iters must be between 1 and 80");
  }
  if (options.simulation_hz < 4 || options.simulation_hz > 240) {
    throw std::invalid_argument("--hz must be between 4 and 240");
  }
  if (options.send_hz < 1 || options.send_hz > options.simulation_hz) {
    throw std::invalid_argument("--send-hz must be between 1 and --hz");
  }
  if (options.sky_send_hz < 1 ||
      options.sky_send_hz > options.simulation_hz) {
    throw std::invalid_argument("--sky-hz must be between 1 and --hz");
  }
  if (options.protocol != 1 && options.protocol != 2) {
    throw std::invalid_argument("--protocol must be 1 or 2");
  }
  if (options.run_seconds < 0.0) {
    throw std::invalid_argument("--seconds cannot be negative");
  }
  if (options.warmup_seconds < 0.0 || options.warmup_seconds > 60.0) {
    throw std::invalid_argument("--warmup-seconds must be between 0 and 60");
  }
  if (options.occupancy_brick != 2 && options.occupancy_brick != 4 &&
      options.occupancy_brick != 8) {
    throw std::invalid_argument("--occupancy-brick must be 2, 4, or 8");
  }
  if (options.max_interactors < 1 || options.max_interactors > 64) {
    throw std::invalid_argument("--max-interactors must be between 1 and 64");
  }
  if (options.latitude_degrees < -90.0 || options.latitude_degrees > 90.0) {
    throw std::invalid_argument("--latitude must be between -90 and 90");
  }
  if (options.longitude_degrees < -180.0 ||
      options.longitude_degrees > 180.0) {
    throw std::invalid_argument("--longitude must be between -180 and 180");
  }
  if (options.elevation_m < -500.0 || options.elevation_m > 100000.0) {
    throw std::invalid_argument(
        "--elevation-m must be between -500 and 100000");
  }
  if (options.time_scale < -86400.0 || options.time_scale > 86400.0) {
    throw std::invalid_argument("--time-scale must be in [-86400, 86400]");
  }
  if (options.domain_width_m < 100.0 ||
      options.domain_width_m > 2000000.0 ||
      options.domain_height_m < 100.0 ||
      options.domain_height_m > 100000.0) {
    throw std::invalid_argument(
        "Domain width/height are outside the supported physical range");
  }
  if (options.sky_host.empty()) {
    options.sky_host = options.host;
  }
  if (options.control && options.sky_control &&
      options.control_host == options.sky_control_host &&
      options.control_port == options.sky_control_port) {
    throw std::invalid_argument(
        "Interactor and sky control ports must be different");
  }
  if (options.protocol == 1 && !options.fields_explicit) {
    options.field_mask = cloud::fieldBit(cloud::FieldId::Density);
  }
  if ((options.field_mask & cloud::fieldBit(cloud::FieldId::Density)) == 0U) {
    throw std::invalid_argument("--fields must include density");
  }
  if (options.protocol == 1 &&
      options.field_mask != cloud::fieldBit(cloud::FieldId::Density)) {
    throw std::invalid_argument("Protocol v1 only supports --fields density");
  }
  return options;
}

cloud::FieldPayload
makeFieldPayload(cloud::QuantizedVolume volume, cloud::FieldId field_id,
                 cloud::VoxelFormat format,
                 std::uint16_t flags = cloud::kFlagKeyframe) {
  cloud::FieldPayload field;
  field.grid_x = static_cast<std::uint16_t>(volume.size_x);
  field.grid_y = static_cast<std::uint16_t>(volume.size_y);
  field.grid_z = static_cast<std::uint16_t>(volume.size_z);
  field.voxel_format = format;
  field.field_id = field_id;
  field.channel_count = volume.channels;
  field.flags = flags;
  field.value_scale = volume.value_scale;
  field.value_bias = volume.value_bias;
  field.bytes = std::move(volume.bytes);
  return field;
}

std::vector<cloud::FieldPayload>
buildRendererFields(const cloud::CloudSimulation &simulation,
                    const Options &options) {
  std::vector<cloud::FieldPayload> fields;
  fields.reserve(5);
  auto selected = [&](cloud::FieldId field) {
    return (options.field_mask & cloud::fieldBit(field)) != 0U;
  };

  if (selected(cloud::FieldId::Density)) {
    fields.push_back(makeFieldPayload(simulation.densityVolume16(),
                                      cloud::FieldId::Density,
                                      cloud::VoxelFormat::UNorm16));
  }
  if (selected(cloud::FieldId::Velocity)) {
    fields.push_back(makeFieldPayload(simulation.velocityVolumeSnorm16(),
                                      cloud::FieldId::Velocity,
                                      cloud::VoxelFormat::SNorm16));
  }
  if (selected(cloud::FieldId::Temperature)) {
    fields.push_back(makeFieldPayload(simulation.temperatureVolume8(),
                                      cloud::FieldId::Temperature,
                                      cloud::VoxelFormat::UNorm8));
  }
  if (selected(cloud::FieldId::Vapor)) {
    fields.push_back(makeFieldPayload(simulation.vaporVolume8(),
                                      cloud::FieldId::Vapor,
                                      cloud::VoxelFormat::UNorm8));
  }
  if (selected(cloud::FieldId::Occupancy)) {
    const std::uint16_t flags = static_cast<std::uint16_t>(
        cloud::kFlagKeyframe |
        (static_cast<std::uint16_t>(options.occupancy_brick)
         << cloud::kOccupancyBrickShift));
    fields.push_back(makeFieldPayload(
        simulation.occupancyVolume8(options.occupancy_brick),
        cloud::FieldId::Occupancy, cloud::VoxelFormat::UNorm8, flags));
  }
  return fields;
}

struct SkyControlRuntime {
  std::uint32_t last_session = 0;
  std::uint32_t last_sequence = 0;
  std::uint8_t last_result = 0;
  cloud::SkyEvolutionMode evolution_mode = cloud::SkyEvolutionMode::Manual;
  bool state_keyframe_requested = false;
};

void applySkyControl(cloud::SkyEnvironment &environment,
                     const cloud::SkyControlCommand &command,
                     SkyControlRuntime &runtime) {
  const double transition_seconds =
      static_cast<double>(command.transition_milliseconds) / 1000.0;
  runtime.last_session = command.session_id;
  runtime.last_sequence = command.sequence;
  runtime.last_result = 1U;

  if (command.opcode == cloud::SkyControlOpcode::RequestKeyframe) {
    runtime.state_keyframe_requested = true;
    return;
  }
  if (command.opcode == cloud::SkyControlOpcode::LoadPreset) {
    environment.setWeatherSeed(command.weather_seed);
    environment.setWeatherPreset(command.preset, transition_seconds);
    runtime.evolution_mode =
        command.preset == cloud::WeatherPreset::Natural
            ? cloud::SkyEvolutionMode::Natural
            : cloud::SkyEvolutionMode::Manual;
    return;
  }
  if (command.opcode == cloud::SkyControlOpcode::ReleaseOverride) {
    environment.releaseWeather(transition_seconds);
    runtime.evolution_mode = cloud::SkyEvolutionMode::Natural;
    return;
  }

  std::size_t prospective_layer_count =
      environment.weatherProfile().cloud_layer_count;
  const float prospective_elevation = static_cast<float>(
      (command.apply_mask & cloud::kSkyApplyLocation) != 0U
          ? command.values.location.elevation_m
          : environment.snapshot().location.elevation_m);
  for (std::size_t index = 0; index < cloud::kMaximumCloudLayers; ++index) {
    if ((command.apply_mask & (cloud::kSkyApplyCloudLayer0 << index)) == 0U) {
      continue;
    }
    if (index > prospective_layer_count) {
      runtime.last_result = 2U;
      return;
    }
    const cloud::CloudLayerState &layer = command.values.cloud_layers[index];
    const float clipped_base =
        std::max(layer.base_altitude_m, prospective_elevation);
    const float clipped_thickness = layer.top_altitude_m - clipped_base;
    if (clipped_thickness < 1.0f) {
      runtime.last_result = 2U;
      return;
    }
    if (index == prospective_layer_count) {
      ++prospective_layer_count;
    }
  }

  if ((command.apply_mask & cloud::kSkyApplyUtc) != 0U) {
    environment.setUtcUnixSeconds(command.values.utc_unix_seconds);
  }
  if ((command.apply_mask & cloud::kSkyApplyLocation) != 0U) {
    environment.setLocation(command.values.location);
  }
  if ((command.apply_mask & cloud::kSkyApplyDomain) != 0U) {
    environment.setDomain(command.values.domain);
  }
  if ((command.apply_mask & cloud::kSkyApplyTimeScale) != 0U) {
    environment.setTimeScale(command.values.time_scale);
  }
  if ((command.apply_mask & cloud::kSkyApplyEvolution) != 0U) {
    environment.setWeatherSeed(command.weather_seed);
    if (command.evolution_mode == cloud::SkyEvolutionMode::Natural) {
      environment.releaseWeather(transition_seconds);
      runtime.evolution_mode = cloud::SkyEvolutionMode::Natural;
    } else {
      runtime.evolution_mode = cloud::SkyEvolutionMode::Manual;
      constexpr std::uint64_t weather_fields =
          cloud::kSkyApplyThermodynamics | cloud::kSkyApplyVisibility |
          cloud::kSkyApplyWind | cloud::kSkyApplyPrecipitation |
          cloud::kSkyApplyConvection | cloud::kSkyApplyCloudLayer0 |
          cloud::kSkyApplyCloudLayer1 | cloud::kSkyApplyCloudLayer2 |
          cloud::kSkyApplyCloudLayer3;
      if ((command.apply_mask & weather_fields) == 0U) {
        environment.step(0.0, 0.0);
        environment.setCustomWeather(environment.weatherProfile(),
                                     transition_seconds);
      }
    }
  }

  constexpr std::uint64_t weather_mask =
      cloud::kSkyApplyThermodynamics | cloud::kSkyApplyVisibility |
      cloud::kSkyApplyWind | cloud::kSkyApplyPrecipitation |
      cloud::kSkyApplyConvection | cloud::kSkyApplyCloudLayer0 |
      cloud::kSkyApplyCloudLayer1 | cloud::kSkyApplyCloudLayer2 |
      cloud::kSkyApplyCloudLayer3;
  if ((command.apply_mask & weather_mask) == 0U) {
    return;
  }

  cloud::WeatherProfile profile = environment.weatherProfile();
  const cloud::WeatherState &source = command.values.weather;
  cloud::WeatherState &target = profile.weather;
  if ((command.apply_mask & cloud::kSkyApplyThermodynamics) != 0U) {
    target.surface_temperature_kelvin = source.surface_temperature_kelvin;
    target.sea_level_pressure_pa = source.sea_level_pressure_pa;
    target.relative_humidity = source.relative_humidity;
  }
  if ((command.apply_mask & cloud::kSkyApplyVisibility) != 0U) {
    target.visibility_m = source.visibility_m;
    target.aerosol_optical_depth_550nm =
        source.aerosol_optical_depth_550nm;
    target.ozone_dobson_units = source.ozone_dobson_units;
  }
  if ((command.apply_mask & cloud::kSkyApplyWind) != 0U) {
    target.wind_m_s = source.wind_m_s;
    target.gust_speed_m_s = source.gust_speed_m_s;
  }
  if ((command.apply_mask & cloud::kSkyApplyPrecipitation) != 0U) {
    target.precipitation_rate_mm_h = source.precipitation_rate_mm_h;
    target.snow_fraction = source.snow_fraction;
    target.surface_wetness = source.surface_wetness;
    environment.setSurfaceWetness(source.surface_wetness);
  }
  if ((command.apply_mask & cloud::kSkyApplyConvection) != 0U) {
    target.convective_activity = source.convective_activity;
    target.lightning_activity = source.lightning_activity;
  }
  const float elevation =
      static_cast<float>(environment.snapshot().location.elevation_m);
  for (std::size_t index = 0; index < cloud::kMaximumCloudLayers; ++index) {
    if ((command.apply_mask & (cloud::kSkyApplyCloudLayer0 << index)) == 0U) {
      continue;
    }
    profile.cloud_layers[index] = command.values.cloud_layers[index];
    const float original_thickness =
        profile.cloud_layers[index].top_altitude_m -
        profile.cloud_layers[index].base_altitude_m;
    const float clipped_base_amsl =
        std::max(profile.cloud_layers[index].base_altitude_m, elevation);
    const float retained_fraction =
        (profile.cloud_layers[index].top_altitude_m - clipped_base_amsl) /
        original_thickness;
    profile.cloud_layers[index].optical_depth *= retained_fraction;
    profile.cloud_layers[index].base_altitude_m = std::max(
        0.0f, profile.cloud_layers[index].base_altitude_m - elevation);
    profile.cloud_layers[index].top_altitude_m -= elevation;
    if (profile.cloud_layers[index].kind ==
            cloud::CloudLayerKind::Stratiform &&
        profile.cloud_layers[index].base_altitude_m <= 5.0f &&
        profile.cloud_layers[index].top_altitude_m <= 1000.0f) {
      profile.cloud_layers[index].kind = cloud::CloudLayerKind::Fog;
    }
    if (index == profile.cloud_layer_count) {
      ++profile.cloud_layer_count;
    }
  }
  environment.setCustomWeather(profile, transition_seconds);
  runtime.evolution_mode = cloud::SkyEvolutionMode::Manual;
}

int runSelfTest() {
  cloud::CloudSimulation simulation(24, 6);
  for (int i = 0; i < 80; ++i) {
    simulation.step(0.05f);
  }

  const auto legacy_density = simulation.densityBytes();
  const auto density = simulation.densityVolume16();
  const auto velocity = simulation.velocityVolumeSnorm16();
  const auto temperature = simulation.temperatureVolume8();
  const auto vapor = simulation.vaporVolume8();
  const auto occupancy = simulation.occupancyVolume8(4);
  const auto stats = simulation.densityStats();

  std::size_t densest_voxel = 0;
  std::uint16_t densest_value = 0;
  for (std::size_t i = 0; i < density.bytes.size() / 2U; ++i) {
    const auto value =
        static_cast<std::uint16_t>(density.bytes[i * 2U]) |
        static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(density.bytes[i * 2U + 1U]) << 8U);
    if (value > densest_value) {
      densest_value = value;
      densest_voxel = i;
    }
  }
  const int dense_x = static_cast<int>(densest_voxel % 24U);
  const int dense_y = static_cast<int>((densest_voxel / 24U) % 24U);
  const int dense_z = static_cast<int>(densest_voxel / (24U * 24U));
  cloud::CloudInteractor test_interactor;
  test_interactor.id = 91;
  test_interactor.shape = cloud::InteractorShape::Ellipsoid;
  test_interactor.position = {
      (static_cast<float>(dense_x) + 0.5f) / 24.0f,
      (static_cast<float>(dense_y) + 0.5f) / 24.0f,
      (static_cast<float>(dense_z) + 0.5f) / 24.0f,
  };
  test_interactor.previous_position = test_interactor.position;
  test_interactor.previous_position.x -= 0.35f;
  test_interactor.has_previous_transform = true;
  test_interactor.transform_interval_seconds = 0.05f;
  test_interactor.half_extents = {0.10f, 0.05f, 0.04f};
  test_interactor.linear_velocity = {7.0f, 0.0f, 0.0f};

  cloud::CloudSimulation mass_simulation = simulation;
  cloud::CloudInteractor mass_interactor = test_interactor;
  mass_interactor.previous_position = mass_interactor.position;
  mass_interactor.has_previous_transform = false;
  mass_interactor.transform_interval_seconds = 0.0f;
  mass_interactor.linear_velocity = {};
  mass_simulation.setInteractors({mass_interactor});
  mass_simulation.step(0.05f);
  const auto mass_interaction = mass_simulation.interactionStats();

  simulation.setInteractors({test_interactor});
  simulation.step(0.05f);
  const auto interaction = simulation.interactionStats();

  cloud::CloudSimulation overlap_simulation(24, 2);
  cloud::CloudInteractor owner_a;
  owner_a.id = 1;
  owner_a.shape = cloud::InteractorShape::Sphere;
  owner_a.flags = cloud::kInteractorSolid;
  owner_a.position = {0.5f, 0.5f, 0.5f};
  owner_a.half_extents = {0.07f, 0.07f, 0.07f};
  owner_a.linear_velocity = {1.0f, 0.0f, 0.0f};
  cloud::CloudInteractor owner_b = owner_a;
  owner_b.id = 2;
  owner_b.linear_velocity = {-1.0f, 0.0f, 0.0f};
  overlap_simulation.setInteractors({owner_a, owner_b});
  const auto center_velocity_x = [](const cloud::QuantizedVolume &volume) {
    constexpr std::size_t center_index = (12U * 24U + 12U) * 24U + 12U;
    const std::size_t offset = center_index * 3U * 2U;
    const auto bits =
        static_cast<std::uint16_t>(volume.bytes[offset]) |
        static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(volume.bytes[offset + 1U]) << 8U);
    return static_cast<std::int16_t>(bits);
  };
  overlap_simulation.step(0.05f);
  const std::int16_t first_owner_velocity =
      center_velocity_x(overlap_simulation.velocityVolumeSnorm16());
  overlap_simulation.step(0.05f);
  const std::int16_t second_owner_velocity =
      center_velocity_x(overlap_simulation.velocityVolumeSnorm16());
  const bool overlap_owner_ok =
      first_owner_velocity > 0 && second_owner_velocity > 0;

  const std::size_t voxel_count = 24U * 24U * 24U;
  const bool fields_ok = legacy_density.size() == voxel_count &&
                         density.bytes.size() == voxel_count * 2U &&
                         velocity.bytes.size() == voxel_count * 3U * 2U &&
                         temperature.bytes.size() == voxel_count &&
                         vapor.bytes.size() == voxel_count &&
                         occupancy.bytes.size() == 6U * 6U * 6U;

  const std::vector<std::uint8_t> codec_input = {0, 0, 0, 0, 1, 2, 3, 4, 4, 4,
                                                 5, 6, 7, 8, 9, 9, 9, 9, 9};
  const auto compressed = cloud::rleCompress(codec_input);
  std::vector<std::uint8_t> decoded;
  const bool codec_ok =
      cloud::rleDecompress(compressed, codec_input.size(), decoded) &&
      decoded == codec_input;

  const std::vector<std::uint8_t> crc_reference = {'1', '2', '3', '4', '5',
                                                   '6', '7', '8', '9'};
  const bool crc_ok = cloud::crc32(crc_reference) == 0xcbf43926U;

  cloud::InteractorCommand control_command;
  control_command.session_id = 0x12345678U;
  control_command.sequence = 0xfedcba98U;
  control_command.interactor_id = 0x0000000100000002ULL;
  control_command.client_time = 1234567890.125;
  control_command.interactor.shape = cloud::InteractorShape::Ellipsoid;
  control_command.interactor.flags = cloud::kInteractorSolid |
                                     cloud::kInteractorDisplaceScalars |
                                     cloud::kInteractorGenerateWake;
  control_command.interactor.position = {0.25f, 0.5f, 0.75f};
  control_command.interactor.rotation = {0.0f, 0.0f, 0.0f, 1.0f};
  control_command.interactor.half_extents = {0.06f, 0.025f, 0.015f};
  control_command.interactor.linear_velocity = {0.3f, -0.1f, 0.02f};
  control_command.interactor.angular_velocity = {0.0f, 0.0f, 1.5f};
  control_command.ttl_ms = 500;
  control_command.strength = 1.25f;
  const auto serialized_control =
      cloud::serializeInteractorCommand(control_command);
  cloud::InteractorCommand parsed_control;
  const bool control_round_trip = cloud::deserializeInteractorCommand(
      serialized_control.data(), serialized_control.size(), parsed_control);
  auto damaged_control = serialized_control;
  damaged_control[80] ^= 0x01U;
  cloud::InteractorCommand ignored_control;
  cloud::InteractorCommand remove_command;
  remove_command.session_id = control_command.session_id;
  remove_command.sequence = control_command.sequence + 1U;
  remove_command.interactor_id = control_command.interactor_id;
  remove_command.client_time = control_command.client_time + 0.1;
  remove_command.action = cloud::ControlAction::Remove;
  const auto serialized_remove =
      cloud::serializeInteractorCommand(remove_command);
  const bool canonical_remove = std::all_of(
      serialized_remove.begin() + 33, serialized_remove.begin() + 108,
      [](std::uint8_t value) { return value == 0U; });
  const bool control_ok =
      serialized_control.size() == cloud::kControlPacketBytes &&
      control_round_trip &&
      parsed_control.session_id == control_command.session_id &&
      parsed_control.sequence == control_command.sequence &&
      parsed_control.interactor_id == control_command.interactor_id &&
      parsed_control.client_time == control_command.client_time &&
      parsed_control.interactor.position.x ==
          control_command.interactor.position.x &&
      parsed_control.strength == control_command.strength &&
      cloud::detail::readU32(serialized_control.data(),
                             cloud::kControlCrcOffset) == 0x864817afU &&
      canonical_remove &&
      !cloud::deserializeInteractorCommand(
          damaged_control.data(), damaged_control.size(), ignored_control);

  cloud::PacketHeaderV2 header;
  header.frame_id = 42;
  header.field_crc32 = 0x12345678U;
  header.simulation_time = 12.5;
  header.grid_x = 24;
  header.grid_y = 24;
  header.grid_z = 24;
  header.voxel_format = cloud::VoxelFormat::UNorm16;
  header.field_id = cloud::FieldId::Density;
  header.channel_count = 1;
  header.compression = cloud::Compression::Rle;
  header.chunk_index = 3;
  header.chunk_count = 10;
  header.payload_bytes = 1200;
  header.field_mask = kAllV2Fields;
  header.flags = cloud::kFlagKeyframe;
  header.payload_offset = 3600;
  header.encoded_field_bytes = 10000;
  header.decoded_field_bytes = 27648;
  header.value_scale = 1.25f;
  header.value_bias = -0.5f;
  const auto serialized_header = cloud::serializeHeaderV2(header);
  cloud::PacketHeaderV2 parsed_header;
  const bool header_ok =
      cloud::deserializeHeaderV2(serialized_header.data(),
                                 serialized_header.size(), parsed_header) &&
      parsed_header.frame_id == header.frame_id &&
      parsed_header.field_crc32 == header.field_crc32 &&
      parsed_header.simulation_time == header.simulation_time &&
      parsed_header.payload_offset == header.payload_offset &&
      parsed_header.decoded_field_bytes == header.decoded_field_bytes &&
      parsed_header.value_scale == header.value_scale &&
      parsed_header.value_bias == header.value_bias;

  cloud::SkyEnvironmentConfig sky_config;
  sky_config.utc_unix_seconds = 1710936000.0; // 2024 equinox, 12:00 UTC
  sky_config.time_scale = 60.0f;
  sky_config.location = {0.0, 0.0, 0.0};
  sky_config.weather_preset = cloud::WeatherPreset::Clear;
  sky_config.weather_seed = 77U;
  cloud::SkyEnvironment sky_environment(sky_config);
  const auto initial_sky = sky_environment.snapshot();
  sky_environment.step(1.0);
  const auto advanced_sky = sky_environment.snapshot();
  const bool astronomy_ok =
      initial_sky.celestial.sun_geometric_elevation_degrees > 88.0f &&
      initial_sky.celestial.sun_geometric_elevation_degrees <= 90.0f &&
      std::abs(cloud::sky_detail::vectorLength(
                   initial_sky.celestial.sun_direction) -
               1.0f) < 0.001f &&
      std::abs(cloud::sky_detail::vectorLength(
                   initial_sky.celestial.moon_direction) -
      1.0f) < 0.001f &&
      advanced_sky.utc_unix_seconds == initial_sky.utc_unix_seconds + 60.0;
  sky_environment.setTimeScale(0.0f);
  const double paused_utc = sky_environment.snapshot().utc_unix_seconds;
  sky_environment.step(1.0);
  const bool clock_pause_ok =
      sky_environment.snapshot().utc_unix_seconds == paused_utc;
  sky_environment.setTimeScale(3600.0f);
  const double spin_up_utc = sky_environment.snapshot().utc_unix_seconds;
  const double spin_up_fluid =
      sky_environment.snapshot().fluid_time_seconds;
  sky_environment.spinUp(0.5);
  const bool spin_up_clock_ok =
      sky_environment.snapshot().utc_unix_seconds == spin_up_utc &&
      std::abs(sky_environment.snapshot().fluid_time_seconds -
                   (spin_up_fluid + 0.5)) <
          1.0e-9;
  cloud::SkyEnvironment wall_clock_environment(sky_config);
  const auto wall_clock_initial = wall_clock_environment.snapshot();
  wall_clock_environment.step(0.10, 0.50);
  const bool wall_clock_ok =
      std::abs(wall_clock_environment.snapshot().fluid_time_seconds -
                   (wall_clock_initial.fluid_time_seconds + 0.10)) <
          1.0e-9 &&
      wall_clock_environment.snapshot().utc_unix_seconds ==
          wall_clock_initial.utc_unix_seconds + 30.0;
  cloud::SkyEnvironmentConfig boundary_clock_config = sky_config;
  boundary_clock_config.utc_unix_seconds = 4133980790.0;
  boundary_clock_config.time_scale = 60.0f;
  cloud::SkyEnvironment boundary_clock_environment(boundary_clock_config);
  boundary_clock_environment.step(0.10, 1.0);
  const bool boundary_clock_ok =
      boundary_clock_environment.snapshot().utc_unix_seconds ==
          4133980800.0 &&
      boundary_clock_environment.snapshot().time_scale == 0.0f;

  cloud::SkyEnvironmentConfig sea_level_atmosphere_config = sky_config;
  sea_level_atmosphere_config.time_scale = 0.0f;
  sea_level_atmosphere_config.location.elevation_m = 0.0;
  cloud::SkyEnvironment sea_level_atmosphere(sea_level_atmosphere_config);
  cloud::SkyEnvironmentConfig high_altitude_atmosphere_config =
      sea_level_atmosphere_config;
  high_altitude_atmosphere_config.location.elevation_m = 10000.0;
  cloud::SkyEnvironment high_altitude_atmosphere(
      high_altitude_atmosphere_config);
  const cloud::EnvironmentSnapshot &sea_level_snapshot =
      sea_level_atmosphere.snapshot();
  const cloud::EnvironmentSnapshot &high_altitude_snapshot =
      high_altitude_atmosphere.snapshot();
  const bool altitude_atmosphere_ok =
      high_altitude_snapshot.celestial.sun_irradiance_w_m2 >
          sea_level_snapshot.celestial.sun_irradiance_w_m2 * 1.05f &&
      high_altitude_snapshot.optics.rayleigh_scattering_per_m ==
          sea_level_snapshot.optics.rayleigh_scattering_per_m;

  cloud::SkyEnvironmentConfig catch_up_config = sky_config;
  catch_up_config.time_scale = 1.0f;
  catch_up_config.weather_preset = cloud::WeatherPreset::Clear;
  catch_up_config.weather_seed = 991U;
  cloud::SkyEnvironment catch_up_single(catch_up_config);
  cloud::SkyEnvironment catch_up_substeps(catch_up_config);
  catch_up_single.setWeatherPreset(cloud::WeatherPreset::Storm, 10.0);
  catch_up_substeps.setWeatherPreset(cloud::WeatherPreset::Storm, 10.0);
  catch_up_single.step(0.5, 5.0);
  for (int step = 0; step < 5; ++step) {
    catch_up_substeps.step(0.1, 1.0);
  }
  const bool catch_up_ok =
      catch_up_single.snapshot().utc_unix_seconds ==
          catch_up_substeps.snapshot().utc_unix_seconds &&
      std::abs(catch_up_single.snapshot().weather.surface_wetness -
               catch_up_substeps.snapshot().weather.surface_wetness) <
          1.0e-6f &&
      std::abs(catch_up_single.snapshot().weather.transition_progress -
               catch_up_substeps.snapshot().weather.transition_progress) <
          1.0e-6f &&
      catch_up_single.snapshot().lightning_event_id ==
          catch_up_substeps.snapshot().lightning_event_id;
  cloud::SkyEnvironmentConfig long_gap_config = catch_up_config;
  long_gap_config.time_scale = 0.0f;
  long_gap_config.weather_preset = cloud::WeatherPreset::Storm;
  cloud::SkyEnvironment long_gap_environment(long_gap_config);
  long_gap_environment.step(0.1, 86400.0);
  const bool long_gap_ok =
      long_gap_environment.snapshot().lightning_event_id > 0U &&
      long_gap_environment.snapshot().weather.lightning_flash == 0.0f;

  cloud::WeatherDirector transition_weather(cloud::WeatherPreset::Cumulus,
                                             123U);
  transition_weather.initialize(1710936000.0, {37.5, 127.0, 50.0});
  const float fair_temperature =
      transition_weather.profile().weather.surface_temperature_kelvin;
  transition_weather.setPreset(cloud::WeatherPreset::Storm, 10.0);
  transition_weather.advance(5.0, 1710936005.0, {37.5, 127.0, 50.0});
  const float midpoint_temperature =
      transition_weather.profile().weather.surface_temperature_kelvin;
  const bool transition_midpoint_ok =
      transition_weather.profile().weather.transition_progress > 0.49f &&
      transition_weather.profile().weather.transition_progress < 0.51f &&
      midpoint_temperature > fair_temperature - 2.0f &&
      midpoint_temperature < 294.0f;
  transition_weather.advance(5.0, 1710936010.0, {37.5, 127.0, 50.0});
  cloud::WeatherDirector natural_a(cloud::WeatherPreset::Natural, 456U);
  cloud::WeatherDirector natural_b(cloud::WeatherPreset::Natural, 456U);
  natural_a.initialize(1756123200.0, {37.5, 127.0, 50.0});
  natural_b.initialize(1756123200.0, {37.5, 127.0, 50.0});
  bool natural_fog_reachable = false;
  std::uint32_t natural_fog_seed = 0U;
  double natural_fog_time = 0.0;
  for (std::uint32_t seed = 1U; seed <= 32U && !natural_fog_reachable;
       ++seed) {
    for (int sample = 0; sample < 30 * 8; ++sample) {
      const cloud::WeatherProfile natural_sample =
          cloud::weather_detail::naturalProfile(
              1735689600.0 + static_cast<double>(sample) * 10800.0,
              {37.5, 127.0, 50.0}, seed);
      natural_fog_reachable =
          natural_sample.cloud_layer_count == 1U &&
          natural_sample.cloud_layers[0].kind ==
              cloud::CloudLayerKind::Fog &&
          natural_sample.weather.relative_humidity >= 0.94f &&
          natural_sample.weather.visibility_m <= 1200.0f &&
          natural_sample.weather.fog_extinction_per_m > 0.0f;
      if (natural_fog_reachable) {
        natural_fog_seed = seed;
        natural_fog_time =
            1735689600.0 + static_cast<double>(sample) * 10800.0;
        break;
      }
    }
  }
  bool natural_threshold_smoothing_ok = false;
  if (natural_fog_reachable) {
    for (int hour_back = 1; hour_back <= 48; ++hour_back) {
      const double clear_time =
          natural_fog_time - static_cast<double>(hour_back) * 3600.0;
      const cloud::WeatherProfile earlier =
          cloud::weather_detail::naturalProfile(
              clear_time, {37.5, 127.0, 50.0}, natural_fog_seed);
      const bool earlier_is_fog =
          earlier.cloud_layer_count == 1U &&
          earlier.cloud_layers[0].kind == cloud::CloudLayerKind::Fog;
      if (earlier_is_fog) {
        continue;
      }
      cloud::WeatherDirector smoothed_natural(
          cloud::WeatherPreset::Natural, natural_fog_seed);
      smoothed_natural.initialize(clear_time, {37.5, 127.0, 50.0});
      smoothed_natural.advance(0.05, natural_fog_time,
                               {37.5, 127.0, 50.0});
      const cloud::WeatherProfile &smoothed = smoothed_natural.profile();
      for (std::size_t index = 0; index < smoothed.cloud_layer_count;
           ++index) {
        const cloud::CloudLayerState &layer = smoothed.cloud_layers[index];
        if (layer.kind == cloud::CloudLayerKind::Fog) {
          natural_threshold_smoothing_ok =
              smoothed.weather.preset == cloud::WeatherPreset::Natural &&
              layer.coverage > 0.0f && layer.coverage < 0.01f &&
              layer.optical_depth > 0.0f && layer.optical_depth < 0.1f;
          break;
        }
      }
      break;
    }
  }
  const cloud::GeoLocation immediate_natural_location{37.5, 127.0, 50.0};
  constexpr double immediate_natural_time = 1756123200.0;
  cloud::WeatherDirector immediate_natural(
      cloud::WeatherPreset::Storm, 789U);
  immediate_natural.initialize(immediate_natural_time,
                               immediate_natural_location);
  immediate_natural.releaseToNatural(0.0);
  immediate_natural.advance(0.05, immediate_natural_time,
                            immediate_natural_location);
  const cloud::WeatherProfile immediate_natural_target =
      cloud::weather_detail::naturalProfile(
          immediate_natural_time, immediate_natural_location, 789U);
  const bool immediate_natural_release_ok =
      immediate_natural.profile().weather.preset ==
          cloud::WeatherPreset::Natural &&
      immediate_natural.profile().weather.surface_temperature_kelvin ==
          immediate_natural_target.weather.surface_temperature_kelvin &&
      immediate_natural.profile().cloud_layer_count ==
          immediate_natural_target.cloud_layer_count &&
      (immediate_natural_target.cloud_layer_count == 0U ||
       immediate_natural.profile().cloud_layers[0].kind ==
           immediate_natural_target.cloud_layers[0].kind);
  constexpr double relaxation_source_time = 1735689600.0;
  constexpr double relaxation_target_time =
      relaxation_source_time + 180.0 * 86400.0;
  const cloud::GeoLocation relaxation_location{37.5, 127.0, 50.0};
  cloud::WeatherDirector relaxation_4hz(cloud::WeatherPreset::Natural,
                                        2468U);
  cloud::WeatherDirector relaxation_20hz(cloud::WeatherPreset::Natural,
                                         2468U);
  relaxation_4hz.initialize(relaxation_source_time, relaxation_location);
  relaxation_20hz.initialize(relaxation_source_time, relaxation_location);
  const float relaxation_initial_temperature =
      relaxation_4hz.profile().weather.surface_temperature_kelvin;
  const float relaxation_target_temperature =
      cloud::weather_detail::naturalProfile(
          relaxation_target_time, relaxation_location, 2468U)
          .weather.surface_temperature_kelvin;
  for (int step = 0; step < 120 * 4; ++step) {
    relaxation_4hz.advance(0.25, relaxation_target_time,
                           relaxation_location);
  }
  for (int step = 0; step < 120 * 20; ++step) {
    relaxation_20hz.advance(0.05, relaxation_target_time,
                            relaxation_location);
  }
  const float initial_relaxation_error =
      std::abs(relaxation_initial_temperature -
               relaxation_target_temperature);
  const float relaxation_denominator =
      std::max(initial_relaxation_error, 1.0e-6f);
  const float relaxation_ratio_4hz =
      std::abs(relaxation_4hz.profile().weather.surface_temperature_kelvin -
               relaxation_target_temperature) /
      relaxation_denominator;
  const float relaxation_ratio_20hz =
      std::abs(relaxation_20hz.profile().weather.surface_temperature_kelvin -
               relaxation_target_temperature) /
      relaxation_denominator;
  const bool natural_relaxation_ok =
      initial_relaxation_error > 1.0f && relaxation_ratio_4hz > 0.35f &&
      relaxation_ratio_4hz < 0.39f && relaxation_ratio_20hz > 0.35f &&
      relaxation_ratio_20hz < 0.39f &&
      std::abs(relaxation_ratio_4hz - relaxation_ratio_20hz) < 0.001f;
  const cloud::GeoLocation accelerated_transition_location{37.5665, 126.978,
                                                           38.0};
  constexpr double accelerated_transition_start = 1736672400.0;
  cloud::WeatherDirector accelerated_transition(
      cloud::WeatherPreset::Storm, 1U);
  accelerated_transition.initialize(accelerated_transition_start,
                                    accelerated_transition_location);
  accelerated_transition.setPreset(cloud::WeatherPreset::Natural, 10.0);
  float previous_fog_coverage = 0.0f;
  float maximum_fog_coverage_step = 0.0f;
  for (int step = 1; step <= 200; ++step) {
    accelerated_transition.advance(
        0.05, accelerated_transition_start + 180.0 * step,
        accelerated_transition_location);
    float fog_coverage = 0.0f;
    const cloud::WeatherProfile &profile = accelerated_transition.profile();
    for (std::size_t index = 0; index < profile.cloud_layer_count; ++index) {
      if (profile.cloud_layers[index].kind == cloud::CloudLayerKind::Fog) {
        fog_coverage += profile.cloud_layers[index].coverage;
      }
    }
    maximum_fog_coverage_step =
        std::max(maximum_fog_coverage_step,
                 std::abs(fog_coverage - previous_fog_coverage));
    previous_fog_coverage = fog_coverage;
  }
  const bool accelerated_transition_ok =
      maximum_fog_coverage_step < 0.02f &&
      accelerated_transition.profile().weather.preset ==
          cloud::WeatherPreset::Natural;
  const bool weather_ok =
      transition_midpoint_ok &&
      transition_weather.profile().weather.preset ==
          cloud::WeatherPreset::Storm &&
      natural_a.profile().weather.surface_temperature_kelvin ==
          natural_b.profile().weather.surface_temperature_kelvin &&
      natural_a.profile().weather.wind_m_s.east ==
          natural_b.profile().weather.wind_m_s.east &&
      natural_fog_reachable && natural_threshold_smoothing_ok &&
      immediate_natural_release_ok && natural_relaxation_ok &&
      accelerated_transition_ok;

  cloud::SkyStatePacketContext sky_context;
  sky_context.volume_frame_id = 42U;
  sky_context.weather_seed = 77U;
  sky_context.evolution_mode = cloud::SkyEvolutionMode::Manual;
  const auto sky_packet =
      cloud::serializeSkyState(advanced_sky, sky_context);
  cloud::EnvironmentSnapshot parsed_sky;
  cloud::SkyStatePacketContext parsed_sky_context;
  const bool sky_state_round_trip = cloud::deserializeSkyState(
      sky_packet.data(), sky_packet.size(), parsed_sky,
      parsed_sky_context);
  auto damaged_sky_packet = sky_packet;
  damaged_sky_packet[300] ^= 0x01U;
  auto invalid_moon_fraction_packet = sky_packet;
  cloud::detail::writeF32(invalid_moon_fraction_packet.data(), 392U,
                          -100.0f);
  cloud::detail::writeU32(
      invalid_moon_fraction_packet.data(), cloud::kSkyStateCrcOffset,
      cloud::crc32(invalid_moon_fraction_packet.data(),
                   cloud::kSkyStateCrcOffset));
  auto invalid_aurora_packet = sky_packet;
  cloud::detail::writeF32(invalid_aurora_packet.data(), 424U, -1.0f);
  cloud::detail::writeU32(
      invalid_aurora_packet.data(), cloud::kSkyStateCrcOffset,
      cloud::crc32(invalid_aurora_packet.data(),
                   cloud::kSkyStateCrcOffset));
  auto boundary_quaternion_packet = sky_packet;
  cloud::detail::writeF32(boundary_quaternion_packet.data(), 400U,
                          0.5497187376f);
  cloud::detail::writeF32(boundary_quaternion_packet.data(), 404U,
                          0.6808882356f);
  cloud::detail::writeF32(boundary_quaternion_packet.data(), 408U,
                          -0.2575999200f);
  cloud::detail::writeF32(boundary_quaternion_packet.data(), 412U,
                          0.4335238039f);
  cloud::detail::writeU32(
      boundary_quaternion_packet.data(), cloud::kSkyStateCrcOffset,
      cloud::crc32(boundary_quaternion_packet.data(),
                   cloud::kSkyStateCrcOffset));
  auto boundary_phase_packet = sky_packet;
  cloud::detail::writeF32(boundary_phase_packet.data(), 160U,
                          1.0f / 3600.0f);
  cloud::detail::writeF32(boundary_phase_packet.data(), 164U,
                          0.414993017911911f);
  cloud::detail::writeF32(boundary_phase_packet.data(), 168U,
                          0.49542924761772156f);
  cloud::detail::writeF32(boundary_phase_packet.data(), 172U,
                          0.08967772871255875f);
  cloud::detail::writeU32(
      boundary_phase_packet.data(), cloud::kSkyStateCrcOffset,
      cloud::crc32(boundary_phase_packet.data(),
                   cloud::kSkyStateCrcOffset));
  cloud::EnvironmentSnapshot ignored_sky;
  cloud::SkyStatePacketContext ignored_sky_context;
  const bool sky_state_ok =
      sky_state_round_trip &&
      parsed_sky.sequence == advanced_sky.sequence &&
      parsed_sky.utc_unix_seconds == advanced_sky.utc_unix_seconds &&
      parsed_sky_context.volume_frame_id == sky_context.volume_frame_id &&
      !cloud::deserializeSkyState(damaged_sky_packet.data(),
                                  damaged_sky_packet.size(), ignored_sky,
                                  ignored_sky_context) &&
      !cloud::deserializeSkyState(invalid_moon_fraction_packet.data(),
                                  invalid_moon_fraction_packet.size(),
                                  ignored_sky, ignored_sky_context) &&
      !cloud::deserializeSkyState(invalid_aurora_packet.data(),
                                  invalid_aurora_packet.size(), ignored_sky,
                                  ignored_sky_context) &&
      cloud::deserializeSkyState(boundary_quaternion_packet.data(),
                                 boundary_quaternion_packet.size(),
                                 ignored_sky, ignored_sky_context) &&
      cloud::deserializeSkyState(boundary_phase_packet.data(),
                                 boundary_phase_packet.size(), ignored_sky,
                                 ignored_sky_context);

  cloud::SkyControlCommand sky_command;
  sky_command.session_id = 0x87654321U;
  sky_command.sequence = 17U;
  sky_command.client_time_seconds = 1234.5;
  sky_command.opcode = cloud::SkyControlOpcode::LoadPreset;
  sky_command.evolution_mode = cloud::SkyEvolutionMode::Manual;
  sky_command.transition_milliseconds = 2500U;
  sky_command.preset = cloud::WeatherPreset::Storm;
  sky_command.weather_seed = 99U;
  const auto serialized_sky_control =
      cloud::serializeSkyControl(sky_command);
  cloud::SkyControlCommand parsed_sky_control;
  const bool sky_control_round_trip = cloud::deserializeSkyControl(
      serialized_sky_control.data(), serialized_sky_control.size(),
      parsed_sky_control);
  auto damaged_sky_control = serialized_sky_control;
  damaged_sky_control[80] ^= 0x01U;
  auto aliased_sky_control = serialized_sky_control;
  cloud::detail::writeU32(aliased_sky_control.data(), 36, 256U);
  cloud::detail::writeU32(
      aliased_sky_control.data(), cloud::kSkyControlCrcOffset,
      cloud::crc32(aliased_sky_control.data(), cloud::kSkyControlCrcOffset));
  cloud::SkyControlCommand layer_validation_command;
  layer_validation_command.session_id = 0x87654321U;
  layer_validation_command.sequence = 18U;
  layer_validation_command.client_time_seconds = 1235.0;
  layer_validation_command.opcode = cloud::SkyControlOpcode::PatchOverride;
  layer_validation_command.evolution_mode = cloud::SkyEvolutionMode::Manual;
  layer_validation_command.apply_mask = cloud::kSkyApplyCloudLayer0;
  cloud::CloudLayerState &validation_layer =
      layer_validation_command.values.cloud_layers[0];
  validation_layer.kind = cloud::CloudLayerKind::Convective;
  validation_layer.base_altitude_m = 1000.0f;
  validation_layer.top_altitude_m = 2200.0f;
  validation_layer.coverage = 0.65f;
  validation_layer.optical_depth = 30.0f;
  validation_layer.liquid_fraction = 0.85f;
  validation_layer.precipitation_rate_mm_h = 2.0f;
  validation_layer.convective_activity = 0.55f;
  auto non_finite_layer_control =
      cloud::serializeSkyControl(layer_validation_command);
  cloud::detail::writeF32(non_finite_layer_control.data(), 248U,
                          std::numeric_limits<float>::infinity());
  cloud::detail::writeU32(
      non_finite_layer_control.data(), cloud::kSkyControlCrcOffset,
      cloud::crc32(non_finite_layer_control.data(),
                   cloud::kSkyControlCrcOffset));
  auto out_of_range_layer_control =
      cloud::serializeSkyControl(layer_validation_command);
  cloud::detail::writeF32(out_of_range_layer_control.data(), 248U, 100.0f);
  cloud::detail::writeU32(
      out_of_range_layer_control.data(), cloud::kSkyControlCrcOffset,
      cloud::crc32(out_of_range_layer_control.data(),
                   cloud::kSkyControlCrcOffset));
  cloud::SkyControlCommand optical_boundary_command =
      layer_validation_command;
  optical_boundary_command.sequence = 19U;
  optical_boundary_command.values.cloud_layers[0] = {
      cloud::CloudLayerKind::Stratiform, 32479.4f, 33787.0f, 0.8f, 500.0f,
      1.0f, 300.0f, 1.0f};
  const auto optical_boundary_control =
      cloud::serializeSkyControl(optical_boundary_command);
  cloud::SkyControlCommand parsed_optical_boundary;
  cloud::SkyControlCommand gust_boundary_command;
  gust_boundary_command.session_id = 0x87654321U;
  gust_boundary_command.sequence = 20U;
  gust_boundary_command.client_time_seconds = 1236.0;
  gust_boundary_command.opcode = cloud::SkyControlOpcode::PatchOverride;
  gust_boundary_command.evolution_mode = cloud::SkyEvolutionMode::Manual;
  gust_boundary_command.apply_mask = cloud::kSkyApplyWind;
  gust_boundary_command.values.weather.wind_m_s = {
      0.0411864f, 0.122675f, 0.585879f};
  gust_boundary_command.values.weather.gust_speed_m_s = 200.0f;
  const auto gust_boundary_control =
      cloud::serializeSkyControl(gust_boundary_command);
  cloud::SkyControlCommand parsed_gust_boundary;
  cloud::SkyControlCommand ignored_sky_control;
  const bool optical_boundary_round_trip =
      cloud::deserializeSkyControl(optical_boundary_control.data(),
                                   optical_boundary_control.size(),
                                   parsed_optical_boundary) &&
      parsed_optical_boundary.values.cloud_layers[0].optical_depth ==
          500.0f;
  const bool gust_boundary_round_trip =
      cloud::deserializeSkyControl(gust_boundary_control.data(),
                                   gust_boundary_control.size(),
                                   parsed_gust_boundary) &&
      std::abs(parsed_gust_boundary.values.weather.gust_speed_m_s - 200.0f) <
          0.001f;
  const bool sky_control_ok =
      sky_control_round_trip &&
      parsed_sky_control.session_id == sky_command.session_id &&
      parsed_sky_control.sequence == sky_command.sequence &&
      parsed_sky_control.preset == sky_command.preset &&
      cloud::detail::readU32(serialized_sky_control.data(),
                             cloud::kSkyControlCrcOffset) == 0xb98b38c2U &&
      !cloud::deserializeSkyControl(damaged_sky_control.data(),
                                    damaged_sky_control.size(),
                                    ignored_sky_control) &&
      !cloud::deserializeSkyControl(aliased_sky_control.data(),
                                    aliased_sky_control.size(),
                                    ignored_sky_control) &&
      !cloud::deserializeSkyControl(non_finite_layer_control.data(),
                                    non_finite_layer_control.size(),
                                    ignored_sky_control) &&
      !cloud::deserializeSkyControl(out_of_range_layer_control.data(),
                                    out_of_range_layer_control.size(),
                                    ignored_sky_control) &&
      optical_boundary_round_trip && gust_boundary_round_trip;

  cloud::SkyControlCommand patch_sky_command;
  patch_sky_command.session_id = 0x87654321U;
  patch_sky_command.sequence = 18U;
  patch_sky_command.client_time_seconds = 1235.0;
  patch_sky_command.opcode = cloud::SkyControlOpcode::PatchOverride;
  patch_sky_command.evolution_mode = cloud::SkyEvolutionMode::Manual;
  patch_sky_command.transition_milliseconds = 1200U;
  patch_sky_command.apply_mask =
      cloud::kSkyApplyThermodynamics | cloud::kSkyApplyWind |
      cloud::kSkyApplyPrecipitation | cloud::kSkyApplyConvection;
  patch_sky_command.values.weather.surface_temperature_kelvin = 285.15f;
  patch_sky_command.values.weather.sea_level_pressure_pa = 99500.0f;
  patch_sky_command.values.weather.relative_humidity = 0.93f;
  patch_sky_command.values.weather.wind_m_s = {12.0f, -3.0f, 0.2f};
  patch_sky_command.values.weather.gust_speed_m_s = 20.0f;
  patch_sky_command.values.weather.precipitation_rate_mm_h = 14.0f;
  patch_sky_command.values.weather.snow_fraction = 0.25f;
  patch_sky_command.values.weather.surface_wetness = 0.75f;
  patch_sky_command.values.weather.convective_activity = 0.82f;
  patch_sky_command.values.weather.lightning_activity = 0.66f;
  const auto serialized_patch =
      cloud::serializeSkyControl(patch_sky_command);
  cloud::SkyControlCommand parsed_patch;
  const bool sky_patch_ok = cloud::deserializeSkyControl(
                                serialized_patch.data(),
                                serialized_patch.size(), parsed_patch) &&
                            parsed_patch.apply_mask ==
                                patch_sky_command.apply_mask &&
                            std::abs(parsed_patch.values.weather
                                         .precipitation_rate_mm_h -
                                     14.0f) < 0.001f &&
                            std::abs(parsed_patch.values.weather.gust_speed_m_s -
                                     20.0f) < 0.001f;

  cloud::CloudSimulation forced_simulation(24, 4);
  forced_simulation.setEnvironmentalForcing(
      sky_environment.cloudForcing(forced_simulation.size()));
  for (int step = 0; step < 40; ++step) {
    forced_simulation.step(0.05f);
  }
  const bool forcing_ok = forced_simulation.allFinite() &&
                          forced_simulation.environmentalForcing().enabled;

  auto make_layer_forcing = [](float base, float top) {
    cloud::CloudForcing forcing;
    forcing.enabled = true;
    forcing.cloud_layer_count = 1;
    forcing.cloud_layers[0] = {
        cloud::CloudForcingLayerKind::Convective, base, top, 0.85f, 40.0f,
        0.80f};
    forcing.surface_temperature_target = 0.20f;
    forcing.top_temperature_target = -0.20f;
    forcing.temperature_target_response_per_second = 0.02f;
    forcing.surface_vapor_target = 0.16f;
    forcing.top_vapor_target = 0.06f;
    forcing.vapor_target_response_per_second = 0.03f;
    return forcing;
  };
  cloud::CloudSimulation low_layer_simulation(24, 4);
  cloud::CloudSimulation high_layer_simulation(24, 4);
  low_layer_simulation.setEnvironmentalForcing(
      make_layer_forcing(0.10f, 0.30f));
  high_layer_simulation.setEnvironmentalForcing(
      make_layer_forcing(0.70f, 0.90f));
  for (int step = 0; step < 120; ++step) {
    low_layer_simulation.step(0.05f);
    high_layer_simulation.step(0.05f);
  }
  const float low_cloud_height =
      low_layer_simulation.densityCenterOfMassNormalizedHeight();
  const float high_cloud_height =
      high_layer_simulation.densityCenterOfMassNormalizedHeight();

  const cloud::CloudForcing active_layer_forcing =
      make_layer_forcing(0.20f, 0.50f);
  cloud::CloudForcing ghost_layer_forcing = active_layer_forcing;
  ghost_layer_forcing.cloud_layers[1] = {
      cloud::CloudForcingLayerKind::Cirrus, 0.70f, 0.90f, 0.0f, 0.0f,
      0.0f};
  ghost_layer_forcing.cloud_layer_count = 2U;
  cloud::CloudSimulation active_layer_only_simulation(24, 4);
  cloud::CloudSimulation ghost_layer_simulation(24, 4);
  active_layer_only_simulation.setEnvironmentalForcing(active_layer_forcing);
  ghost_layer_simulation.setEnvironmentalForcing(ghost_layer_forcing);
  for (int step = 0; step < 80; ++step) {
    active_layer_only_simulation.step(0.05f);
    ghost_layer_simulation.step(0.05f);
  }
  const cloud::SimulationStats active_layer_stats =
      active_layer_only_simulation.densityStats();
  const cloud::SimulationStats ghost_layer_stats =
      ghost_layer_simulation.densityStats();
  const bool zero_coverage_layer_is_inert =
      std::abs(active_layer_stats.maximum - ghost_layer_stats.maximum) <
          1.0e-7f &&
      std::abs(active_layer_stats.mean - ghost_layer_stats.mean) < 1.0e-7f;

  cloud::SkyEnvironmentConfig outside_config = sky_config;
  outside_config.time_scale = 0.0f;
  outside_config.weather_preset = cloud::WeatherPreset::Clear;
  cloud::SkyEnvironment outside_environment(outside_config);
  cloud::WeatherProfile outside_profile =
      outside_environment.weatherProfile();
  outside_profile.cloud_layers = {};
  outside_profile.cloud_layers[0] = {
      cloud::CloudLayerKind::Cirrus, 13000.0f, 15000.0f, 1.0f, 30.0f,
      0.0f, 0.0f, 0.0f};
  outside_profile.cloud_layer_count = 1;
  outside_environment.setCustomWeather(outside_profile, 0.0);
  outside_environment.step(0.0);
  const cloud::CloudForcing outside_forcing =
      outside_environment.cloudForcing(24);

  cloud::SkyEnvironment negligible_layer_environment(outside_config);
  cloud::WeatherProfile negligible_layer_profile =
      negligible_layer_environment.weatherProfile();
  negligible_layer_profile.cloud_layers = {};
  negligible_layer_profile.cloud_layers[0] = {
      cloud::CloudLayerKind::Cirrus, 8000.0f, 10000.0f, 0.00001f, 0.0001f,
      0.0f, 0.0f, 0.0001f};
  negligible_layer_profile.cloud_layer_count = 1U;
  negligible_layer_environment.setCustomWeather(negligible_layer_profile,
                                                 0.0);
  negligible_layer_environment.step(0.0);
  const bool negligible_layer_filtered =
      negligible_layer_environment.cloudForcing(24).cloud_layer_count == 0U;

  cloud::WeatherProfile no_layer_profile = outside_profile;
  no_layer_profile.cloud_layers = {};
  no_layer_profile.cloud_layer_count = 0;
  cloud::SkyEnvironment no_layer_environment(outside_config);
  no_layer_environment.setCustomWeather(no_layer_profile, 0.0);
  no_layer_environment.step(0.0);
  const cloud::CloudForcing no_layer_forcing =
      no_layer_environment.cloudForcing(24);
  const bool outside_layer_isolated =
      outside_forcing.cloud_layer_count == 0U &&
      outside_forcing.vapor_source_multiplier ==
          no_layer_forcing.vapor_source_multiplier &&
      outside_forcing.surface_vapor_target ==
          no_layer_forcing.surface_vapor_target &&
      outside_forcing.top_vapor_target ==
          no_layer_forcing.top_vapor_target &&
      outside_forcing.vapor_target_response_per_second ==
          no_layer_forcing.vapor_target_response_per_second &&
      outside_forcing.cloud_decay_per_second ==
          no_layer_forcing.cloud_decay_per_second &&
      outside_forcing.precipitation_threshold ==
          no_layer_forcing.precipitation_threshold;

  cloud::WeatherProfile partial_profile = no_layer_profile;
  partial_profile.cloud_layers[0] = {
      cloud::CloudLayerKind::Stratiform, 10000.0f, 14000.0f, 0.8f,
      40.0f, 1.0f, 0.0f, 0.0f};
  partial_profile.cloud_layer_count = 1;
  cloud::SkyEnvironment partial_environment(outside_config);
  partial_environment.setCustomWeather(partial_profile, 0.0);
  partial_environment.step(0.0);
  const cloud::CloudForcing partial_forcing =
      partial_environment.cloudForcing(24);
  const bool partial_layer_optics_ok =
      partial_forcing.cloud_layer_count == 1U &&
      std::abs(partial_forcing.cloud_layers[0].optical_depth - 20.0f) <
          0.0001f;

  const cloud::CloudForcing physical_forcing_24 =
      sky_environment.cloudForcing(24);
  const cloud::CloudForcing physical_forcing_48 =
      sky_environment.cloudForcing(48);
  cloud::SkyEnvironmentConfig tall_domain_config = sky_config;
  tall_domain_config.time_scale = 0.0f;
  tall_domain_config.domain.vertical_extent_m = 24000.0f;
  cloud::SkyEnvironment tall_domain_environment(tall_domain_config);
  tall_domain_environment.setCustomWeather(sky_environment.weatherProfile(),
                                           0.0);
  const cloud::CloudForcing physical_forcing_tall =
      tall_domain_environment.cloudForcing(47);
  const auto decode_vertical_acceleration = [](float cells_per_second_squared,
                                               int grid,
                                               float height_m) {
    return cells_per_second_squared * height_m /
           static_cast<float>(grid - 1);
  };
  const float updraft_acceleration_24 = decode_vertical_acceleration(
      physical_forcing_24
          .updraft_acceleration_cells_per_second_squared,
      24, sky_environment.snapshot().domain.vertical_extent_m);
  const float updraft_acceleration_48 = decode_vertical_acceleration(
      physical_forcing_48
          .updraft_acceleration_cells_per_second_squared,
      48, sky_environment.snapshot().domain.vertical_extent_m);
  const float updraft_acceleration_tall = decode_vertical_acceleration(
      physical_forcing_tall
          .updraft_acceleration_cells_per_second_squared,
      47, tall_domain_environment.snapshot().domain.vertical_extent_m);
  const float buoyancy_acceleration_24 = decode_vertical_acceleration(
      physical_forcing_24
          .buoyancy_acceleration_cells_per_second_squared,
      24, sky_environment.snapshot().domain.vertical_extent_m);
  const float buoyancy_acceleration_tall = decode_vertical_acceleration(
      physical_forcing_tall
          .buoyancy_acceleration_cells_per_second_squared,
      47, tall_domain_environment.snapshot().domain.vertical_extent_m);
  const bool physical_vertical_units_ok =
      std::abs(updraft_acceleration_24 - updraft_acceleration_48) < 1.0e-6f &&
      std::abs(updraft_acceleration_24 - updraft_acceleration_tall) <
          1.0e-6f &&
      std::abs(buoyancy_acceleration_24 - buoyancy_acceleration_tall) <
          1.0e-6f;

  cloud::CloudSimulation source_free_simulation(24, 4);
  source_free_simulation.setEnvironmentalForcing(outside_forcing);
  for (int step = 0; step < 40; ++step) {
    source_free_simulation.step(0.05f);
  }
  const bool layer_forcing_ok =
      low_layer_simulation.allFinite() && high_layer_simulation.allFinite() &&
      low_cloud_height > 0.05f && low_cloud_height < 0.45f &&
      high_cloud_height > 0.55f && high_cloud_height < 0.95f &&
      high_cloud_height - low_cloud_height > 0.35f &&
      zero_coverage_layer_is_inert && negligible_layer_filtered &&
      outside_layer_isolated && partial_layer_optics_ok &&
      physical_vertical_units_ok &&
      source_free_simulation.densityStats().maximum == 0.0f;

  cloud::SkyEnvironmentConfig transition_config = sky_config;
  transition_config.time_scale = 0.0f;
  transition_config.weather_preset = cloud::WeatherPreset::Clear;
  cloud::SkyEnvironment layer_transition_environment(transition_config);
  cloud::SkyStatePacketContext transition_context;
  transition_context.weather_seed = transition_config.weather_seed;
  transition_context.evolution_mode = cloud::SkyEvolutionMode::Manual;
  bool layer_transition_packets_ok = true;
  bool transition_layer_altitudes_ok = true;
  bool saw_transition_cirrus = false;
  bool saw_transition_convective = false;
  auto validate_transition_packet = [&] {
    const auto packet = cloud::serializeSkyState(
        layer_transition_environment.snapshot(), transition_context);
    cloud::EnvironmentSnapshot decoded;
    cloud::SkyStatePacketContext decoded_context;
    layer_transition_packets_ok =
        layer_transition_packets_ok &&
        cloud::deserializeSkyState(packet.data(), packet.size(), decoded,
                                   decoded_context);
    for (std::size_t index = 0; index < decoded.cloud_layer_count; ++index) {
      const cloud::CloudLayerState &layer = decoded.cloud_layers[index];
      if (layer.kind == cloud::CloudLayerKind::Cirrus) {
        saw_transition_cirrus = true;
        transition_layer_altitudes_ok =
            transition_layer_altitudes_ok &&
            layer.base_altitude_m >= 7000.0f;
      }
      if (layer.kind == cloud::CloudLayerKind::Convective) {
        saw_transition_convective = true;
        transition_layer_altitudes_ok =
            transition_layer_altitudes_ok &&
            layer.base_altitude_m >= 300.0f &&
            layer.base_altitude_m <= 2000.0f;
      }
    }
  };
  layer_transition_environment.setWeatherPreset(
      cloud::WeatherPreset::Storm, 1.0);
  for (int step = 0; step < 20; ++step) {
    layer_transition_environment.step(0.05);
    validate_transition_packet();
  }
  layer_transition_packets_ok =
      layer_transition_packets_ok && transition_layer_altitudes_ok &&
      saw_transition_cirrus && saw_transition_convective;
  layer_transition_environment.setWeatherPreset(
      cloud::WeatherPreset::Clear, 1.0);
  for (int step = 0; step < 20; ++step) {
    layer_transition_environment.step(0.05);
    validate_transition_packet();
  }

  cloud::SkyEnvironmentConfig control_mode_config = sky_config;
  control_mode_config.time_scale = 0.0f;
  control_mode_config.weather_preset = cloud::WeatherPreset::Natural;
  cloud::SkyEnvironment control_mode_environment(control_mode_config);
  SkyControlRuntime control_mode_runtime;
  control_mode_runtime.evolution_mode = cloud::SkyEvolutionMode::Natural;
  cloud::SkyControlCommand keyframe_command;
  keyframe_command.session_id = 10U;
  keyframe_command.sequence = 1U;
  keyframe_command.client_time_seconds = 1.0;
  keyframe_command.opcode = cloud::SkyControlOpcode::RequestKeyframe;
  keyframe_command.evolution_mode = cloud::SkyEvolutionMode::Manual;
  keyframe_command.transition_milliseconds = 0U;
  keyframe_command.preset = cloud::WeatherPreset::Natural;
  applySkyControl(control_mode_environment, keyframe_command,
                  control_mode_runtime);
  cloud::SkyControlCommand utc_only_command;
  utc_only_command.session_id = 10U;
  utc_only_command.sequence = 2U;
  utc_only_command.client_time_seconds = 2.0;
  utc_only_command.opcode = cloud::SkyControlOpcode::PatchOverride;
  utc_only_command.evolution_mode = cloud::SkyEvolutionMode::Manual;
  utc_only_command.apply_mask = cloud::kSkyApplyUtc;
  utc_only_command.values.utc_unix_seconds = 1710936060.0;
  applySkyControl(control_mode_environment, utc_only_command,
                  control_mode_runtime);
  const bool control_mode_ok =
      control_mode_runtime.evolution_mode ==
          cloud::SkyEvolutionMode::Natural &&
      control_mode_environment.snapshot().weather.preset ==
          cloud::WeatherPreset::Natural;

  cloud::SkyEnvironment sparse_layer_environment(transition_config);
  SkyControlRuntime sparse_layer_runtime;
  sparse_layer_runtime.evolution_mode = cloud::SkyEvolutionMode::Manual;
  cloud::SkyControlCommand sparse_layer_command;
  sparse_layer_command.session_id = 11U;
  sparse_layer_command.sequence = 1U;
  sparse_layer_command.client_time_seconds = 1.0;
  sparse_layer_command.opcode = cloud::SkyControlOpcode::PatchOverride;
  sparse_layer_command.evolution_mode = cloud::SkyEvolutionMode::Manual;
  sparse_layer_command.transition_milliseconds = 0U;
  sparse_layer_command.apply_mask = cloud::kSkyApplyCloudLayer3;
  sparse_layer_command.values.cloud_layers[3] = {
      cloud::CloudLayerKind::Stratiform, 1500.0f, 3000.0f, 0.7f, 20.0f,
      1.0f, 0.0f, 0.1f};
  applySkyControl(sparse_layer_environment, sparse_layer_command,
                  sparse_layer_runtime);
  const bool sparse_layer_ok =
      sparse_layer_runtime.last_result == 2U &&
      sparse_layer_environment.snapshot().cloud_layer_count == 1U;

  cloud::SkyEnvironmentConfig terrain_config = transition_config;
  terrain_config.location.elevation_m = 1000.0;
  cloud::SkyEnvironment terrain_environment(terrain_config);
  SkyControlRuntime terrain_runtime;
  cloud::SkyControlCommand terrain_layer_command;
  terrain_layer_command.session_id = 12U;
  terrain_layer_command.sequence = 1U;
  terrain_layer_command.client_time_seconds = 1.0;
  terrain_layer_command.opcode = cloud::SkyControlOpcode::PatchOverride;
  terrain_layer_command.evolution_mode = cloud::SkyEvolutionMode::Manual;
  terrain_layer_command.transition_milliseconds = 0U;
  terrain_layer_command.apply_mask = cloud::kSkyApplyCloudLayer0;
  terrain_layer_command.values.cloud_layers[0] = {
      cloud::CloudLayerKind::Stratiform, 500.0f, 1100.0f, 0.8f, 500.0f,
      1.0f, 0.0f, 0.0f};
  applySkyControl(terrain_environment, terrain_layer_command,
                  terrain_runtime);
  terrain_environment.step(0.0);
  const cloud::CloudLayerState &terrain_layer =
      terrain_environment.weatherProfile().cloud_layers[0];
  const bool terrain_layer_clip_ok =
      terrain_runtime.last_result == 1U &&
      terrain_layer.base_altitude_m == 0.0f &&
      terrain_layer.top_altitude_m == 100.0f &&
      std::abs(terrain_layer.optical_depth - (500.0f / 6.0f)) < 0.001f;

  cloud::SkyEnvironmentConfig high_terrain_config = transition_config;
  high_terrain_config.location.elevation_m = 5000.0;
  cloud::SkyEnvironment high_terrain_environment(high_terrain_config);
  SkyControlRuntime high_terrain_runtime;
  cloud::SkyControlCommand high_terrain_layer_command;
  high_terrain_layer_command.session_id = 13U;
  high_terrain_layer_command.sequence = 1U;
  high_terrain_layer_command.client_time_seconds = 1.0;
  high_terrain_layer_command.opcode = cloud::SkyControlOpcode::PatchOverride;
  high_terrain_layer_command.evolution_mode =
      cloud::SkyEvolutionMode::Manual;
  high_terrain_layer_command.transition_milliseconds = 0U;
  high_terrain_layer_command.apply_mask = cloud::kSkyApplyCloudLayer0;
  high_terrain_layer_command.values.cloud_layers[0] = {
      cloud::CloudLayerKind::Convective, 6000.0f, 9000.0f, 0.8f, 40.0f,
      0.8f, 2.0f, 0.7f};
  const auto high_terrain_control_packet =
      cloud::serializeSkyControl(high_terrain_layer_command);
  applySkyControl(high_terrain_environment, high_terrain_layer_command,
                  high_terrain_runtime);
  high_terrain_environment.step(0.0);
  cloud::SkyStatePacketContext high_terrain_context;
  const auto high_terrain_state_packet = cloud::serializeSkyState(
      high_terrain_environment.snapshot(), high_terrain_context);
  constexpr std::size_t first_layer_kind_offset = 224U + 28U;
  const bool high_terrain_kind_ok =
      high_terrain_runtime.last_result == 1U &&
      high_terrain_control_packet[first_layer_kind_offset] == 3U &&
      high_terrain_state_packet[first_layer_kind_offset] == 3U;

  const bool cloud_formed = stats.maximum > 0.001f;
  const bool finite =
      simulation.allFinite() && std::isfinite(simulation.time());
  const bool protocol_ok = cloud::kV1HeaderBytes == 40 &&
                           cloud::kV2HeaderBytes == 64 && header_ok &&
                           codec_ok && crc_ok && control_ok && sky_state_ok &&
                           sky_control_ok && sky_patch_ok;
  const bool interaction_ok = interaction.active_interactors == 1U &&
                              interaction.affected_velocity_cells > 0U &&
                              interaction.wake_velocity_cells > 0U &&
                              !interaction.work_budget_exhausted &&
                              mass_interaction.displaced_scalar_cells > 0U &&
                              mass_interaction.displaced_cloud_mass > 0.0 &&
                              mass_interaction.invalid_scalar_targets == 0U &&
                              !mass_interaction.work_budget_exhausted &&
                              overlap_owner_ok;

  std::cout << "Self-test: voxels=" << voxel_count
            << " cloud_max=" << stats.maximum << " cloud_mean=" << stats.mean
            << " density16=" << density.bytes.size()
            << " velocity=" << velocity.bytes.size()
            << " finite=" << (finite ? "yes" : "no")
            << " v2_header=" << cloud::kV2HeaderBytes << " bytes"
            << " codec=" << (codec_ok ? "yes" : "no") << '\n';

  std::cout << "Interaction: velocity_cells="
            << interaction.affected_velocity_cells
            << " wake_cells=" << interaction.wake_velocity_cells
            << " scalar_cells=" << interaction.displaced_scalar_cells
            << " displaced_cloud=" << interaction.displaced_cloud_mass
            << " mass_test_cloud=" << mass_interaction.displaced_cloud_mass
            << " invalid_targets=" << interaction.invalid_scalar_targets
            << " stable_overlap=" << (overlap_owner_ok ? "yes" : "no")
            << " budget="
            << (interaction.work_budget_exhausted ? "exhausted" : "ok")
            << " control_packet=" << cloud::kControlPacketBytes << " bytes\n";

  std::cout << "Sky: sun_elevation="
            << initial_sky.celestial.sun_geometric_elevation_degrees
            << " moon_phase="
            << initial_sky.celestial.moon_illuminated_fraction
            << " state_packet=" << cloud::kSkyStatePacketBytes
            << " control_packet=" << cloud::kSkyControlPacketBytes
            << " state_crc=0x" << std::hex
            << cloud::detail::readU32(sky_packet.data(),
                                      cloud::kSkyStateCrcOffset)
            << " control_crc=0x"
            << cloud::detail::readU32(serialized_sky_control.data(),
                                      cloud::kSkyControlCrcOffset)
            << std::dec
            << " forcing=" << (forcing_ok ? "yes" : "no")
            << " layer_centers=" << low_cloud_height << ','
            << high_cloud_height << '\n';

  if (!fields_ok || !cloud_formed || !finite || !protocol_ok ||
      !interaction_ok || !astronomy_ok || !clock_pause_ok ||
      !spin_up_clock_ok || !wall_clock_ok || !boundary_clock_ok ||
      !altitude_atmosphere_ok ||
      !catch_up_ok || !long_gap_ok || !weather_ok || !forcing_ok ||
      !layer_forcing_ok || !layer_transition_packets_ok ||
      !control_mode_ok || !sparse_layer_ok || !terrain_layer_clip_ok ||
      !high_terrain_kind_ok) {
    std::cerr << "Checks: fields=" << fields_ok
              << " cloud=" << cloud_formed << " finite=" << finite
              << " protocol=" << protocol_ok
              << " sky_control_edges=" << optical_boundary_round_trip << ','
              << gust_boundary_round_trip
              << " interaction=" << interaction_ok
              << " astronomy=" << astronomy_ok
              << " pause=" << clock_pause_ok
              << " spinup=" << spin_up_clock_ok
              << " wall_clock=" << wall_clock_ok
              << " boundary_clock=" << boundary_clock_ok
              << " altitude_atmosphere=" << altitude_atmosphere_ok
              << " catch_up=" << catch_up_ok
              << " long_gap=" << long_gap_ok
              << " weather=" << weather_ok
              << " forcing=" << forcing_ok
              << " layers=" << layer_forcing_ok
              << " layer_transition=" << layer_transition_packets_ok
              << " control_mode=" << control_mode_ok
              << " sparse_layer=" << sparse_layer_ok
              << " terrain_clip=" << terrain_layer_clip_ok
              << " terrain_kind=" << high_terrain_kind_ok
              << " terrain_kind_values="
              << static_cast<unsigned>(
                     high_terrain_control_packet[first_layer_kind_offset])
              << ','
              << static_cast<unsigned>(
                     high_terrain_state_packet[first_layer_kind_offset])
              << ','
              << static_cast<unsigned>(high_terrain_environment.snapshot()
                                           .cloud_layers[0]
                                           .kind)
              << ','
              << high_terrain_environment.snapshot()
                     .cloud_layers[0]
                     .top_altitude_m
              << " terrain_values="
              << static_cast<unsigned>(terrain_runtime.last_result) << ','
              << terrain_layer.base_altitude_m << ','
              << terrain_layer.top_altitude_m << ','
              << terrain_layer.optical_depth << '\n';
    std::cerr << "Self-test FAILED\n";
    return 1;
  }
  std::cout << "Self-test PASSED\n";
  return 0;
}

int runServer(const Options &options) {
  cloud::SkyEnvironmentConfig sky_config;
  sky_config.utc_unix_seconds =
      std::isfinite(options.utc_unix_seconds)
          ? options.utc_unix_seconds
          : std::chrono::duration<double>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
  sky_config.time_scale = static_cast<float>(options.time_scale);
  sky_config.location = {options.latitude_degrees, options.longitude_degrees,
                         options.elevation_m};
  sky_config.domain = {static_cast<float>(options.domain_width_m),
                       static_cast<float>(options.domain_height_m)};
  sky_config.weather_preset = options.weather_preset;
  sky_config.weather_seed = options.weather_seed;
  cloud::SkyEnvironment environment(sky_config);

  cloud::CloudSimulation simulation(options.grid, options.pressure_iterations);
  simulation.setEnvironmentalForcing(environment.cloudForcing(options.grid));
  std::unique_ptr<cloud::InteractorControlReceiver> control;
  if (options.control) {
    control = std::make_unique<cloud::InteractorControlReceiver>(
        options.control_host, options.control_port,
        static_cast<std::size_t>(options.max_interactors));
  }
  std::unique_ptr<cloud::SkyControlReceiver> sky_control;
  if (options.sky_control) {
    sky_control = std::make_unique<cloud::SkyControlReceiver>(
        options.sky_control_host, options.sky_control_port);
  }
  SkyControlRuntime sky_runtime;
  sky_runtime.evolution_mode =
      options.weather_preset == cloud::WeatherPreset::Natural
          ? cloud::SkyEvolutionMode::Natural
          : cloud::SkyEvolutionMode::Manual;

  auto pollControls = [&] {
    if (control) {
      control->poll();
      simulation.setInteractors(control->activeInteractors());
    }
    if (sky_control) {
      sky_control->poll();
      while (const auto command = sky_control->takeNextCommand()) {
        applySkyControl(environment, *command, sky_runtime);
      }
    }
  };

  const double dt = 1.0 / static_cast<double>(options.simulation_hz);
  const float simulation_dt = static_cast<float>(dt);
  const int warmup_steps =
      static_cast<int>(std::ceil(options.warmup_seconds / dt));
  if (warmup_steps > 0) {
    std::cout << "Warming up " << warmup_steps << " simulation steps...\n";
    for (int step = 0; step < warmup_steps; ++step) {
      pollControls();
      environment.spinUp(static_cast<double>(simulation_dt));
      simulation.setEnvironmentalForcing(environment.cloudForcing(options.grid));
      simulation.step(simulation_dt);
    }
  }

  std::unique_ptr<cloud::UdpSender> sender;
  if (options.send) {
    sender = std::make_unique<cloud::UdpSender>(options.host, options.port);
  }
  std::unique_ptr<cloud::UdpSender> sky_sender;
  if (options.sky_send) {
    sky_sender =
        std::make_unique<cloud::UdpSender>(options.sky_host, options.sky_port);
  }

  std::cout << "Sky server started: " << options.grid << "^3, "
            << options.simulation_hz << " simulation Hz, " << options.send_hz
            << " send Hz, protocol v" << options.protocol;
  if (options.protocol == 2) {
    std::cout << ", fields=" << fieldMaskName(options.field_mask)
              << ", compression=" << compressionName(options.compression);
  }
  if (sender) {
    std::cout << ", UDP " << options.host << ':' << options.port;
  } else {
    std::cout << ", UDP disabled";
  }
  if (control) {
    std::cout << ", control " << options.control_host << ':'
              << options.control_port;
  } else {
    std::cout << ", control disabled";
  }
  if (sky_sender) {
    std::cout << ", SKS1 " << options.sky_host << ':' << options.sky_port
              << '@' << options.sky_send_hz << "Hz";
  } else {
    std::cout << ", SKS1 disabled";
  }
  if (sky_control) {
    std::cout << ", SKC1 " << options.sky_control_host << ':'
              << options.sky_control_port;
  } else {
    std::cout << ", SKC1 disabled";
  }
  std::cout << ", weather=" << cloud::weatherPresetName(options.weather_preset)
            << ", UTC scale=" << options.time_scale << 'x'
            << ", location=" << options.latitude_degrees << ','
            << options.longitude_degrees;
  std::cout << "\nPress Ctrl+C to stop.\n";

  using clock = std::chrono::steady_clock;
  const auto tick_duration = std::chrono::duration<double>(dt);
  const double send_interval = 1.0 / static_cast<double>(options.send_hz);
  const double sky_send_interval =
      1.0 / static_cast<double>(options.sky_send_hz);
  double send_accumulator = 0.0;
  double sky_send_accumulator = 0.0;
  std::uint32_t frame_id = 0;
  std::uint32_t last_volume_frame_id = 0;
  std::uint32_t sky_state_sequence = 0;
  auto next_tick = clock::now();
  auto last_environment_wall = next_tick;
  auto next_log = next_tick + std::chrono::seconds(1);
  const auto start_time = next_tick;
  auto last_log_time = next_tick;
  std::uint64_t last_log_bytes = 0;
  std::uint64_t last_log_packets = 0;

  while (keep_running != 0) {
    const auto now = clock::now();
    if (options.run_seconds > 0.0 &&
        std::chrono::duration<double>(now - start_time).count() >=
            options.run_seconds) {
      break;
    }

    pollControls();
    double environment_wall_remaining = std::max(
        0.0,
        std::chrono::duration<double>(now - last_environment_wall).count());
    last_environment_wall = now;
    const double first_environment_wall_step =
        std::min(environment_wall_remaining, 86400.0);
    environment.step(static_cast<double>(simulation_dt),
                     first_environment_wall_step);
    environment_wall_remaining -= first_environment_wall_step;
    while (environment_wall_remaining > 0.0) {
      const double wall_step =
          std::min(environment_wall_remaining, 86400.0);
      environment.step(0.0, wall_step);
      environment_wall_remaining -= wall_step;
    }
    simulation.setEnvironmentalForcing(environment.cloudForcing(options.grid));
    simulation.step(simulation_dt);
    send_accumulator += dt;
    sky_send_accumulator += dt;
    if (sender && send_accumulator + 0.0000001 >= send_interval) {
      last_volume_frame_id = frame_id;
      if (options.protocol == 1) {
        sender->sendDensityV1(simulation.densityBytes(), options.grid, frame_id,
                              static_cast<float>(simulation.time()));
      } else {
        sender->sendFrameV2(buildRendererFields(simulation, options), frame_id,
                            simulation.time(), options.compression);
      }
      ++frame_id;
      send_accumulator = std::max(0.0, send_accumulator - send_interval);
    }
    if (sky_sender &&
        (sky_runtime.state_keyframe_requested ||
         sky_send_accumulator + 0.0000001 >= sky_send_interval)) {
      cloud::SkyStatePacketContext sky_context;
      sky_context.last_control_sequence = sky_runtime.last_sequence;
      sky_context.last_control_session = sky_runtime.last_session;
      sky_context.volume_frame_id = last_volume_frame_id;
      sky_context.predicted_valid_time_seconds =
          environment.snapshot().fluid_time_seconds + sky_send_interval;
      sky_context.weather_seed = environment.weatherSeed();
      sky_context.evolution_mode = sky_runtime.evolution_mode;
      sky_context.last_control_result = sky_runtime.last_result;
      sky_context.active_cloud_field_mask =
          sender ? (options.protocol == 2
                        ? options.field_mask
                        : cloud::fieldBit(cloud::FieldId::Density))
                 : 0U;
      cloud::EnvironmentSnapshot transmitted_sky = environment.snapshot();
      transmitted_sky.sequence = sky_state_sequence++;
      const auto state_packet =
          cloud::serializeSkyState(transmitted_sky, sky_context);
      (void)sky_sender->sendSingleDatagram(state_packet.data(),
                                            state_packet.size());
      sky_runtime.state_keyframe_requested = false;
      sky_send_accumulator =
          std::max(0.0, sky_send_accumulator - sky_send_interval);
    }

    const auto after_work = clock::now();
    if (after_work >= next_log) {
      const auto stats = simulation.densityStats();
      const auto interaction = simulation.interactionStats();
      const auto &sky = environment.snapshot();
      std::cout << std::fixed << std::setprecision(4)
                << "t=" << simulation.time() << "s"
                << " frame=" << frame_id << " cloud[max=" << stats.maximum
                << ", mean=" << stats.mean << ']'
                << " interaction[active=" << interaction.active_interactors
                << ", velocity=" << interaction.affected_velocity_cells
                << ", scalar=" << interaction.displaced_scalar_cells << ']'
                << " quality[wake=" << interaction.wake_velocity_cells
                << ", invalid_targets=" << interaction.invalid_scalar_targets
                << ", budget="
                << (interaction.work_budget_exhausted ? "exhausted" : "ok")
                << ']'
                << " sky[weather="
                << cloud::weatherPresetName(sky.weather.preset)
                << ", sun="
                << sky.celestial.sun_geometric_elevation_degrees << "deg"
                << ", temp="
                << sky.weather.surface_temperature_kelvin - 273.15f << "C"
                << ", humidity=" << sky.weather.relative_humidity * 100.0f
                << "%, rain=" << sky.weather.precipitation_rate_mm_h
                << "mm/h]";
      if (control) {
        std::cout << " control[ok=" << control->acceptedPackets()
                  << ", rejected=" << control->rejectedPackets()
                  << ", stale=" << control->stalePackets()
                  << ", limited=" << control->limitedPackets()
                  << ", expired=" << control->expiredInteractors() << ']';
      }
      if (sender) {
        const double log_seconds =
            std::chrono::duration<double>(after_work - last_log_time).count();
        const std::uint64_t bytes = sender->totalBytesSent();
        const std::uint64_t packets = sender->totalPacketsSent();
        const double megabytes_per_second =
            static_cast<double>(bytes - last_log_bytes) /
            (1024.0 * 1024.0 * log_seconds);
        const double packets_per_second =
            static_cast<double>(packets - last_log_packets) / log_seconds;
        std::cout << " tx=" << std::setprecision(2) << megabytes_per_second
                  << " MiB/s" << ','
                  << static_cast<std::uint64_t>(packets_per_second) << " pkt/s"
                  << " drops[frame=" << sender->droppedFrames()
                  << ", packet=" << sender->droppedPackets() << ']';
        last_log_bytes = bytes;
        last_log_packets = packets;
        last_log_time = after_work;
      }
      if (sky_control) {
        std::cout << " sky_control[ok=" << sky_control->acceptedPackets()
                  << ", rejected=" << sky_control->rejectedPackets()
                  << ", stale=" << sky_control->stalePackets() << ']';
      }
      if (sky_sender) {
        std::cout << " sky_tx[packets=" << sky_sender->totalPacketsSent()
                  << ", drops=" << sky_sender->droppedPackets() << ']';
      }
      std::cout << '\n';
      next_log = after_work + std::chrono::seconds(1);
    }

    next_tick += std::chrono::duration_cast<clock::duration>(tick_duration);
    if (after_work < next_tick) {
      std::this_thread::sleep_until(next_tick);
    } else if (after_work - next_tick > std::chrono::seconds(1)) {
      next_tick = after_work;
    }
  }

  std::cout << "Sky server stopped at t=" << simulation.time() << "s.\n";
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  try {
    const Options options = parseOptions(argc, argv);
    if (options.self_test) {
      return runSelfTest();
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    return runServer(options);
  } catch (const std::exception &error) {
    std::cerr << "Error: " << error.what() << '\n';
    return 1;
  }
}
