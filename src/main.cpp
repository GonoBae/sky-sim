#include "control.hpp"
#include "protocol.hpp"
#include "simulation.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
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
  int max_interactors = 8;
  double run_seconds = 0.0;
  double warmup_seconds = 2.0;
  bool fields_explicit = false;
  bool send = true;
  bool control = true;
  bool self_test = false;
};

void signalHandler(int) { keep_running = 0; }

void printUsage(const char *executable) {
  std::cout
      << "Cloud simulation server 0.3.0\n\n"
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
      << "  --max-interactors N  Maximum simultaneous objects, 1..64 (default "
         "8)\n"
      << "  --warmup-seconds N   Simulate before the first transmitted frame "
         "(default 2)\n"
      << "  --seconds N          Stop after N wall-clock seconds; 0 runs "
         "forever\n"
      << "  --no-send            Simulate without UDP transmission\n"
      << "  --no-control         Disable dynamic object interaction input\n"
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

  const bool cloud_formed = stats.maximum > 0.001f;
  const bool finite =
      simulation.allFinite() && std::isfinite(simulation.time());
  const bool protocol_ok = cloud::kV1HeaderBytes == 40 &&
                           cloud::kV2HeaderBytes == 64 && header_ok &&
                           codec_ok && crc_ok && control_ok;
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

  if (!fields_ok || !cloud_formed || !finite || !protocol_ok ||
      !interaction_ok) {
    std::cerr << "Self-test FAILED\n";
    return 1;
  }
  std::cout << "Self-test PASSED\n";
  return 0;
}

int runServer(const Options &options) {
  cloud::CloudSimulation simulation(options.grid, options.pressure_iterations);
  std::unique_ptr<cloud::InteractorControlReceiver> control;
  if (options.control) {
    control = std::make_unique<cloud::InteractorControlReceiver>(
        options.control_host, options.control_port,
        static_cast<std::size_t>(options.max_interactors));
  }

  const double dt = 1.0 / static_cast<double>(options.simulation_hz);
  const int warmup_steps =
      static_cast<int>(std::ceil(options.warmup_seconds / dt));
  if (warmup_steps > 0) {
    std::cout << "Warming up " << warmup_steps << " simulation steps...\n";
    for (int step = 0; step < warmup_steps; ++step) {
      if (control) {
        control->poll();
        simulation.setInteractors(control->activeInteractors());
      }
      simulation.step(static_cast<float>(dt));
    }
  }

  std::unique_ptr<cloud::UdpSender> sender;
  if (options.send) {
    sender = std::make_unique<cloud::UdpSender>(options.host, options.port);
  }

  std::cout << "Cloud server started: " << options.grid << "^3, "
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
  std::cout << "\nPress Ctrl+C to stop.\n";

  using clock = std::chrono::steady_clock;
  const auto tick_duration = std::chrono::duration<double>(dt);
  const double send_interval = 1.0 / static_cast<double>(options.send_hz);
  double send_accumulator = 0.0;
  std::uint32_t frame_id = 0;
  auto next_tick = clock::now();
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

    if (control) {
      control->poll();
      simulation.setInteractors(control->activeInteractors());
    }
    simulation.step(static_cast<float>(dt));
    send_accumulator += dt;
    if (sender && send_accumulator + 0.0000001 >= send_interval) {
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

    const auto after_work = clock::now();
    if (after_work >= next_log) {
      const auto stats = simulation.densityStats();
      const auto interaction = simulation.interactionStats();
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
                << ']';
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

  std::cout << "Cloud server stopped at t=" << simulation.time() << "s.\n";
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
