#pragma once

#include "environment_types.hpp"
#include "protocol.hpp"
#include "sky_protocol.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace cloud {

constexpr std::uint16_t kSkyControlProtocolVersion = 1;
constexpr std::size_t kSkyControlPacketBytes = 512;
constexpr std::size_t kSkyControlCrcOffset = 508;

constexpr std::uint64_t kSkyApplyUtc = 1ULL << 0U;
constexpr std::uint64_t kSkyApplyLocation = 1ULL << 1U;
constexpr std::uint64_t kSkyApplyDomain = 1ULL << 2U;
constexpr std::uint64_t kSkyApplyTimeScale = 1ULL << 3U;
constexpr std::uint64_t kSkyApplyEvolution = 1ULL << 4U;
constexpr std::uint64_t kSkyApplyThermodynamics = 1ULL << 5U;
constexpr std::uint64_t kSkyApplyVisibility = 1ULL << 6U;
constexpr std::uint64_t kSkyApplyWind = 1ULL << 7U;
constexpr std::uint64_t kSkyApplyPrecipitation = 1ULL << 8U;
constexpr std::uint64_t kSkyApplyConvection = 1ULL << 9U;
constexpr std::uint64_t kSkyApplyCloudLayer0 = 1ULL << 11U;
constexpr std::uint64_t kSkyApplyCloudLayer1 = 1ULL << 12U;
constexpr std::uint64_t kSkyApplyCloudLayer2 = 1ULL << 13U;
constexpr std::uint64_t kSkyApplyCloudLayer3 = 1ULL << 14U;
constexpr std::uint64_t kSkyApplyAtmosphereOptics = 1ULL << 19U;
constexpr std::uint64_t kSupportedSkyApplyMask =
    kSkyApplyUtc | kSkyApplyLocation | kSkyApplyDomain |
    kSkyApplyTimeScale | kSkyApplyEvolution | kSkyApplyThermodynamics |
    kSkyApplyVisibility | kSkyApplyWind | kSkyApplyPrecipitation |
    kSkyApplyConvection | kSkyApplyCloudLayer0 | kSkyApplyCloudLayer1 |
    kSkyApplyCloudLayer2 | kSkyApplyCloudLayer3;
constexpr std::uint64_t kReleasableSkyApplyMask =
    kSkyApplyThermodynamics | kSkyApplyVisibility | kSkyApplyWind |
    kSkyApplyPrecipitation | kSkyApplyConvection |
    kSkyApplyCloudLayer0 | kSkyApplyCloudLayer1 | kSkyApplyCloudLayer2 |
    kSkyApplyCloudLayer3;

enum class SkyControlOpcode : std::uint8_t {
  PatchOverride = 1,
  ReleaseOverride = 2,
  LoadPreset = 3,
  RequestKeyframe = 4,
};

struct SkyControlCommand {
  std::uint32_t session_id = 0;
  std::uint32_t sequence = 0;
  double client_time_seconds = 0.0;
  SkyControlOpcode opcode = SkyControlOpcode::LoadPreset;
  SkyEvolutionMode evolution_mode = SkyEvolutionMode::Manual;
  std::uint32_t transition_milliseconds = 5000;
  std::uint32_t hold_milliseconds = 0;
  WeatherPreset preset = WeatherPreset::Cumulus;
  std::uint64_t apply_mask = 0;
  std::uint64_t clear_mask = 0;
  std::uint32_t weather_seed = 1;
  std::uint16_t requested_cloud_field_mask = 0;
  std::uint16_t requested_state_hz = 0;
  EnvironmentSnapshot values{};
};

