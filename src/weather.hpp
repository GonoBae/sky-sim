#pragma once

#include "astronomy.hpp"
#include "environment_types.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace cloud {

inline const char *weatherPresetName(WeatherPreset preset) {
  switch (preset) {
  case WeatherPreset::Natural:
    return "natural";
  case WeatherPreset::Clear:
    return "clear";
  case WeatherPreset::Cumulus:
    return "cumulus";
  case WeatherPreset::Overcast:
    return "overcast";
  case WeatherPreset::Rain:
    return "rain";
  case WeatherPreset::Storm:
    return "storm";
  case WeatherPreset::Snow:
    return "snow";
  case WeatherPreset::Fog:
    return "fog";
  case WeatherPreset::Custom:
    return "custom";
  }
  return "unknown";
}

inline WeatherPreset parseWeatherPreset(const std::string &name) {
  if (name == "natural" || name == "auto") {
    return WeatherPreset::Natural;
  }
  if (name == "clear") {
    return WeatherPreset::Clear;
  }
  if (name == "cumulus" || name == "fair" || name == "partly-cloudy") {
    return WeatherPreset::Cumulus;
  }
  if (name == "overcast") {
    return WeatherPreset::Overcast;
  }
  if (name == "rain") {
    return WeatherPreset::Rain;
  }
  if (name == "storm" || name == "thunderstorm") {
    return WeatherPreset::Storm;
  }
  if (name == "snow") {
    return WeatherPreset::Snow;
  }
  if (name == "fog") {
    return WeatherPreset::Fog;
  }
  throw std::invalid_argument("Unknown weather preset: " + name);
}

struct WeatherProfile {
  WeatherState weather{};
  std::array<CloudLayerState, kMaximumCloudLayers> cloud_layers{};
  std::uint8_t cloud_layer_count = 0;
};

