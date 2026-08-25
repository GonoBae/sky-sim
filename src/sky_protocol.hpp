#pragma once

#include "astronomy.hpp"
#include "environment_types.hpp"
#include "protocol.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace cloud {

constexpr std::uint16_t kSkyStateProtocolVersion = 1;
constexpr std::size_t kSkyStatePacketBytes = 512;
constexpr std::size_t kSkyStateCrcOffset = 508;
constexpr std::uint16_t kSupportedCloudFieldMask = 0x001fU;

enum class SkyEvolutionMode : std::uint8_t {
  Natural = 1,
  Timeline = 2,
  Manual = 3,
  Replay = 4,
};

struct SkyStatePacketContext {
  std::uint32_t last_control_sequence = 0;
  std::uint32_t last_control_session = 0;
  std::uint32_t volume_frame_id = 0;
  double predicted_valid_time_seconds = 0.0;
  std::uint32_t weather_seed = 1;
  SkyEvolutionMode evolution_mode = SkyEvolutionMode::Manual;
  std::uint8_t last_control_result = 0;
  std::uint32_t weather_model_revision = 1;
  std::uint16_t supported_cloud_field_mask = kSupportedCloudFieldMask;
  std::uint16_t active_cloud_field_mask = kSupportedCloudFieldMask;
};