namespace sky_control_detail {

inline bool finiteVector(EnvironmentVector value) {
  return std::isfinite(value.east) && std::isfinite(value.north) &&
         std::isfinite(value.up);
}

inline bool finiteRgb(const std::array<float, 3> &value) {
  return std::isfinite(value[0]) && std::isfinite(value[1]) &&
         std::isfinite(value[2]);
}

inline void writeLayer(std::uint8_t *output, std::size_t index,
                       const CloudLayerState &layer) {
  const std::size_t offset = 224U + index * 32U;
  const float thickness =
      std::max(1.0f, layer.top_altitude_m - layer.base_altitude_m);
  const float condensate =
      std::clamp(layer.optical_depth / (100.0f * thickness), 0.0f, 0.01f);
  detail::writeF32(output, offset, layer.base_altitude_m);
  detail::writeF32(output, offset + 4U, layer.top_altitude_m);
  detail::writeF32(output, offset + 8U, layer.coverage);
  detail::writeF32(output, offset + 12U, condensate);
  detail::writeF32(output, offset + 16U, 1.0f - layer.liquid_fraction);
  detail::writeF32(output, offset + 20U,
                   layer.precipitation_rate_mm_h / 3600.0f);
  detail::writeF32(output, offset + 24U,
                   0.25f + 4.0f * layer.convective_activity);
  output[offset + 28U] =
      sky_detail::wireCloudKind(layer, layer.top_altitude_m);
  output[offset + 29U] = static_cast<std::uint8_t>(
      (layer.convective_activity > 0.35f ? 1U : 0U) |
      (layer.precipitation_rate_mm_h > 0.001f ? 2U : 0U));
}

inline CloudLayerState readLayer(const std::uint8_t *input,
                                 std::size_t index) {
  const std::size_t offset = 224U + index * 32U;
  CloudLayerState result;
  result.base_altitude_m = detail::readF32(input, offset);
  result.top_altitude_m = detail::readF32(input, offset + 4U);
  result.coverage = detail::readF32(input, offset + 8U);
  result.optical_depth =
      detail::readF32(input, offset + 12U) * 100.0f *
      std::max(1.0f, result.top_altitude_m - result.base_altitude_m);
  const float maximum_optical_depth = std::min(
      500.0f, result.top_altitude_m - result.base_altitude_m);
  constexpr float optical_depth_wire_tolerance = 0.001f;
  if (std::isfinite(result.optical_depth) &&
      result.optical_depth > maximum_optical_depth &&
      result.optical_depth <=
          maximum_optical_depth + optical_depth_wire_tolerance) {
    result.optical_depth = maximum_optical_depth;
  }
  result.liquid_fraction = 1.0f - detail::readF32(input, offset + 16U);
  result.precipitation_rate_mm_h =
      detail::readF32(input, offset + 20U) * 3600.0f;
  result.convective_activity = std::clamp(
      (detail::readF32(input, offset + 24U) - 0.25f) / 4.0f, 0.0f,
      1.0f);
  result.kind =
      sky_detail::internalCloudKind(input[offset + 28U],
                                    result.base_altitude_m);
  return result;
}

inline bool rawLayerFieldsAreValid(const std::uint8_t *input,
                                   std::size_t index) {
  const std::size_t offset = 224U + index * 32U;
  for (std::size_t field_offset = 0U; field_offset <= 24U;
       field_offset += 4U) {
    if (!std::isfinite(detail::readF32(input, offset + field_offset))) {
      return false;
    }
  }
  const float turbulence = detail::readF32(input, offset + 24U);
  return turbulence >= 0.25f && turbulence <= 4.25f;
}

inline bool validLayer(const CloudLayerState &layer) {
  return std::isfinite(layer.base_altitude_m) &&
         std::isfinite(layer.top_altitude_m) &&
         std::isfinite(layer.coverage) && std::isfinite(layer.optical_depth) &&
         std::isfinite(layer.liquid_fraction) &&
         std::isfinite(layer.precipitation_rate_mm_h) &&
         std::isfinite(layer.convective_activity) &&
         layer.base_altitude_m >= -500.0f &&
         layer.top_altitude_m > layer.base_altitude_m &&
         layer.top_altitude_m <= 100000.0f && layer.coverage >= 0.0f &&
         layer.coverage <= 1.0f && layer.optical_depth >= 0.0f &&
         layer.optical_depth <= 500.0f &&
         layer.optical_depth <=
             layer.top_altitude_m - layer.base_altitude_m &&
         layer.liquid_fraction >= 0.0f &&
         layer.liquid_fraction <= 1.0f &&
         layer.precipitation_rate_mm_h >= 0.0f &&
         layer.precipitation_rate_mm_h <= 300.0f &&
         layer.convective_activity >= 0.0f &&
         layer.convective_activity <= 1.0f &&
         layer.kind != CloudLayerKind::None;
}

inline bool validCommand(const SkyControlCommand &command) {
  const auto evolution = static_cast<std::uint8_t>(command.evolution_mode);
  if (command.session_id == 0U ||
      !std::isfinite(command.client_time_seconds) ||
      command.client_time_seconds < 0.0 ||
      evolution < static_cast<std::uint8_t>(SkyEvolutionMode::Natural) ||
      evolution > static_cast<std::uint8_t>(SkyEvolutionMode::Replay) ||
      command.transition_milliseconds > 86400000U ||
      command.hold_milliseconds != 0U ||
      command.requested_state_hz != 0U ||
      command.requested_cloud_field_mask != 0U ||
      (command.apply_mask & ~kSupportedSkyApplyMask) != 0U ||
      (command.clear_mask & ~kSupportedSkyApplyMask) != 0U) {
    return false;
  }

  if (command.opcode == SkyControlOpcode::LoadPreset) {
    return command.apply_mask == 0U && command.clear_mask == 0U &&
           command.preset != WeatherPreset::Custom &&
           static_cast<std::uint8_t>(command.preset) <=
               static_cast<std::uint8_t>(WeatherPreset::Fog);
  }
  if (command.opcode == SkyControlOpcode::RequestKeyframe) {
    return command.apply_mask == 0U && command.clear_mask == 0U &&
           command.transition_milliseconds == 0U &&
           command.preset == WeatherPreset::Natural;
  }
  if (command.opcode == SkyControlOpcode::ReleaseOverride) {
    return command.apply_mask == 0U &&
           command.clear_mask == kReleasableSkyApplyMask &&
           command.transition_milliseconds <= 86400000U;
  }
  if (command.opcode != SkyControlOpcode::PatchOverride ||
      command.apply_mask == 0U || command.clear_mask != 0U) {
    return false;
  }

  if ((command.apply_mask & kSkyApplyEvolution) != 0U) {
    if (command.evolution_mode != SkyEvolutionMode::Natural &&
        command.evolution_mode != SkyEvolutionMode::Manual) {
      return false;
    }
    if (command.evolution_mode == SkyEvolutionMode::Natural &&
        (command.apply_mask & kReleasableSkyApplyMask) != 0U) {
      return false;
    }
  }

  const EnvironmentSnapshot &value = command.values;
  if ((command.apply_mask & kSkyApplyUtc) != 0U &&
      (!std::isfinite(value.utc_unix_seconds) ||
       value.utc_unix_seconds < -2208988800.0 ||
       value.utc_unix_seconds > 4133980800.0)) {
    return false;
  }
  if ((command.apply_mask & kSkyApplyLocation) != 0U &&
      (!std::isfinite(value.location.latitude_degrees) ||
       !std::isfinite(value.location.longitude_degrees) ||
       !std::isfinite(value.location.elevation_m) ||
       value.location.latitude_degrees < -90.0 ||
       value.location.latitude_degrees > 90.0 ||
       value.location.longitude_degrees < -180.0 ||
       value.location.longitude_degrees > 180.0 ||
       value.location.elevation_m < -500.0 ||
       value.location.elevation_m > 100000.0)) {
    return false;
  }
  if ((command.apply_mask & kSkyApplyDomain) != 0U &&
      (!std::isfinite(value.domain.horizontal_extent_m) ||
       !std::isfinite(value.domain.vertical_extent_m) ||
       value.domain.horizontal_extent_m < 100.0f ||
       value.domain.horizontal_extent_m > 2000000.0f ||
       value.domain.vertical_extent_m < 100.0f ||
       value.domain.vertical_extent_m > 100000.0f)) {
    return false;
  }
  if ((command.apply_mask & kSkyApplyTimeScale) != 0U &&
      (!std::isfinite(value.time_scale) || value.time_scale < -86400.0f ||
       value.time_scale > 86400.0f)) {
    return false;
  }
  const WeatherState &weather = value.weather;
  if ((command.apply_mask & kSkyApplyThermodynamics) != 0U &&
      (!std::isfinite(weather.surface_temperature_kelvin) ||
       !std::isfinite(weather.sea_level_pressure_pa) ||
       !std::isfinite(weather.relative_humidity) ||
       weather.surface_temperature_kelvin < 203.15f ||
       weather.surface_temperature_kelvin > 333.15f ||
       weather.sea_level_pressure_pa < 80000.0f ||
       weather.sea_level_pressure_pa > 108000.0f ||
       weather.relative_humidity < 0.01f ||
       weather.relative_humidity > 1.0f)) {
    return false;
  }
  if ((command.apply_mask & kSkyApplyVisibility) != 0U &&
      (!std::isfinite(weather.visibility_m) ||
       !std::isfinite(weather.aerosol_optical_depth_550nm) ||
       !std::isfinite(weather.ozone_dobson_units) ||
       weather.visibility_m < 25.0f || weather.visibility_m > 200000.0f ||
       weather.aerosol_optical_depth_550nm < 0.005f ||
       weather.aerosol_optical_depth_550nm > 3.0f ||
       weather.ozone_dobson_units < 100.0f ||
       weather.ozone_dobson_units > 600.0f)) {
    return false;
  }
  if ((command.apply_mask & kSkyApplyWind) != 0U &&
      (!finiteVector(weather.wind_m_s) ||
       !std::isfinite(weather.gust_speed_m_s) ||
       sky_detail::vectorLength(weather.wind_m_s) > 150.0f ||
       weather.gust_speed_m_s < sky_detail::vectorLength(weather.wind_m_s) ||
       weather.gust_speed_m_s > 200.0f)) {
    return false;
  }
  if ((command.apply_mask & kSkyApplyPrecipitation) != 0U &&
      (!std::isfinite(weather.precipitation_rate_mm_h) ||
       !std::isfinite(weather.snow_fraction) ||
       !std::isfinite(weather.surface_wetness) ||
       weather.precipitation_rate_mm_h < 0.0f ||
       weather.precipitation_rate_mm_h > 300.0f ||
       weather.snow_fraction < 0.0f || weather.snow_fraction > 1.0f ||
       weather.surface_wetness < 0.0f || weather.surface_wetness > 1.0f)) {
    return false;
  }
  if ((command.apply_mask & kSkyApplyConvection) != 0U &&
      (!std::isfinite(weather.convective_activity) ||
       !std::isfinite(weather.lightning_activity) ||
       weather.convective_activity < 0.0f ||
       weather.convective_activity > 1.0f ||
       weather.lightning_activity < 0.0f ||
       weather.lightning_activity > 1.0f)) {
    return false;
  }
  for (std::size_t index = 0; index < kMaximumCloudLayers; ++index) {
    const std::uint64_t bit = kSkyApplyCloudLayer0 << index;
    if ((command.apply_mask & bit) != 0U &&
        !validLayer(value.cloud_layers[index])) {
      return false;
    }
  }
  if ((command.apply_mask & kSkyApplyAtmosphereOptics) != 0U) {
    const AtmosphereOptics &optics = value.optics;
    if (!finiteRgb(optics.rayleigh_scattering_per_m) ||
        !finiteRgb(optics.mie_scattering_per_m) ||
        !finiteRgb(optics.mie_absorption_per_m) ||
        !finiteRgb(optics.ozone_absorption_per_m) ||
        !finiteRgb(optics.ground_albedo) ||
        !std::isfinite(optics.mie_anisotropy) ||
        !std::isfinite(optics.mie_scale_height_m) ||
        !std::isfinite(optics.rayleigh_scale_height_m) ||
        optics.mie_anisotropy < -0.99f || optics.mie_anisotropy > 0.99f ||
        optics.mie_scale_height_m <= 0.0f ||
        optics.rayleigh_scale_height_m <= 0.0f) {
      return false;
    }
  }
  return true;
}

inline bool unusedControlBytesAreZero(const std::uint8_t *data,
                                      std::uint64_t apply_mask) {
  std::array<bool, kSkyControlPacketBytes> used{};
  auto mark = [&](std::size_t offset, std::size_t count) {
    for (std::size_t index = offset; index < offset + count; ++index) {
      used[index] = true;
    }
  };
  mark(0, 64);
  mark(kSkyControlCrcOffset, 4);
  if ((apply_mask & kSkyApplyUtc) != 0U) {
    mark(64, 8);
  }
  if ((apply_mask & kSkyApplyLocation) != 0U) {
    mark(72, 20);
  }
  if ((apply_mask & kSkyApplyTimeScale) != 0U) {
    mark(92, 4);
  }
  if ((apply_mask & kSkyApplyDomain) != 0U) {
    mark(96, 12);
  }
  if ((apply_mask & kSkyApplyThermodynamics) != 0U) {
    mark(108, 12);
  }
  if ((apply_mask & kSkyApplyVisibility) != 0U) {
    mark(120, 12);
  }
  if ((apply_mask & kSkyApplyWind) != 0U) {
    mark(132, 24);
  }
  if ((apply_mask & kSkyApplyPrecipitation) != 0U) {
    mark(160, 12);
    mark(176, 4);
  }
  if ((apply_mask & kSkyApplyConvection) != 0U) {
    mark(184, 4);
    mark(192, 4);
  }
  for (std::size_t index = 0; index < kMaximumCloudLayers; ++index) {
    if ((apply_mask & (kSkyApplyCloudLayer0 << index)) != 0U) {
      mark(224U + index * 32U, 30);
    }
  }
  if ((apply_mask & kSkyApplyAtmosphereOptics) != 0U) {
    mark(196, 24);
    mark(432, 48);
  }
  for (std::size_t index = 64; index < kSkyControlCrcOffset; ++index) {
    if (!used[index] && data[index] != 0U) {
      return false;
    }
  }
  return true;
}

} // namespace sky_control_detail