namespace weather_detail {

inline float smoothstep(float value) {
  value = std::clamp(value, 0.0f, 1.0f);
  return value * value * (3.0f - 2.0f * value);
}

inline float mix(float a, float b, float amount) {
  return a + (b - a) * amount;
}

inline EnvironmentVector mix(EnvironmentVector a, EnvironmentVector b,
                             float amount) {
  return {mix(a.east, b.east, amount), mix(a.north, b.north, amount),
          mix(a.up, b.up, amount)};
}

inline float dewPointKelvin(float temperature_kelvin,
                            float relative_humidity) {
  const float temperature_celsius = temperature_kelvin - 273.15f;
  const float humidity = std::clamp(relative_humidity, 0.001f, 1.0f);
  const float gamma = std::log(humidity) +
                      17.67f * temperature_celsius /
                          (temperature_celsius + 243.5f);
  return 273.15f + 243.5f * gamma / (17.67f - gamma);
}

inline PrecipitationType precipitationType(float rate_mm_h,
                                           float snow_fraction) {
  if (rate_mm_h <= 0.001f) {
    return PrecipitationType::None;
  }
  if (snow_fraction >= 0.85f) {
    return PrecipitationType::Snow;
  }
  if (snow_fraction <= 0.15f) {
    return PrecipitationType::Rain;
  }
  return PrecipitationType::Mixed;
}

inline void finishProfile(WeatherProfile &profile) {
  WeatherState &weather = profile.weather;
  weather.surface_temperature_kelvin =
      std::clamp(weather.surface_temperature_kelvin, 203.15f, 333.15f);
  weather.sea_level_pressure_pa =
      std::clamp(weather.sea_level_pressure_pa, 80000.0f, 108000.0f);
  weather.relative_humidity =
      std::clamp(weather.relative_humidity, 0.01f, 1.0f);
  weather.visibility_m = std::clamp(weather.visibility_m, 25.0f, 200000.0f);
  weather.gust_speed_m_s = std::max(
      weather.gust_speed_m_s,
      std::sqrt(weather.wind_m_s.east * weather.wind_m_s.east +
                weather.wind_m_s.north * weather.wind_m_s.north +
                weather.wind_m_s.up * weather.wind_m_s.up));
  weather.precipitation_rate_mm_h =
      std::clamp(weather.precipitation_rate_mm_h, 0.0f, 300.0f);
  weather.snow_fraction = std::clamp(weather.snow_fraction, 0.0f, 1.0f);
  weather.convective_activity =
      std::clamp(weather.convective_activity, 0.0f, 1.0f);
  weather.lightning_activity =
      std::clamp(weather.lightning_activity, 0.0f, 1.0f);
  weather.aerosol_optical_depth_550nm =
      std::clamp(weather.aerosol_optical_depth_550nm, 0.005f, 3.0f);
  weather.ozone_dobson_units =
      std::clamp(weather.ozone_dobson_units, 100.0f, 600.0f);
  weather.dew_point_kelvin =
      dewPointKelvin(weather.surface_temperature_kelvin,
                     weather.relative_humidity);
  weather.precipitation_type =
      precipitationType(weather.precipitation_rate_mm_h,
                        weather.snow_fraction);
  const bool low_visibility_fog = weather.visibility_m < 2000.0f &&
                                  weather.relative_humidity > 0.90f;
  weather.fog_extinction_per_m =
      low_visibility_fog ? 3.912f / weather.visibility_m : 0.0f;

  const std::size_t requested_layer_count = std::min<std::size_t>(
      profile.cloud_layer_count, kMaximumCloudLayers);
  std::array<CloudLayerState, kMaximumCloudLayers> compact_layers{};
  std::size_t compact_count = 0;
  for (std::size_t index = 0; index < requested_layer_count; ++index) {
    CloudLayerState layer = profile.cloud_layers[index];
    if (layer.kind == CloudLayerKind::None) {
      continue;
    }
    layer.base_altitude_m = std::max(0.0f, layer.base_altitude_m);
    layer.top_altitude_m =
        std::max(layer.base_altitude_m + 1.0f, layer.top_altitude_m);
    layer.coverage = std::clamp(layer.coverage, 0.0f, 1.0f);
    layer.optical_depth = std::max(0.0f, layer.optical_depth);
    layer.liquid_fraction = std::clamp(layer.liquid_fraction, 0.0f, 1.0f);
    layer.precipitation_rate_mm_h =
        std::max(0.0f, layer.precipitation_rate_mm_h);
    layer.convective_activity =
        std::clamp(layer.convective_activity, 0.0f, 1.0f);
    compact_layers[compact_count++] = layer;
  }
  profile.cloud_layers = compact_layers;
  profile.cloud_layer_count = static_cast<std::uint8_t>(compact_count);
}

inline CloudLayerState layer(CloudLayerKind kind, float base_m, float top_m,
                             float coverage, float optical_depth,
                             float liquid_fraction, float precipitation,
                             float convection) {
  CloudLayerState result;
  result.kind = kind;
  result.base_altitude_m = base_m;
  result.top_altitude_m = top_m;
  result.coverage = coverage;
  result.optical_depth = optical_depth;
  result.liquid_fraction = liquid_fraction;
  result.precipitation_rate_mm_h = precipitation;
  result.convective_activity = convection;
  return result;
}

inline WeatherProfile fixedProfile(WeatherPreset preset) {
  WeatherProfile result;
  WeatherState &value = result.weather;
  value.preset = preset;
  switch (preset) {
  case WeatherPreset::Clear:
    value.surface_temperature_kelvin = 296.15f;
    value.sea_level_pressure_pa = 102000.0f;
    value.relative_humidity = 0.38f;
    value.visibility_m = 65000.0f;
    value.wind_m_s = {2.5f, 1.0f, 0.0f};
    value.gust_speed_m_s = 4.0f;
    value.convective_activity = 0.10f;
    value.aerosol_optical_depth_550nm = 0.07f;
    result.cloud_layers[0] =
        layer(CloudLayerKind::Cirrus, 8000.0f, 10500.0f, 0.08f, 0.4f,
              0.05f, 0.0f, 0.0f);
    result.cloud_layer_count = 1;
    break;
  case WeatherPreset::Cumulus:
    value.surface_temperature_kelvin = 294.15f;
    value.sea_level_pressure_pa = 101500.0f;
    value.relative_humidity = 0.58f;
    value.visibility_m = 40000.0f;
    value.wind_m_s = {4.5f, 1.5f, 0.0f};
    value.gust_speed_m_s = 7.0f;
    value.convective_activity = 0.45f;
    value.aerosol_optical_depth_550nm = 0.10f;
    result.cloud_layers[0] =
        layer(CloudLayerKind::Convective, 1200.0f, 3600.0f, 0.42f, 7.0f,
              0.95f, 0.0f, 0.55f);
    result.cloud_layer_count = 1;
    break;
  case WeatherPreset::Overcast:
    value.surface_temperature_kelvin = 289.15f;
    value.sea_level_pressure_pa = 100900.0f;
    value.relative_humidity = 0.84f;
    value.visibility_m = 16000.0f;
    value.wind_m_s = {6.0f, 2.5f, 0.0f};
    value.gust_speed_m_s = 9.0f;
    value.precipitation_rate_mm_h = 0.15f;
    value.convective_activity = 0.25f;
    value.aerosol_optical_depth_550nm = 0.16f;
    result.cloud_layers[0] =
        layer(CloudLayerKind::Stratiform, 650.0f, 4300.0f, 0.96f, 32.0f,
              0.90f, 0.15f, 0.20f);
    result.cloud_layer_count = 1;
    break;
  case WeatherPreset::Rain:
    value.surface_temperature_kelvin = 287.15f;
    value.sea_level_pressure_pa = 100200.0f;
    value.relative_humidity = 0.94f;
    value.visibility_m = 6500.0f;
    value.wind_m_s = {8.0f, 3.0f, -0.05f};
    value.gust_speed_m_s = 13.0f;
    value.precipitation_rate_mm_h = 8.0f;
    value.convective_activity = 0.55f;
    value.aerosol_optical_depth_550nm = 0.22f;
    result.cloud_layers[0] =
        layer(CloudLayerKind::Stratiform, 450.0f, 6200.0f, 0.99f, 58.0f,
              0.85f, 8.0f, 0.45f);
    result.cloud_layer_count = 1;
    break;
  case WeatherPreset::Storm:
    value.surface_temperature_kelvin = 293.15f;
    value.sea_level_pressure_pa = 99000.0f;
    value.relative_humidity = 0.91f;
    value.visibility_m = 3000.0f;
    value.wind_m_s = {14.0f, 7.0f, 0.10f};
    value.gust_speed_m_s = 28.0f;
    value.precipitation_rate_mm_h = 32.0f;
    value.convective_activity = 1.0f;
    value.lightning_activity = 0.90f;
    value.aerosol_optical_depth_550nm = 0.30f;
    result.cloud_layers[0] =
        layer(CloudLayerKind::Convective, 350.0f, 11800.0f, 1.0f, 95.0f,
              0.58f, 32.0f, 1.0f);
    result.cloud_layers[1] =
        layer(CloudLayerKind::Cirrus, 8500.0f, 12500.0f, 0.86f, 9.0f,
              0.02f, 0.0f, 0.55f);
    result.cloud_layer_count = 2;
    break;
  case WeatherPreset::Snow:
    value.surface_temperature_kelvin = 268.15f;
    value.sea_level_pressure_pa = 100500.0f;
    value.relative_humidity = 0.92f;
    value.visibility_m = 4000.0f;
    value.wind_m_s = {5.5f, 2.5f, -0.02f};
    value.gust_speed_m_s = 10.0f;
    value.precipitation_rate_mm_h = 5.0f;
    value.snow_fraction = 1.0f;
    value.convective_activity = 0.35f;
    value.aerosol_optical_depth_550nm = 0.12f;
    result.cloud_layers[0] =
        layer(CloudLayerKind::Stratiform, 400.0f, 4800.0f, 0.98f, 48.0f,
              0.12f, 5.0f, 0.30f);
    result.cloud_layer_count = 1;
    break;
  case WeatherPreset::Fog:
    value.surface_temperature_kelvin = 283.15f;
    value.sea_level_pressure_pa = 101600.0f;
    value.relative_humidity = 0.995f;
    value.visibility_m = 350.0f;
    value.wind_m_s = {0.8f, 0.3f, 0.0f};
    value.gust_speed_m_s = 1.5f;
    value.convective_activity = 0.02f;
    value.aerosol_optical_depth_550nm = 0.45f;
    result.cloud_layers[0] =
        layer(CloudLayerKind::Fog, 0.0f, 280.0f, 1.0f, 18.0f, 1.0f,
              0.0f, 0.0f);
    result.cloud_layer_count = 1;
    break;
  case WeatherPreset::Natural:
  case WeatherPreset::Custom:
    return fixedProfile(WeatherPreset::Cumulus);
  }
  finishProfile(result);
  return result;
}

inline float periodicNoise(double seconds, double period_seconds,
                           double phase) {
  return static_cast<float>(
      std::sin(2.0 * astronomy::kPi * seconds / period_seconds + phase));
}

inline WeatherProfile naturalProfile(double unix_seconds,
                                     const GeoLocation &location,
                                     std::uint32_t seed) {
  const double day_number = astronomy::julianDay(unix_seconds) - 2451545.0;
  const double seed_phase =
      std::fmod(static_cast<double>(seed) * 0.6180339887498948,
                2.0 * astronomy::kPi);
  const float latitude =
      static_cast<float>(std::clamp(location.latitude_degrees, -90.0, 90.0));
  const float absolute_latitude = std::abs(latitude);
  const float hemisphere = latitude < 0.0f ? -1.0f : 1.0f;
  const float seasonal = static_cast<float>(
      std::cos(2.0 * astronomy::kPi * (day_number - 200.0) / 365.2422)) *
                         hemisphere;
  double local_seconds =
      unix_seconds + location.longitude_degrees * 240.0;
  local_seconds = std::fmod(local_seconds, 86400.0);
  if (local_seconds < 0.0) {
    local_seconds += 86400.0;
  }
  const float diurnal = static_cast<float>(std::sin(
      2.0 * astronomy::kPi * (local_seconds / 86400.0 - 0.375)));
  const float synoptic =
      0.55f * periodicNoise(unix_seconds, 4.8 * 86400.0, seed_phase) +
      0.30f * periodicNoise(unix_seconds, 2.3 * 86400.0,
                            seed_phase * 1.71 + 0.4) +
      0.15f * periodicNoise(unix_seconds, 0.9 * 86400.0,
                            seed_phase * 2.37 + 1.2);
  const float moisture_wave =
      0.6f * periodicNoise(unix_seconds, 3.7 * 86400.0,
                           seed_phase * 0.83 + 2.0) +
      0.4f * periodicNoise(unix_seconds, 1.4 * 86400.0,
                           seed_phase * 1.19 + 0.8);
  const float storminess = smoothstep((synoptic + 1.0f) * 0.5f);
  const float moisture = std::clamp(
      0.56f + 0.22f * moisture_wave + 0.18f * storminess, 0.18f, 0.99f);
  const float coverage = smoothstep(
      std::clamp((moisture - 0.38f) / 0.56f + 0.15f * storminess, 0.0f,
                 1.0f));
  const float precipitation =
      24.0f * smoothstep((moisture - 0.77f) / 0.22f) *
      smoothstep((coverage - 0.72f) / 0.28f) *
      (0.35f + 0.65f * storminess);

  WeatherProfile result;
  WeatherState &weather = result.weather;
  weather.preset = WeatherPreset::Natural;
  const float sea_level_mean_celsius =
      25.0f - 0.24f * absolute_latitude +
      (5.0f + 0.10f * absolute_latitude) * seasonal;
  weather.surface_temperature_kelvin =
      273.15f + sea_level_mean_celsius + 4.5f * diurnal -
      static_cast<float>(location.elevation_m) * 0.0065f -
      2.0f * coverage;
  weather.sea_level_pressure_pa =
      101800.0f - 1900.0f * storminess + 350.0f * synoptic;
  weather.relative_humidity = moisture;
  weather.visibility_m = std::clamp(
      65000.0f * (1.0f - 0.82f * moisture * coverage) /
          (1.0f + precipitation * 0.04f),
      300.0f, 100000.0f);
  const float wind_angle =
      static_cast<float>(seed_phase) +
      0.45f * periodicNoise(unix_seconds, 2.1 * 86400.0, seed_phase + 0.2);
  const float wind_speed = 2.0f + 12.0f * storminess;
  weather.wind_m_s = {wind_speed * std::cos(wind_angle),
                      wind_speed * std::sin(wind_angle),
                      0.08f * storminess};
  weather.gust_speed_m_s = wind_speed * (1.25f + 0.65f * storminess);
  weather.precipitation_rate_mm_h = precipitation;
  weather.snow_fraction = smoothstep(
      (274.65f - weather.surface_temperature_kelvin) / 4.0f);
  weather.convective_activity =
      std::clamp(0.15f + 0.85f * storminess *
                             smoothstep((weather.surface_temperature_kelvin -
                                         278.15f) /
                                        15.0f),
                 0.0f, 1.0f);
  weather.lightning_activity =
      std::clamp(weather.convective_activity * precipitation / 18.0f, 0.0f,
                 1.0f);
  weather.aerosol_optical_depth_550nm =
      std::clamp(0.06f + 0.20f * moisture + 0.08f * storminess, 0.03f,
                 0.45f);
  weather.ozone_dobson_units =
      285.0f + 35.0f * seasonal * absolute_latitude / 90.0f;

  const float lifted_base = std::clamp(
      125.0f * ((weather.surface_temperature_kelvin - 273.15f) -
                (dewPointKelvin(weather.surface_temperature_kelvin,
                                weather.relative_humidity) -
                 273.15f)),
      120.0f, 3200.0f);
  if (coverage > 0.04f) {
    const bool convective = weather.convective_activity > 0.42f;
    const float top = convective
                          ? 2800.0f + 8200.0f * weather.convective_activity
                          : lifted_base + 1700.0f + 2600.0f * coverage;
    result.cloud_layers[0] = layer(
        convective ? CloudLayerKind::Convective : CloudLayerKind::Stratiform,
        lifted_base, top, coverage,
        2.0f + 70.0f * coverage * coverage, 1.0f - 0.55f * storminess,
        precipitation, weather.convective_activity);
    result.cloud_layer_count = 1;
  }
  if (weather.convective_activity > 0.72f && result.cloud_layer_count < 4) {
    result.cloud_layers[result.cloud_layer_count++] = layer(
        CloudLayerKind::Cirrus, 8200.0f, 12500.0f,
        0.35f + 0.55f * weather.convective_activity,
        2.0f + 9.0f * weather.convective_activity, 0.02f, 0.0f,
        weather.convective_activity);
  }
  // Calm nocturnal boundary layers can saturate even when the synoptic-scale
  // humidity is only moderate. Model that near-surface moisture separately;
  // using `moisture > 0.94` directly was unreachable together with the old
  // calm-wind condition because both were driven by opposing storminess.
  const float nocturnal_cooling =
      smoothstep((-diurnal - 0.25f) / 0.70f);
  const float calm_boundary_layer =
      smoothstep((5.0f - wind_speed) / 3.0f);
  const float near_surface_humidity = std::clamp(
      moisture + 0.38f * nocturnal_cooling * calm_boundary_layer,
      moisture, 0.999f);
  const bool radiation_fog = near_surface_humidity >= 0.94f &&
                             wind_speed < 5.0f && diurnal < -0.25f &&
                             precipitation < 0.1f;
  if (radiation_fog) {
    weather.relative_humidity = near_surface_humidity;
    const float fog_density =
        smoothstep((near_surface_humidity - 0.94f) / 0.059f);
    weather.visibility_m =
        std::min(weather.visibility_m, 1200.0f - 1000.0f * fog_density);
    result.cloud_layers[0] =
        layer(CloudLayerKind::Fog, 0.0f, 120.0f + 180.0f * fog_density,
              1.0f, 8.0f + 12.0f * fog_density, 1.0f, 0.0f, 0.0f);
    result.cloud_layer_count = 1;
  }
  finishProfile(result);
  return result;
}

inline WeatherProfile mixProfiles(const WeatherProfile &a,
                                  const WeatherProfile &b, float amount,
                                  bool ease_amount = true) {
  amount = std::clamp(amount, 0.0f, 1.0f);
  if (amount <= 0.0f) {
    WeatherProfile result = a;
    result.weather.transition_progress = 0.0f;
    return result;
  }
  if (amount >= 1.0f) {
    WeatherProfile result = b;
    result.weather.transition_progress = 1.0f;
    return result;
  }

  WeatherProfile result;
  if (ease_amount) {
    amount = smoothstep(amount);
  }
  const WeatherState &wa = a.weather;
  const WeatherState &wb = b.weather;
  WeatherState &weather = result.weather;
  weather.preset = amount >= 1.0f ? wb.preset : WeatherPreset::Custom;
  weather.transition_progress = amount;
  weather.surface_temperature_kelvin =
      mix(wa.surface_temperature_kelvin, wb.surface_temperature_kelvin,
          amount);
  weather.sea_level_pressure_pa =
      mix(wa.sea_level_pressure_pa, wb.sea_level_pressure_pa, amount);
  weather.relative_humidity =
      mix(wa.relative_humidity, wb.relative_humidity, amount);
  weather.visibility_m = mix(wa.visibility_m, wb.visibility_m, amount);
  weather.wind_m_s = mix(wa.wind_m_s, wb.wind_m_s, amount);
  weather.gust_speed_m_s = mix(wa.gust_speed_m_s, wb.gust_speed_m_s, amount);
  weather.precipitation_rate_mm_h =
      mix(wa.precipitation_rate_mm_h, wb.precipitation_rate_mm_h, amount);
  weather.snow_fraction = mix(wa.snow_fraction, wb.snow_fraction, amount);
  weather.surface_wetness =
      mix(wa.surface_wetness, wb.surface_wetness, amount);
  weather.convective_activity =
      mix(wa.convective_activity, wb.convective_activity, amount);
  weather.lightning_activity =
      mix(wa.lightning_activity, wb.lightning_activity, amount);
  weather.aerosol_optical_depth_550nm =
      mix(wa.aerosol_optical_depth_550nm,
          wb.aerosol_optical_depth_550nm, amount);
  weather.ozone_dobson_units =
      mix(wa.ozone_dobson_units, wb.ozone_dobson_units, amount);

  // A layer has no persistent wire ID, so first match equal cloud kinds by
  // altitude. This keeps an existing cirrus deck paired with the destination
  // cirrus deck instead of morphing it into a low convective layer merely
  // because both happened to occupy array slot zero.
  std::array<int, kMaximumCloudLayers> source_for_target{};
  std::array<int, kMaximumCloudLayers> target_for_source{};
  source_for_target.fill(-1);
  target_for_source.fill(-1);
  const std::size_t source_count =
      std::min<std::size_t>(a.cloud_layer_count, kMaximumCloudLayers);
  const std::size_t target_count =
      std::min<std::size_t>(b.cloud_layer_count, kMaximumCloudLayers);

  auto pairClosest = [&](bool require_same_kind) {
    std::size_t best_source = kMaximumCloudLayers;
    std::size_t best_target = kMaximumCloudLayers;
    float best_distance = std::numeric_limits<float>::max();
    for (std::size_t source = 0; source < source_count; ++source) {
      if (target_for_source[source] >= 0) {
        continue;
      }
      const CloudLayerState &source_layer = a.cloud_layers[source];
      const float source_center =
          0.5f * (source_layer.base_altitude_m +
                  source_layer.top_altitude_m);
      for (std::size_t target = 0; target < target_count; ++target) {
        if (source_for_target[target] >= 0) {
          continue;
        }
        const CloudLayerState &target_layer = b.cloud_layers[target];
        if (require_same_kind && source_layer.kind != target_layer.kind) {
          continue;
        }
        const float target_center =
            0.5f * (target_layer.base_altitude_m +
                    target_layer.top_altitude_m);
        const float distance = std::abs(source_center - target_center);
        if (distance < best_distance) {
          best_distance = distance;
          best_source = source;
          best_target = target;
        }
      }
    }
    if (best_source == kMaximumCloudLayers) {
      return false;
    }
    target_for_source[best_source] = static_cast<int>(best_target);
    source_for_target[best_target] = static_cast<int>(best_source);
    return true;
  };

  while (pairClosest(true)) {
  }

  std::size_t paired_count = 0;
  for (std::size_t source = 0; source < source_count; ++source) {
    paired_count += target_for_source[source] >= 0 ? 1U : 0U;
  }
  std::size_t output_slot_count =
      source_count + target_count - paired_count;
  while (output_slot_count > kMaximumCloudLayers && pairClosest(false)) {
    --output_slot_count;
  }

  auto interpolateLayer = [&](const CloudLayerState &source,
                              const CloudLayerState &target) {
    CloudLayerState layer_value;
    layer_value.kind = amount < 0.5f ? source.kind : target.kind;
    layer_value.base_altitude_m = mix(source.base_altitude_m,
                                      target.base_altitude_m, amount);
    layer_value.top_altitude_m =
        mix(source.top_altitude_m, target.top_altitude_m, amount);
    layer_value.coverage = mix(source.coverage, target.coverage, amount);
    layer_value.optical_depth =
        mix(source.optical_depth, target.optical_depth, amount);
    layer_value.liquid_fraction =
        mix(source.liquid_fraction, target.liquid_fraction, amount);
    layer_value.precipitation_rate_mm_h =
        mix(source.precipitation_rate_mm_h,
            target.precipitation_rate_mm_h, amount);
    layer_value.convective_activity =
        mix(source.convective_activity, target.convective_activity, amount);
    return layer_value;
  };
  auto fadeLayer = [&](const CloudLayerState &layer_value, float strength) {
    CloudLayerState faded = layer_value;
    faded.coverage *= strength;
    faded.optical_depth *= strength;
    faded.precipitation_rate_mm_h *= strength;
    faded.convective_activity *= strength;
    return faded;
  };
  auto appendLayer = [&](const CloudLayerState &layer_value) {
    if (result.cloud_layer_count >= kMaximumCloudLayers) {
      return;
    }
    result.cloud_layers[result.cloud_layer_count++] = layer_value;
  };

  // Destination order makes the transition endpoint identical to the target.
  // Unmatched layers fade at their own physical altitude instead of travelling
  // to or from zero metres.
  for (std::size_t target = 0; target < target_count; ++target) {
    const int source = source_for_target[target];
    appendLayer(source >= 0
                    ? interpolateLayer(
                          a.cloud_layers[static_cast<std::size_t>(source)],
                          b.cloud_layers[target])
                    : fadeLayer(b.cloud_layers[target], amount));
  }
  for (std::size_t source = 0; source < source_count; ++source) {
    if (target_for_source[source] < 0) {
      appendLayer(fadeLayer(a.cloud_layers[source], 1.0f - amount));
    }
  }
  finishProfile(result);
  return result;
}

} // namespace weather_detail