namespace sky_detail {

inline float vectorLength(EnvironmentVector value) {
  const double east = static_cast<double>(value.east);
  const double north = static_cast<double>(value.north);
  const double up = static_cast<double>(value.up);
  return static_cast<float>(
      std::sqrt(east * east + north * north + up * up));
}

inline std::uint8_t wireCloudKind(const CloudLayerState &layer,
                                 float top_altitude_amsl_m) {
  switch (layer.kind) {
  case CloudLayerKind::None:
    return 0;
  case CloudLayerKind::Stratiform:
  case CloudLayerKind::Fog:
    return 1;
  case CloudLayerKind::Convective:
    return top_altitude_amsl_m >= 8000.0f ? 3 : 2;
  case CloudLayerKind::Cirrus:
    return 4;
  }
  return 5;
}

inline CloudLayerKind internalCloudKind(std::uint8_t value,
                                        float base_altitude_agl_m) {
  switch (value) {
  case 0:
    return CloudLayerKind::None;
  case 1:
    return base_altitude_agl_m <= 5.0f ? CloudLayerKind::Fog
                                      : CloudLayerKind::Stratiform;
  case 2:
  case 3:
    return CloudLayerKind::Convective;
  case 4:
    return CloudLayerKind::Cirrus;
  case 5:
    return CloudLayerKind::Stratiform;
  default:
    return CloudLayerKind::None;
  }
}

inline void writeVector(std::uint8_t *output, std::size_t offset,
                        EnvironmentVector value) {
  detail::writeF32(output, offset, value.east);
  detail::writeF32(output, offset + 4U, value.north);
  detail::writeF32(output, offset + 8U, value.up);
}

inline EnvironmentVector readVector(const std::uint8_t *input,
                                    std::size_t offset) {
  return {detail::readF32(input, offset), detail::readF32(input, offset + 4U),
          detail::readF32(input, offset + 8U)};
}

inline void writeRgb(std::uint8_t *output, std::size_t offset,
                     const std::array<float, 3> &value) {
  detail::writeF32(output, offset, value[0]);
  detail::writeF32(output, offset + 4U, value[1]);
  detail::writeF32(output, offset + 8U, value[2]);
}

inline std::array<float, 3> readRgb(const std::uint8_t *input,
                                   std::size_t offset) {
  return {detail::readF32(input, offset), detail::readF32(input, offset + 4U),
          detail::readF32(input, offset + 8U)};
}

inline void writeCommonEnvironment(std::uint8_t *output,
                                   const EnvironmentSnapshot &snapshot) {
  const WeatherState &weather = snapshot.weather;
  const AtmosphereOptics &optics = snapshot.optics;
  detail::writeF64(output, 64, snapshot.utc_unix_seconds);
  detail::writeF64(output, 72, snapshot.location.latitude_degrees);
  detail::writeF64(output, 80, snapshot.location.longitude_degrees);
  detail::writeF32(output, 88,
                   static_cast<float>(snapshot.location.elevation_m));
  detail::writeF32(output, 92, snapshot.time_scale);
  detail::writeF32(output, 96, snapshot.domain.horizontal_extent_m);
  detail::writeF32(output, 100, snapshot.domain.horizontal_extent_m);
  detail::writeF32(output, 104, snapshot.domain.vertical_extent_m);
  detail::writeF32(output, 108, weather.surface_temperature_kelvin);
  detail::writeF32(output, 112, weather.sea_level_pressure_pa);
  detail::writeF32(output, 116, weather.relative_humidity);
  detail::writeF32(output, 120, weather.visibility_m);
  detail::writeF32(output, 124, weather.aerosol_optical_depth_550nm);
  detail::writeF32(output, 128, weather.ozone_dobson_units);
  writeVector(output, 132, weather.wind_m_s);
  const float mean_speed = vectorLength(weather.wind_m_s);
  const float gust_delta = std::max(0.0f, weather.gust_speed_m_s - mean_speed);
  const float inverse_speed = mean_speed > 0.0001f ? 1.0f / mean_speed : 0.0f;
  const EnvironmentVector gust =
      mean_speed > 0.0001f
          ? EnvironmentVector{
                weather.wind_m_s.east * inverse_speed * gust_delta,
                weather.wind_m_s.north * inverse_speed * gust_delta,
                weather.wind_m_s.up * inverse_speed * gust_delta}
          : EnvironmentVector{gust_delta, 0.0f, 0.0f};
  writeVector(output, 144, gust);
  detail::writeF32(output, 156,
                   400.0f + 1800.0f * weather.convective_activity);
  const float precipitation_flux = weather.precipitation_rate_mm_h / 3600.0f;
  detail::writeF32(output, 160, precipitation_flux);
  detail::writeF32(output, 164,
                   weather.precipitation_rate_mm_h > 0.0f
                       ? 1.0f - weather.snow_fraction
                       : 0.0f);
  detail::writeF32(output, 168,
                   weather.precipitation_rate_mm_h > 0.0f
                       ? weather.snow_fraction
                       : 0.0f);
  detail::writeF32(output, 172, 0.0f); // hail fraction, reserved for ice phase
  detail::writeF32(output, 176, weather.surface_wetness);
  detail::writeF32(output, 180, 0.0f); // snow water equivalent
  detail::writeF32(output, 184,
                   3000.0f * weather.convective_activity);
  detail::writeF32(output, 188,
                   120.0f * (1.0f - weather.convective_activity));
  detail::writeF32(output, 192, weather.lightning_activity * 0.25f);
  writeRgb(output, 196, optics.ground_albedo);
  detail::writeF32(output, 208, optics.mie_anisotropy);
  detail::writeF32(output, 212, optics.mie_scale_height_m);
  detail::writeF32(output, 216, optics.rayleigh_scale_height_m);
  detail::writeF32(output, 220, -0.0065f);

  for (std::size_t index = 0; index < kMaximumCloudLayers; ++index) {
    const std::size_t offset = 224U + index * 32U;
    if (index >= snapshot.cloud_layer_count) {
      continue;
    }
    const CloudLayerState &layer = snapshot.cloud_layers[index];
    const float base_amsl =
        static_cast<float>(snapshot.location.elevation_m) +
        layer.base_altitude_m;
    const float top_amsl =
        static_cast<float>(snapshot.location.elevation_m) +
        layer.top_altitude_m;
    const float thickness = std::max(1.0f, layer.top_altitude_m -
                                               layer.base_altitude_m);
    const float condensate =
        std::clamp(layer.optical_depth / (100.0f * thickness), 0.0f, 0.01f);
    detail::writeF32(output, offset, base_amsl);
    detail::writeF32(output, offset + 4U, top_amsl);
    detail::writeF32(output, offset + 8U, layer.coverage);
    detail::writeF32(output, offset + 12U, condensate);
    detail::writeF32(output, offset + 16U, 1.0f - layer.liquid_fraction);
    detail::writeF32(output, offset + 20U,
                     layer.precipitation_rate_mm_h / 3600.0f);
    detail::writeF32(output, offset + 24U,
                     0.25f + 4.0f * layer.convective_activity);
    output[offset + 28U] = wireCloudKind(layer, top_amsl);
    output[offset + 29U] = static_cast<std::uint8_t>(
        (layer.convective_activity > 0.35f ? 1U : 0U) |
        (layer.precipitation_rate_mm_h > 0.001f ? 2U : 0U) |
        (weather.lightning_activity > 0.02f ? 4U : 0U));
  }

  const CelestialState &celestial = snapshot.celestial;
  writeVector(output, 352, celestial.sun_direction);
  detail::writeF32(
      output, 364,
      celestial.sun_angular_radius_degrees *
          static_cast<float>(astronomy::kDegreesToRadians));
  detail::writeF32(output, 368, celestial.sun_irradiance_w_m2);
  detail::writeF32(output, 372, celestial.sun_direct_illuminance_lux);
  writeVector(output, 376, celestial.moon_direction);
  detail::writeF32(
      output, 388,
      celestial.moon_angular_radius_degrees *
          static_cast<float>(astronomy::kDegreesToRadians));
  detail::writeF32(output, 392, celestial.moon_illuminated_fraction);
  detail::writeF32(output, 396, celestial.moon_illuminance_lux);
  const auto star_rotation = astronomy::celestialToEnuQuaternion(
      snapshot.utc_unix_seconds, snapshot.location);
  detail::writeF32(output, 400, star_rotation[0]);
  detail::writeF32(output, 404, star_rotation[1]);
  detail::writeF32(output, 408, star_rotation[2]);
  detail::writeF32(output, 412, star_rotation[3]);
  detail::writeF32(
      output, 416,
      static_cast<float>(astronomy::localSiderealAngleRadians(
          snapshot.utc_unix_seconds, snapshot.location.longitude_degrees)));
  detail::writeF32(output, 420, 0.0f); // geomagnetic Kp: external input later
  detail::writeF32(output, 424, 0.0f); // aurora intensity
  detail::writeF32(output, 428, 0.04f * celestial.star_visibility);
  writeRgb(output, 432, optics.rayleigh_scattering_per_m);
  writeRgb(output, 444, optics.mie_scattering_per_m);
  writeRgb(output, 456, optics.mie_absorption_per_m);
  writeRgb(output, 468, optics.ozone_absorption_per_m);
  const float daylight = std::clamp(
      (celestial.sun_color_temperature_kelvin - 1900.0f) / 4600.0f, 0.0f,
      1.0f);
  writeRgb(output, 480,
           {1.0f, 0.72f + 0.28f * daylight,
            0.43f + 0.57f * daylight});
  writeRgb(output, 492, {0.78f, 0.84f, 1.0f});
}

inline bool finiteCommonEnvironment(const std::uint8_t *data) {
  const std::size_t float_offsets[] = {
      88,  92,  96,  100, 104, 108, 112, 116, 120, 124, 128,
      132, 136, 140, 144, 148, 152, 156, 160, 164, 168, 172,
      176, 180, 184, 188, 192, 196, 200, 204, 208, 212, 216,
      220, 352, 356, 360, 364, 368, 372, 376, 380, 384, 388,
      392, 396, 400, 404, 408, 412, 416, 420, 424, 428, 432,
      436, 440, 444, 448, 452, 456, 460, 464, 468, 472, 476,
      480, 484, 488, 492, 496, 500,
  };
  if (!std::isfinite(detail::readF64(data, 64)) ||
      !std::isfinite(detail::readF64(data, 72)) ||
      !std::isfinite(detail::readF64(data, 80))) {
    return false;
  }
  for (const std::size_t offset : float_offsets) {
    if (!std::isfinite(detail::readF32(data, offset))) {
      return false;
    }
  }
  for (std::size_t index = 0; index < kMaximumCloudLayers; ++index) {
    const std::size_t offset = 224U + index * 32U;
    for (std::size_t value_offset = 0; value_offset <= 24U;
         value_offset += 4U) {
      if (!std::isfinite(detail::readF32(data, offset + value_offset))) {
        return false;
      }
    }
  }
  return true;
}

} // namespace sky_detail