inline std::array<std::uint8_t, kSkyControlPacketBytes>
serializeSkyControl(const SkyControlCommand &command) {
  if (!sky_control_detail::validCommand(command)) {
    throw std::invalid_argument("Cannot serialize an invalid SKC1 command");
  }
  std::array<std::uint8_t, kSkyControlPacketBytes> output{};
  std::memcpy(output.data(), "SKC1", 4);
  detail::writeU16(output.data(), 4, kSkyControlProtocolVersion);
  detail::writeU16(output.data(), 6,
                   static_cast<std::uint16_t>(kSkyControlPacketBytes));
  detail::writeU32(output.data(), 8, command.session_id);
  detail::writeU32(output.data(), 12, command.sequence);
  detail::writeF64(output.data(), 16, command.client_time_seconds);
  output[24] = static_cast<std::uint8_t>(command.opcode);
  output[25] = static_cast<std::uint8_t>(command.evolution_mode);
  detail::writeU32(output.data(), 28, command.transition_milliseconds);
  detail::writeU32(output.data(), 32, command.hold_milliseconds);
  detail::writeU32(output.data(), 36,
                   static_cast<std::uint32_t>(command.preset));
  detail::writeU64(output.data(), 40, command.apply_mask);
  detail::writeU64(output.data(), 48, command.clear_mask);
  detail::writeU32(output.data(), 56, command.weather_seed);
  detail::writeU16(output.data(), 60, command.requested_cloud_field_mask);
  detail::writeU16(output.data(), 62, command.requested_state_hz);

  const EnvironmentSnapshot &value = command.values;
  const WeatherState &weather = value.weather;
  if ((command.apply_mask & kSkyApplyUtc) != 0U) {
    detail::writeF64(output.data(), 64, value.utc_unix_seconds);
  }
  if ((command.apply_mask & kSkyApplyLocation) != 0U) {
    detail::writeF64(output.data(), 72, value.location.latitude_degrees);
    detail::writeF64(output.data(), 80, value.location.longitude_degrees);
    detail::writeF32(output.data(), 88,
                     static_cast<float>(value.location.elevation_m));
  }
  if ((command.apply_mask & kSkyApplyTimeScale) != 0U) {
    detail::writeF32(output.data(), 92, value.time_scale);
  }
  if ((command.apply_mask & kSkyApplyDomain) != 0U) {
    detail::writeF32(output.data(), 96, value.domain.horizontal_extent_m);
    detail::writeF32(output.data(), 100, value.domain.horizontal_extent_m);
    detail::writeF32(output.data(), 104, value.domain.vertical_extent_m);
  }
  if ((command.apply_mask & kSkyApplyThermodynamics) != 0U) {
    detail::writeF32(output.data(), 108, weather.surface_temperature_kelvin);
    detail::writeF32(output.data(), 112, weather.sea_level_pressure_pa);
    detail::writeF32(output.data(), 116, weather.relative_humidity);
  }
  if ((command.apply_mask & kSkyApplyVisibility) != 0U) {
    detail::writeF32(output.data(), 120, weather.visibility_m);
    detail::writeF32(output.data(), 124,
                     weather.aerosol_optical_depth_550nm);
    detail::writeF32(output.data(), 128, weather.ozone_dobson_units);
  }
  if ((command.apply_mask & kSkyApplyWind) != 0U) {
    sky_detail::writeVector(output.data(), 132, weather.wind_m_s);
    const float mean_speed = sky_detail::vectorLength(weather.wind_m_s);
    const float delta = std::max(0.0f, weather.gust_speed_m_s - mean_speed);
    const float inverse = mean_speed > 0.0001f ? 1.0f / mean_speed : 0.0f;
    sky_detail::writeVector(output.data(), 144,
                            mean_speed > 0.0001f
                                ? EnvironmentVector{
                                      weather.wind_m_s.east * inverse * delta,
                                      weather.wind_m_s.north * inverse * delta,
                                      weather.wind_m_s.up * inverse * delta}
                                : EnvironmentVector{delta, 0.0f, 0.0f});
  }
  if ((command.apply_mask & kSkyApplyPrecipitation) != 0U) {
    detail::writeF32(output.data(), 160,
                     weather.precipitation_rate_mm_h / 3600.0f);
    detail::writeF32(output.data(), 164, 1.0f - weather.snow_fraction);
    detail::writeF32(output.data(), 168, weather.snow_fraction);
    detail::writeF32(output.data(), 176, weather.surface_wetness);
  }
  if ((command.apply_mask & kSkyApplyConvection) != 0U) {
    detail::writeF32(output.data(), 184,
                     weather.convective_activity * 3000.0f);
    detail::writeF32(output.data(), 192,
                     weather.lightning_activity * 0.25f);
  }
  for (std::size_t index = 0; index < kMaximumCloudLayers; ++index) {
    if ((command.apply_mask & (kSkyApplyCloudLayer0 << index)) != 0U) {
      sky_control_detail::writeLayer(output.data(), index,
                                     value.cloud_layers[index]);
    }
  }
  if ((command.apply_mask & kSkyApplyAtmosphereOptics) != 0U) {
    sky_detail::writeRgb(output.data(), 196, value.optics.ground_albedo);
    detail::writeF32(output.data(), 208, value.optics.mie_anisotropy);
    detail::writeF32(output.data(), 212, value.optics.mie_scale_height_m);
    detail::writeF32(output.data(), 216,
                     value.optics.rayleigh_scale_height_m);
    sky_detail::writeRgb(output.data(), 432,
                         value.optics.rayleigh_scattering_per_m);
    sky_detail::writeRgb(output.data(), 444,
                         value.optics.mie_scattering_per_m);
    sky_detail::writeRgb(output.data(), 456,
                         value.optics.mie_absorption_per_m);
    sky_detail::writeRgb(output.data(), 468,
                         value.optics.ozone_absorption_per_m);
  }
  detail::writeU32(output.data(), kSkyControlCrcOffset,
                   crc32(output.data(), kSkyControlCrcOffset));
  return output;
}