class WeatherDirector {
public:
  explicit WeatherDirector(WeatherPreset preset = WeatherPreset::Cumulus,
                           std::uint32_t seed = 1U)
      : requested_preset_(preset), seed_(seed == 0U ? 1U : seed) {
    current_ = weather_detail::fixedProfile(
        preset == WeatherPreset::Natural ? WeatherPreset::Cumulus : preset);
    current_.weather.preset = preset;
    transition_start_ = current_;
  }

  void initialize(double unix_seconds, const GeoLocation &location) {
    current_ = targetFor(unix_seconds, location);
    current_.weather.transition_progress = 1.0f;
    transition_start_ = current_;
    transition_target_ = current_;
    transition_elapsed_seconds_ = transition_duration_seconds_ = 0.0;
    transition_target_initialized_ = false;
    snap_to_target_on_next_advance_ = false;
  }

  void setPreset(WeatherPreset preset, double transition_seconds) {
    if (preset == WeatherPreset::Custom) {
      throw std::invalid_argument("Custom is not a loadable weather preset");
    }
    beginTransition(transition_seconds);
    requested_preset_ = preset;
    has_custom_target_ = false;
  }

  void setCustomTarget(WeatherProfile profile, double transition_seconds) {
    weather_detail::finishProfile(profile);
    profile.weather.preset = WeatherPreset::Custom;
    beginTransition(transition_seconds);
    custom_target_ = profile;
    requested_preset_ = WeatherPreset::Custom;
    has_custom_target_ = true;
  }