inline std::array<std::uint8_t, kSkyStatePacketBytes>
serializeSkyState(const EnvironmentSnapshot &snapshot,
                  const SkyStatePacketContext &context) {
  std::array<std::uint8_t, kSkyStatePacketBytes> output{};
  std::memcpy(output.data(), "SKS1", 4);
  detail::writeU16(output.data(), 4, kSkyStateProtocolVersion);
  detail::writeU16(output.data(), 6,
                   static_cast<std::uint16_t>(kSkyStatePacketBytes));
  detail::writeU32(output.data(), 8, snapshot.sequence);
  detail::writeU32(output.data(), 12, context.last_control_sequence);
  detail::writeU32(output.data(), 16, context.last_control_session);
  detail::writeU32(output.data(), 20, context.volume_frame_id);
  detail::writeF64(output.data(), 24, snapshot.fluid_time_seconds);
  detail::writeF64(
      output.data(), 32,
      context.predicted_valid_time_seconds > 0.0
          ? context.predicted_valid_time_seconds
          : snapshot.fluid_time_seconds);
  detail::writeU32(output.data(), 40, snapshot.flags);
  detail::writeU32(output.data(), 44, context.weather_seed);
  output[48] = static_cast<std::uint8_t>(context.evolution_mode);
  output[49] = context.last_control_result;
  output[50] = snapshot.cloud_layer_count;
  detail::writeU32(output.data(), 52, context.weather_model_revision);
  detail::writeU32(output.data(), 56, snapshot.lightning_event_id);
  detail::writeU16(output.data(), 60,
                   context.supported_cloud_field_mask);
  detail::writeU16(output.data(), 62, context.active_cloud_field_mask);
  sky_detail::writeCommonEnvironment(output.data(), snapshot);
  detail::writeU32(output.data(), kSkyStateCrcOffset,
                   crc32(output.data(), kSkyStateCrcOffset));
  return output;
}