inline bool deserializeSkyControl(const std::uint8_t *data, std::size_t size,
                                  SkyControlCommand &command) {
  if (size != kSkyControlPacketBytes || std::memcmp(data, "SKC1", 4) != 0 ||
      detail::readU16(data, 4) != kSkyControlProtocolVersion ||
      detail::readU16(data, 6) != kSkyControlPacketBytes ||
      detail::readU16(data, 26) != 0U || detail::readU32(data, 504) != 0U ||
      detail::readU32(data, kSkyControlCrcOffset) !=
          crc32(data, kSkyControlCrcOffset)) {
    return false;
  }

  command = {};
  command.session_id = detail::readU32(data, 8);
  command.sequence = detail::readU32(data, 12);
  command.client_time_seconds = detail::readF64(data, 16);
  command.opcode = static_cast<SkyControlOpcode>(data[24]);
  command.evolution_mode = static_cast<SkyEvolutionMode>(data[25]);
  command.transition_milliseconds = detail::readU32(data, 28);
  command.hold_milliseconds = detail::readU32(data, 32);
  const std::uint32_t raw_preset = detail::readU32(data, 36);
  if (raw_preset > static_cast<std::uint32_t>(WeatherPreset::Custom)) {
    return false;
  }
  command.preset = static_cast<WeatherPreset>(raw_preset);
  command.apply_mask = detail::readU64(data, 40);
  command.clear_mask = detail::readU64(data, 48);
  command.weather_seed = detail::readU32(data, 56);
  command.requested_cloud_field_mask = detail::readU16(data, 60);
  command.requested_state_hz = detail::readU16(data, 62);

  EnvironmentSnapshot &value = command.values;
  if ((command.apply_mask & kSkyApplyUtc) != 0U) {
    value.utc_unix_seconds = detail::readF64(data, 64);
  }
  if ((command.apply_mask & kSkyApplyLocation) != 0U) {
    value.location = {detail::readF64(data, 72), detail::readF64(data, 80),
                      detail::readF32(data, 88)};
  }
  if ((command.apply_mask & kSkyApplyTimeScale) != 0U) {
    value.time_scale = detail::readF32(data, 92);
  }
  if ((command.apply_mask & kSkyApplyDomain) != 0U) {
    const float horizontal_x = detail::readF32(data, 96);
    const float horizontal_y = detail::readF32(data, 100);
    if (horizontal_x != horizontal_y) {
      return false;
    }
    value.domain = {horizontal_x, detail::readF32(data, 104)};
  }
  WeatherState &weather = value.weather;
  if ((command.apply_mask & kSkyApplyThermodynamics) != 0U) {
    weather.surface_temperature_kelvin = detail::readF32(data, 108);
    weather.sea_level_pressure_pa = detail::readF32(data, 112);
    weather.relative_humidity = detail::readF32(data, 116);
  }
  if ((command.apply_mask & kSkyApplyVisibility) != 0U) {
    weather.visibility_m = detail::readF32(data, 120);
    weather.aerosol_optical_depth_550nm = detail::readF32(data, 124);
    weather.ozone_dobson_units = detail::readF32(data, 128);
  }
  if ((command.apply_mask & kSkyApplyWind) != 0U) {
    weather.wind_m_s = sky_detail::readVector(data, 132);
    const float wire_gust_speed =
        sky_detail::vectorLength(weather.wind_m_s) +
        sky_detail::vectorLength(sky_detail::readVector(data, 144));
    constexpr float wire_gust_tolerance_m_s = 0.0001f;
    weather.gust_speed_m_s =
        std::isfinite(wire_gust_speed) &&
                wire_gust_speed <= 200.0f + wire_gust_tolerance_m_s
            ? std::min(wire_gust_speed, 200.0f)
            : wire_gust_speed;
  }
  if ((command.apply_mask & kSkyApplyPrecipitation) != 0U) {
    weather.precipitation_rate_mm_h = detail::readF32(data, 160) * 3600.0f;
    weather.snow_fraction = detail::readF32(data, 168);
    weather.surface_wetness = detail::readF32(data, 176);
  }
  if ((command.apply_mask & kSkyApplyConvection) != 0U) {
    weather.convective_activity = detail::readF32(data, 184) / 3000.0f;
    weather.lightning_activity = detail::readF32(data, 192) / 0.25f;
  }
  for (std::size_t index = 0; index < kMaximumCloudLayers; ++index) {
    if ((command.apply_mask & (kSkyApplyCloudLayer0 << index)) != 0U) {
      const std::size_t offset = 224U + index * 32U;
      if (!sky_control_detail::rawLayerFieldsAreValid(data, index) ||
          data[offset + 28U] == 0U || data[offset + 28U] > 5U ||
          (data[offset + 29U] & ~0x07U) != 0U) {
        return false;
      }
      value.cloud_layers[index] =
          sky_control_detail::readLayer(data, index);
      value.cloud_layer_count = static_cast<std::uint8_t>(index + 1U);
    }
  }
  if ((command.apply_mask & kSkyApplyAtmosphereOptics) != 0U) {
    value.optics.ground_albedo = sky_detail::readRgb(data, 196);
    value.optics.mie_anisotropy = detail::readF32(data, 208);
    value.optics.mie_scale_height_m = detail::readF32(data, 212);
    value.optics.rayleigh_scale_height_m = detail::readF32(data, 216);
    value.optics.rayleigh_scattering_per_m = sky_detail::readRgb(data, 432);
    value.optics.mie_scattering_per_m = sky_detail::readRgb(data, 444);
    value.optics.mie_absorption_per_m = sky_detail::readRgb(data, 456);
    value.optics.ozone_absorption_per_m = sky_detail::readRgb(data, 468);
  }

  if (!sky_control_detail::validCommand(command) ||
      !sky_control_detail::unusedControlBytesAreZero(data,
                                                     command.apply_mask)) {
    return false;
  }
  if ((command.apply_mask & kSkyApplyPrecipitation) != 0U) {
    const float rain_fraction = detail::readF32(data, 164);
    if (!std::isfinite(rain_fraction) || rain_fraction < 0.0f ||
        rain_fraction > 1.0f ||
        std::abs(rain_fraction - (1.0f - command.values.weather.snow_fraction)) >
            0.0001f) {
      return false;
    }
  }
  return true;
}