  void releaseToNatural(double transition_seconds) {
    setPreset(WeatherPreset::Natural, transition_seconds);
  }

  void setSeed(std::uint32_t seed) { seed_ = seed == 0U ? 1U : seed; }
  void setSurfaceWetness(float value) {
    if (!std::isfinite(value) || value < 0.0f || value > 1.0f) {
      throw std::invalid_argument("Surface wetness must be in [0, 1]");
    }
    surface_wetness_ = value;
    current_.weather.surface_wetness = value;
  }
  std::uint32_t seed() const { return seed_; }
  WeatherPreset requestedPreset() const { return requested_preset_; }

  void advance(double real_dt, double unix_seconds,
               const GeoLocation &location) {
    if (!(real_dt >= 0.0) || !std::isfinite(real_dt)) {
      throw std::invalid_argument("Weather dt must be finite and non-negative");
    }
    const WeatherProfile target = targetFor(unix_seconds, location);
    if (snap_to_target_on_next_advance_) {
      current_ = target;
      current_.weather.transition_progress = 1.0f;
      snap_to_target_on_next_advance_ = false;
    } else if (transition_duration_seconds_ > 0.0 &&
        transition_elapsed_seconds_ < transition_duration_seconds_) {
      if (!transition_target_initialized_) {
        transition_target_ = target;
        transition_target_initialized_ = true;
      }
      transition_elapsed_seconds_ =
          std::min(transition_duration_seconds_,
                   transition_elapsed_seconds_ + real_dt);
      const float progress = static_cast<float>(
          transition_elapsed_seconds_ / transition_duration_seconds_);
      current_ = weather_detail::mixProfiles(
          transition_start_, transition_target_, progress);
      current_.weather.transition_progress = progress;
    } else if (requested_preset_ == WeatherPreset::Natural &&
               !has_custom_target_ && real_dt > 0.0) {
      // Procedural thresholds (fog saturation, convective type and anvil
      // formation) describe a target regime, not an instantaneous state
      // change. Relax toward that target in wall time so normal simulation
      // ticks never pop a full layer or visibility field in one snapshot.
      constexpr double natural_response_seconds = 120.0;
      const float response = static_cast<float>(
          1.0 - std::exp(-real_dt / natural_response_seconds));
      current_ =
          weather_detail::mixProfiles(current_, target, response, false);
      current_.weather.preset = WeatherPreset::Natural;
      current_.weather.transition_progress = 1.0f;
    } else {
      current_ = target;
      current_.weather.transition_progress = 1.0f;
    }

    const float wetting_rate =
        std::min(1.0f, current_.weather.precipitation_rate_mm_h / 4.0f);
    const float drying_rate = 0.006f +
                              0.018f * (1.0f - current_.weather.relative_humidity) +
                              0.001f * current_.weather.gust_speed_m_s;
    const float wetting_source = 0.08f * wetting_rate;
    if (drying_rate > 0.000001f) {
      const float equilibrium = wetting_source / drying_rate;
      const float decay =
          std::exp(-drying_rate * static_cast<float>(real_dt));
      surface_wetness_ =
          equilibrium + (surface_wetness_ - equilibrium) * decay;
    } else {
      surface_wetness_ += wetting_source * static_cast<float>(real_dt);
    }
    surface_wetness_ = std::clamp(surface_wetness_, 0.0f, 1.0f);
    current_.weather.surface_wetness = surface_wetness_;
  }