inline bool deserializeSkyState(const std::uint8_t *data, std::size_t size,
                                EnvironmentSnapshot &snapshot,
                                SkyStatePacketContext &context) {
  if (size != kSkyStatePacketBytes || std::memcmp(data, "SKS1", 4) != 0 ||
      detail::readU16(data, 4) != kSkyStateProtocolVersion ||
      detail::readU16(data, 6) != kSkyStatePacketBytes || data[51] != 0U ||
      detail::readU32(data, 504) != 0U ||
      detail::readU32(data, kSkyStateCrcOffset) !=
          crc32(data, kSkyStateCrcOffset) ||
      !std::isfinite(detail::readF64(data, 24)) ||
      !std::isfinite(detail::readF64(data, 32)) ||
      !sky_detail::finiteCommonEnvironment(data)) {
    return false;
  }

  const std::uint8_t evolution = data[48];
  const std::uint8_t control_result = data[49];
  const std::uint8_t layer_count = data[50];
  const std::uint32_t flags = detail::readU32(data, 40);
  const std::uint16_t supported_fields = detail::readU16(data, 60);
  const std::uint16_t active_fields = detail::readU16(data, 62);
  const double fluid_time = detail::readF64(data, 24);
  const double predicted_time = detail::readF64(data, 32);
  const double utc = detail::readF64(data, 64);
  const double latitude = detail::readF64(data, 72);
  const double longitude = detail::readF64(data, 80);
  const float elevation = detail::readF32(data, 88);
  const float time_scale = detail::readF32(data, 92);
  const float horizontal_x = detail::readF32(data, 96);
  const float horizontal_y = detail::readF32(data, 100);
  const float vertical = detail::readF32(data, 104);
  const float temperature = detail::readF32(data, 108);
  const float pressure = detail::readF32(data, 112);
  const float humidity = detail::readF32(data, 116);
  const float visibility = detail::readF32(data, 120);
  const float aerosol = detail::readF32(data, 124);
  const float ozone = detail::readF32(data, 128);
  const EnvironmentVector wind = sky_detail::readVector(data, 132);
  const EnvironmentVector gust_delta = sky_detail::readVector(data, 144);
  const float precipitation_flux = detail::readF32(data, 160);
  const float rain_fraction = detail::readF32(data, 164);
  const float snow_fraction = detail::readF32(data, 168);
  const float hail_fraction = detail::readF32(data, 172);
  const float surface_wetness = detail::readF32(data, 176);
  const float snow_water_equivalent = detail::readF32(data, 180);
  const float cape = detail::readF32(data, 184);
  const float cin = detail::readF32(data, 188);
  const float lightning_rate = detail::readF32(data, 192);
  const EnvironmentVector sun = sky_detail::readVector(data, 352);
  const EnvironmentVector moon = sky_detail::readVector(data, 376);
  const float moon_illuminated_fraction = detail::readF32(data, 392);
  const float aurora_intensity = detail::readF32(data, 424);
  const std::array<float, 4> celestial_rotation{
      detail::readF32(data, 400), detail::readF32(data, 404),
      detail::readF32(data, 408), detail::readF32(data, 412)};
  double rotation_length_squared = 0.0;
  for (const float component : celestial_rotation) {
    const double value = static_cast<double>(component);
    rotation_length_squared += value * value;
  }
  const float rotation_length =
      static_cast<float>(std::sqrt(rotation_length_squared));
  if (evolution < static_cast<std::uint8_t>(SkyEvolutionMode::Natural) ||
      evolution > static_cast<std::uint8_t>(SkyEvolutionMode::Replay) ||
      control_result > 2U || layer_count > kMaximumCloudLayers ||
      (flags & ~kKnownSkyFlags) != 0U ||
      (supported_fields & ~kSupportedCloudFieldMask) != 0U ||
      (active_fields & ~supported_fields) != 0U || fluid_time < 0.0 ||
      predicted_time + 1.0e-9 < fluid_time || utc < -2208988800.0 ||
      utc > 4133980800.0 || latitude < -90.0 ||
      latitude > 90.0 || longitude < -180.0 || longitude > 180.0 ||
      elevation < -500.0f || elevation > 100000.0f ||
      time_scale < -86400.0f || time_scale > 86400.0f ||
      horizontal_x < 100.0f || horizontal_x > 2000000.0f ||
      horizontal_y < 100.0f || horizontal_y > 2000000.0f ||
      std::abs(horizontal_x - horizontal_y) > 0.01f || vertical < 100.0f ||
      vertical > 100000.0f ||
      temperature < 150.0f || temperature > 350.0f || pressure < 10000.0f ||
      pressure > 120000.0f || humidity < 0.0f || humidity > 1.0f ||
      visibility < 1.0f || visibility > 200000.0f || aerosol < 0.0f ||
      aerosol > 3.0f || ozone < 100.0f || ozone > 600.0f ||
      sky_detail::vectorLength(wind) > 150.0f ||
      sky_detail::vectorLength(gust_delta) > 200.0f ||
      precipitation_flux < 0.0f || precipitation_flux > 300.0f / 3600.0f ||
      rain_fraction < 0.0f || rain_fraction > 1.0f ||
      snow_fraction < 0.0f || snow_fraction > 1.0f ||
      hail_fraction < 0.0f || hail_fraction > 1.0f ||
      surface_wetness < 0.0f || surface_wetness > 1.0f ||
      snow_water_equivalent < 0.0f || cape < 0.0f || cape > 3000.0f ||
      cin < 0.0f || cin > 3000.0f || lightning_rate < 0.0f ||
      lightning_rate > 10.0f || moon_illuminated_fraction < 0.0f ||
      moon_illuminated_fraction > 1.0f || aurora_intensity < 0.0f ||
      aurora_intensity > 1.0f || rotation_length < 0.99f ||
      rotation_length > 1.01f ||
      sky_detail::vectorLength(sun) < 0.99f ||
      sky_detail::vectorLength(sun) > 1.01f ||
      sky_detail::vectorLength(moon) < 0.99f ||
      sky_detail::vectorLength(moon) > 1.01f) {
    return false;
  }
  const double phase_fraction_sum =
      static_cast<double>(rain_fraction) +
      static_cast<double>(snow_fraction) +
      static_cast<double>(hail_fraction);
  if ((precipitation_flux > 0.0f &&
       std::abs(phase_fraction_sum - 1.0) > 0.0001) ||
      (precipitation_flux == 0.0f && phase_fraction_sum > 0.0001)) {
    return false;
  }
  for (std::size_t index = 0; index < kMaximumCloudLayers; ++index) {
    const std::size_t offset = 224U + index * 32U;
    if (data[offset + 30U] != 0U || data[offset + 31U] != 0U ||
        data[offset + 28U] > 5U || (data[offset + 29U] & ~0x07U) != 0U) {
      return false;
    }
    if (index >= layer_count) {
      for (std::size_t byte = 0; byte < 30U; ++byte) {
        if (data[offset + byte] != 0U) {
          return false;
        }
      }
      continue;
    }
    const float base = detail::readF32(data, offset);
    const float top = detail::readF32(data, offset + 4U);
    const float coverage = detail::readF32(data, offset + 8U);
    const float condensate = detail::readF32(data, offset + 12U);
    const float ice_fraction = detail::readF32(data, offset + 16U);
    const float precipitation_flux = detail::readF32(data, offset + 20U);
    const float turbulence = detail::readF32(data, offset + 24U);
    if (data[offset + 28U] == 0U || base < elevation || top <= base ||
        top > 200000.0f || coverage < 0.0f || coverage > 1.0f ||
        condensate < 0.0f || condensate > 0.01f || ice_fraction < 0.0f ||
        ice_fraction > 1.0f || precipitation_flux < 0.0f ||
        precipitation_flux > 300.0f / 3600.0f || turbulence < 0.25f ||
        turbulence > 4.25f) {
      return false;
    }
  }

  snapshot = {};
  snapshot.sequence = detail::readU32(data, 8);
  snapshot.flags = detail::readU32(data, 40);
  snapshot.lightning_event_id = detail::readU32(data, 56);
  snapshot.fluid_time_seconds = detail::readF64(data, 24);
  snapshot.utc_unix_seconds = detail::readF64(data, 64);
  snapshot.location = {latitude, longitude, elevation};
  snapshot.time_scale = time_scale;
  snapshot.domain = {horizontal_x, vertical};
  snapshot.cloud_layer_count = layer_count;
  WeatherState &weather = snapshot.weather;
  weather.surface_temperature_kelvin = temperature;
  weather.sea_level_pressure_pa = pressure;
  weather.relative_humidity = humidity;
  weather.visibility_m = visibility;
  weather.aerosol_optical_depth_550nm = aerosol;
  weather.ozone_dobson_units = ozone;
  weather.wind_m_s = sky_detail::readVector(data, 132);
  const EnvironmentVector gust = sky_detail::readVector(data, 144);
  weather.gust_speed_m_s =
      sky_detail::vectorLength(weather.wind_m_s) +
      sky_detail::vectorLength(gust);
  weather.precipitation_rate_mm_h = detail::readF32(data, 160) * 3600.0f;
  weather.snow_fraction = detail::readF32(data, 168);
  weather.surface_wetness = detail::readF32(data, 176);
  weather.convective_activity =
      std::clamp(detail::readF32(data, 184) / 3000.0f, 0.0f, 1.0f);
  weather.lightning_activity =
      std::clamp(detail::readF32(data, 192) / 0.25f, 0.0f, 1.0f);
  weather.precipitation_type =
      weather.precipitation_rate_mm_h <= 0.001f
          ? PrecipitationType::None
          : (weather.snow_fraction >= 0.85f
                 ? PrecipitationType::Snow
                 : (weather.snow_fraction <= 0.15f
                        ? PrecipitationType::Rain
                        : PrecipitationType::Mixed));
  weather.fog_extinction_per_m =
      visibility < 2000.0f && humidity > 0.90f ? 3.912f / visibility : 0.0f;
  const float temperature_celsius = temperature - 273.15f;
  const float gamma =
      std::log(std::max(0.001f, humidity)) +
      17.67f * temperature_celsius / (temperature_celsius + 243.5f);
  weather.dew_point_kelvin =
      273.15f + 243.5f * gamma / (17.67f - gamma);

  for (std::size_t index = 0; index < layer_count; ++index) {
    const std::size_t offset = 224U + index * 32U;
    CloudLayerState &layer = snapshot.cloud_layers[index];
    layer.base_altitude_m =
        detail::readF32(data, offset) - static_cast<float>(elevation);
    layer.top_altitude_m =
        detail::readF32(data, offset + 4U) - static_cast<float>(elevation);
    layer.coverage = detail::readF32(data, offset + 8U);
    const float condensate = detail::readF32(data, offset + 12U);
    layer.optical_depth =
        condensate * 100.0f *
        std::max(1.0f, layer.top_altitude_m - layer.base_altitude_m);
    layer.liquid_fraction = 1.0f - detail::readF32(data, offset + 16U);
    layer.precipitation_rate_mm_h =
        detail::readF32(data, offset + 20U) * 3600.0f;
    layer.convective_activity = std::clamp(
        (detail::readF32(data, offset + 24U) - 0.25f) / 4.0f, 0.0f,
        1.0f);
    layer.kind =
        sky_detail::internalCloudKind(data[offset + 28U],
                                      layer.base_altitude_m);
  }

  CelestialState &celestial = snapshot.celestial;
  celestial.sun_direction = sun;
  celestial.sun_geometric_elevation_degrees = static_cast<float>(
      std::asin(std::clamp(sun.up, -1.0f, 1.0f)) *
      astronomy::kRadiansToDegrees);
  celestial.sun_angular_radius_degrees =
      detail::readF32(data, 364) *
      static_cast<float>(astronomy::kRadiansToDegrees);
  celestial.sun_irradiance_w_m2 = detail::readF32(data, 368);
  celestial.sun_direct_illuminance_lux = detail::readF32(data, 372);
  celestial.moon_direction = moon;
  celestial.moon_geometric_elevation_degrees = static_cast<float>(
      std::asin(std::clamp(moon.up, -1.0f, 1.0f)) *
      astronomy::kRadiansToDegrees);
  celestial.moon_angular_radius_degrees =
      detail::readF32(data, 388) *
      static_cast<float>(astronomy::kRadiansToDegrees);
  celestial.moon_illuminated_fraction = moon_illuminated_fraction;
  celestial.moon_illuminance_lux = detail::readF32(data, 396);

  AtmosphereOptics &optics = snapshot.optics;
  optics.ground_albedo = sky_detail::readRgb(data, 196);
  optics.mie_anisotropy = detail::readF32(data, 208);
  optics.mie_scale_height_m = detail::readF32(data, 212);
  optics.rayleigh_scale_height_m = detail::readF32(data, 216);
  optics.rayleigh_scattering_per_m = sky_detail::readRgb(data, 432);
  optics.mie_scattering_per_m = sky_detail::readRgb(data, 444);
  optics.mie_absorption_per_m = sky_detail::readRgb(data, 456);
  optics.ozone_absorption_per_m = sky_detail::readRgb(data, 468);
  optics.aerosol_optical_depth_550nm = aerosol;
  optics.ozone_dobson_units = ozone;

  context.last_control_sequence = detail::readU32(data, 12);
  context.last_control_session = detail::readU32(data, 16);
  context.volume_frame_id = detail::readU32(data, 20);
  context.predicted_valid_time_seconds = detail::readF64(data, 32);
  context.weather_seed = detail::readU32(data, 44);
  context.evolution_mode = static_cast<SkyEvolutionMode>(evolution);
  context.last_control_result = data[49];
  context.weather_model_revision = detail::readU32(data, 52);
  context.supported_cloud_field_mask = detail::readU16(data, 60);
  context.active_cloud_field_mask = detail::readU16(data, 62);
  return true;
}

} // namespace cloud