class SkyControlReceiver {
public:
  SkyControlReceiver(const std::string &host, std::uint16_t port) {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
      throw std::runtime_error("Sky control WSAStartup failed");
    }
    platform_started_ = true;
#endif
    try {
      socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
      if (socket_ == invalidSocket()) {
        throw std::runtime_error("Could not create sky control socket");
      }
      const int receive_buffer_bytes = 256 * 1024;
      (void)setsockopt(socketHandle(), SOL_SOCKET, SO_RCVBUF,
                       reinterpret_cast<const char *>(&receive_buffer_bytes),
                       sizeof(receive_buffer_bytes));
      setNonBlocking();
      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_port = htons(port);
      if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        throw std::runtime_error("Sky control bind address must be IPv4: " +
                                 host);
      }
      if (::bind(socketHandle(), reinterpret_cast<const sockaddr *>(&address),
                 static_cast<socklen_type>(sizeof(address))) != 0) {
        throw std::runtime_error("Could not bind sky control socket");
      }
    } catch (...) {
      closeSocket();
      cleanupPlatform();
      throw;
    }
  }

  ~SkyControlReceiver() {
    closeSocket();
    cleanupPlatform();
  }

  SkyControlReceiver(const SkyControlReceiver &) = delete;
  SkyControlReceiver &operator=(const SkyControlReceiver &) = delete;

  void poll(std::size_t max_packets = 128) {
    expireSessions();
    std::array<std::uint8_t, 1024> packet{};
    max_packets = std::min<std::size_t>(max_packets, 1024U);
    for (std::size_t packet_index = 0; packet_index < max_packets;
         ++packet_index) {
      sockaddr_in source{};
      socklen_type source_size = static_cast<socklen_type>(sizeof(source));
      const int received = static_cast<int>(::recvfrom(
          socketHandle(), reinterpret_cast<char *>(packet.data()),
          static_cast<int>(packet.size()), 0,
          reinterpret_cast<sockaddr *>(&source), &source_size));
      if (received < 0) {
        if (wouldBlock()) {
          break;
        }
        if (messageTooLarge()) {
          ++rejected_packets_;
          continue;
        }
        throw std::runtime_error("Sky control receive failed");
      }
      SkyControlCommand command;
      if (!deserializeSkyControl(packet.data(),
                                 static_cast<std::size_t>(received), command)) {
        ++rejected_packets_;
        continue;
      }
      const SessionKey key{ntohl(source.sin_addr.s_addr),
                           ntohs(source.sin_port), command.session_id};
      if (!acceptSequence(key, command.sequence)) {
        ++stale_packets_;
        continue;
      }
      if (pending_commands_.size() >= 256U) {
        pending_commands_.pop_front();
      }
      pending_commands_.push_back(command);
      last_session_ = command.session_id;
      last_sequence_ = command.sequence;
      ++accepted_packets_;
    }
  }

  std::optional<SkyControlCommand> takeNextCommand() {
    if (pending_commands_.empty()) {
      return std::nullopt;
    }
    SkyControlCommand result = std::move(pending_commands_.front());
    pending_commands_.pop_front();
    return result;
  }

  std::uint32_t lastSession() const { return last_session_; }
  std::uint32_t lastSequence() const { return last_sequence_; }
  std::uint64_t acceptedPackets() const { return accepted_packets_; }
  std::uint64_t rejectedPackets() const { return rejected_packets_; }
  std::uint64_t stalePackets() const { return stale_packets_; }