  const WeatherProfile &profile() const { return current_; }

private:
  WeatherProfile targetFor(double unix_seconds,
                           const GeoLocation &location) const {
    if (has_custom_target_) {
      return custom_target_;
    }
    if (requested_preset_ == WeatherPreset::Natural) {
      return weather_detail::naturalProfile(unix_seconds, location, seed_);
    }
    return weather_detail::fixedProfile(requested_preset_);
  }

  void beginTransition(double transition_seconds) {
    if (!std::isfinite(transition_seconds) || transition_seconds < 0.0 ||
        transition_seconds > 86400.0) {
      throw std::invalid_argument(
          "Weather transition must be between 0 and 86400 seconds");
    }
    transition_start_ = current_;
    transition_elapsed_seconds_ = 0.0;
    transition_duration_seconds_ = transition_seconds;
    transition_target_initialized_ = false;
    snap_to_target_on_next_advance_ = transition_seconds == 0.0;
  }

  WeatherPreset requested_preset_ = WeatherPreset::Cumulus;
  std::uint32_t seed_ = 1U;
  WeatherProfile current_{};
  WeatherProfile transition_start_{};
  WeatherProfile transition_target_{};
  WeatherProfile custom_target_{};
  double transition_elapsed_seconds_ = 0.0;
  double transition_duration_seconds_ = 0.0;
  float surface_wetness_ = 0.0f;
  bool has_custom_target_ = false;
  bool transition_target_initialized_ = false;
  bool snap_to_target_on_next_advance_ = false;
};

} // namespace cloud