private:
  using clock = std::chrono::steady_clock;

  struct SessionKey {
    std::uint32_t address = 0;
    std::uint16_t port = 0;
    std::uint32_t session = 0;
    bool operator==(const SessionKey &other) const {
      return address == other.address && port == other.port &&
             session == other.session;
    }
  };

  struct SessionHash {
    std::size_t operator()(const SessionKey &key) const {
      std::uint64_t value =
          (static_cast<std::uint64_t>(key.address) << 32U) ^
          (static_cast<std::uint64_t>(key.session) * 0x9e3779b97f4a7c15ULL) ^
          key.port;
      value ^= value >> 33U;
      value *= 0xff51afd7ed558ccdULL;
      return static_cast<std::size_t>(value ^ (value >> 33U));
    }
  };

  struct Session {
    std::uint32_t sequence = 0;
    bool has_sequence = false;
    clock::time_point last_seen = clock::now();
  };

#ifdef _WIN32
  using socket_type = SOCKET;
  using socklen_type = int;
  static constexpr socket_type invalidSocket() { return INVALID_SOCKET; }
  SOCKET socketHandle() const { return socket_; }
#else
  using socket_type = int;
  using socklen_type = socklen_t;
  static constexpr socket_type invalidSocket() { return -1; }
  int socketHandle() const { return socket_; }
#endif

  static bool newerSequence(std::uint32_t sequence, std::uint32_t previous) {
    const std::uint32_t difference = sequence - previous;
    return difference != 0U && difference < 0x80000000U;
  }

  bool acceptSequence(const SessionKey &key, std::uint32_t sequence) {
    auto iterator = sessions_.find(key);
    if (iterator == sessions_.end()) {
      expireSessions();
      if (sessions_.size() >= 32U) {
        return false;
      }
      iterator = sessions_.emplace(key, Session{}).first;
    }
    Session &session = iterator->second;
    if (session.has_sequence &&
        !newerSequence(sequence, session.sequence)) {
      return false;
    }
    session.sequence = sequence;
    session.has_sequence = true;
    session.last_seen = clock::now();
    return true;
  }

  void expireSessions() {
    const auto cutoff = clock::now() - std::chrono::minutes(10);
    for (auto iterator = sessions_.begin(); iterator != sessions_.end();) {
      if (iterator->second.last_seen < cutoff) {
        iterator = sessions_.erase(iterator);
      } else {
        ++iterator;
      }
    }
  }

  void setNonBlocking() {
#ifdef _WIN32
    u_long enabled = 1;
    if (ioctlsocket(socket_, FIONBIO, &enabled) != 0) {
      throw std::runtime_error("Could not make sky control socket non-blocking");
    }
#else
    const int flags = fcntl(socket_, F_GETFL, 0);
    if (flags < 0 || fcntl(socket_, F_SETFL, flags | O_NONBLOCK) != 0) {
      throw std::runtime_error("Could not make sky control socket non-blocking");
    }
#endif
  }

  static bool wouldBlock() {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
  }

  static bool messageTooLarge() {
#ifdef _WIN32
    return WSAGetLastError() == WSAEMSGSIZE;
#else
    return false;
#endif
  }

  void closeSocket() {
    if (socket_ == invalidSocket()) {
      return;
    }
#ifdef _WIN32
    closesocket(socket_);
#else
    close(socket_);
#endif
    socket_ = invalidSocket();
  }

  void cleanupPlatform() {
#ifdef _WIN32
    if (platform_started_) {
      WSACleanup();
      platform_started_ = false;
    }
#endif
  }

  socket_type socket_ = invalidSocket();
  std::unordered_map<SessionKey, Session, SessionHash> sessions_;
  std::deque<SkyControlCommand> pending_commands_;
#ifdef _WIN32
  bool platform_started_ = false;
#endif
  std::uint32_t last_session_ = 0;
  std::uint32_t last_sequence_ = 0;
  std::uint64_t accepted_packets_ = 0;
  std::uint64_t rejected_packets_ = 0;
  std::uint64_t stale_packets_ = 0;
};

} // namespace cloud
