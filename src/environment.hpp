#pragma once

#include "astronomy.hpp"
#include "environment_types.hpp"
#include "simulation.hpp"
#include "weather.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace cloud {

struct SkyEnvironmentConfig {
  double utc_unix_seconds = 1710936000.0; // 2024-03-20 12:00:00 UTC
  float time_scale = 1.0f;
  GeoLocation location{};
  WorldDomain domain{};
  WeatherPreset weather_preset = WeatherPreset::Natural;
  std::uint32_t weather_seed = 1U;
};

class SkyEnvironment {
public:
  explicit SkyEnvironment(const SkyEnvironmentConfig &config)
      : utc_unix_seconds_(config.utc_unix_seconds),
        time_scale_(config.time_scale), location_(config.location),
        domain_(config.domain),
        weather_(config.weather_preset, config.weather_seed),
        random_state_(config.weather_seed == 0U ? 1U : config.weather_seed) {
    validateUtc(utc_unix_seconds_);
    validateTimeScale(time_scale_);
    validateLocation(location_);
    validateDomain(domain_);
    weather_.initialize(utc_unix_seconds_, location_);
    updateSnapshot(0.0);
  }

  void step(double real_dt) {
    advance(real_dt, real_dt, true);
  }

  void step(double fluid_dt, double wall_dt) {
    advance(fluid_dt, wall_dt, true);
  }

  // Cloud spin-up must not move a user-selected calendar time. It advances
  // fluid/weather relaxation only, so the first rendered frame still matches
  // the requested UTC even when time_scale is very large.
  void spinUp(double real_dt) { advance(real_dt, real_dt, false); }

  const EnvironmentSnapshot &snapshot() const { return snapshot_; }

  void setUtcUnixSeconds(double value) {
    validateUtc(value);
    utc_unix_seconds_ = value;
    updateSnapshot(0.0);
  }

  void setTimeScale(float value) {
    validateTimeScale(value);
    time_scale_ = value;
    updateSnapshot(0.0);
  }

  void setLocation(const GeoLocation &value) {
    validateLocation(value);
    location_ = value;
    updateSnapshot(0.0);
  }

  void setDomain(const WorldDomain &value) {
    validateDomain(value);
    domain_ = value;
    updateSnapshot(0.0);
  }

  void setWeatherPreset(WeatherPreset preset, double transition_seconds) {
    weather_.setPreset(preset, transition_seconds);
    updateSnapshot(0.0);
  }

  void setCustomWeather(WeatherProfile profile, double transition_seconds) {
    weather_.setCustomTarget(profile, transition_seconds);
    updateSnapshot(0.0);
  }

  void releaseWeather(double transition_seconds) {
    weather_.releaseToNatural(transition_seconds);
    updateSnapshot(0.0);
  }

  void setWeatherSeed(std::uint32_t seed) {
    weather_.setSeed(seed);
    random_state_ = seed == 0U ? 1U : seed;
    lightning_countdown_seconds_ = 0.0;
    updateSnapshot(0.0);
  }

  void setSurfaceWetness(float value) {
    weather_.setSurfaceWetness(value);
    updateSnapshot(0.0);
  }

  std::uint32_t weatherSeed() const { return weather_.seed(); }
  const WeatherProfile &weatherProfile() const { return weather_.profile(); }

  CloudForcing cloudForcing(int grid_size) const {
    if (grid_size < 1) {
      throw std::invalid_argument("Cloud forcing grid must be positive");
    }
    const WeatherState &weather = snapshot_.weather;
    CloudForcing forcing;
    forcing.enabled = true;
    forcing.spatial_seed = weather_.seed();
    float coverage = 0.0f;
    float optical_depth = 0.0f;
    for (std::size_t index = 0; index < snapshot_.cloud_layer_count; ++index) {
      const CloudLayerState &layer = snapshot_.cloud_layers[index];
      const float clipped_base =
          std::clamp(layer.base_altitude_m, 0.0f, domain_.vertical_extent_m);
      const float clipped_top =
          std::clamp(layer.top_altitude_m, 0.0f, domain_.vertical_extent_m);
      const float cell_height =
          domain_.vertical_extent_m /
          static_cast<float>(std::max(1, grid_size - 1));
      const bool negligible_faded_layer =
          layer.coverage < 0.0001f && layer.optical_depth < 0.01f &&
          layer.precipitation_rate_mm_h < 0.001f &&
          layer.convective_activity < 0.001f;
      if (layer.kind == CloudLayerKind::None || layer.coverage <= 0.0f ||
          negligible_faded_layer ||
          clipped_top - clipped_base < 0.25f * cell_height ||
          forcing.cloud_layer_count >= forcing.cloud_layers.size()) {
        continue;
      }
      CloudForcingLayer forcing_layer;
      switch (layer.kind) {
      case CloudLayerKind::Stratiform:
        forcing_layer.kind = CloudForcingLayerKind::Stratiform;
        break;
      case CloudLayerKind::Convective:
        forcing_layer.kind = CloudForcingLayerKind::Convective;
        break;
      case CloudLayerKind::Cirrus:
        forcing_layer.kind = CloudForcingLayerKind::Cirrus;
        break;
      case CloudLayerKind::Fog:
        forcing_layer.kind = CloudForcingLayerKind::Fog;
        break;
      case CloudLayerKind::None:
        continue;
      }
      forcing_layer.normalized_base =
          clipped_base / domain_.vertical_extent_m;
      forcing_layer.normalized_top =
          clipped_top / domain_.vertical_extent_m;
      forcing_layer.coverage = layer.coverage;
      const float original_thickness =
          layer.top_altitude_m - layer.base_altitude_m;
      const float clipped_fraction =
          (clipped_top - clipped_base) / original_thickness;
      forcing_layer.optical_depth = layer.optical_depth * clipped_fraction;
      forcing_layer.convective_activity = layer.convective_activity;
      forcing.cloud_layers[forcing.cloud_layer_count++] = forcing_layer;
      coverage = std::max(coverage, forcing_layer.coverage);
      optical_depth = std::max(optical_depth, forcing_layer.optical_depth);
    }
    const float horizontal_cells_per_m =
        static_cast<float>(std::max(1, grid_size - 1)) /
        domain_.horizontal_extent_m;
    const float vertical_cells_per_m =
        static_cast<float>(std::max(1, grid_size - 1)) /
        domain_.vertical_extent_m;
    auto velocity = [&](float speed_multiplier, float veer_degrees,
                        float up_multiplier) {
      const float angle = veer_degrees *
                          static_cast<float>(astronomy::kDegreesToRadians);
      const float east = weather.wind_m_s.east * std::cos(angle) -
                         weather.wind_m_s.north * std::sin(angle);
      const float north = weather.wind_m_s.east * std::sin(angle) +
                          weather.wind_m_s.north * std::cos(angle);
      return SimulationVector{
          east * speed_multiplier * horizontal_cells_per_m,
          north * speed_multiplier * horizontal_cells_per_m,
          (weather.wind_m_s.up * up_multiplier +
           0.35f * weather.convective_activity * up_multiplier) *
              vertical_cells_per_m};
    };

    forcing.wind_profile = {{
        {0.0f, velocity(0.65f, -7.0f, 0.20f)},
        {0.25f, velocity(1.00f, 0.0f, 0.65f)},
        {0.70f, velocity(1.35f, 12.0f, 0.35f)},
        {1.0f, velocity(1.65f, 22.0f, 0.05f)},
    }};
    forcing.wind_response_per_second =
        0.10f + 0.30f * weather.convective_activity;
    forcing.thermal_source_multiplier =
        0.05f + 1.35f * weather.convective_activity;
    forcing.vapor_source_multiplier =
        std::clamp(0.08f + 1.35f * weather.relative_humidity *
                               (0.25f + 0.75f * coverage),
                   0.05f, 1.60f);
    const float physical_updraft_acceleration_m_s2 =
        0.128f + 2.72f * weather.convective_activity;
    forcing.updraft_acceleration_cells_per_second_squared =
        physical_updraft_acceleration_m_s2 * vertical_cells_per_m;
    const float physical_buoyancy_acceleration_m_s2 =
        0.25f + 1.25f * weather.convective_activity;
    forcing.buoyancy_acceleration_cells_per_second_squared =
        physical_buoyancy_acceleration_m_s2 * vertical_cells_per_m;
    const float surface_celsius = weather.surface_temperature_kelvin - 273.15f;
    forcing.surface_temperature_target =
        std::clamp((surface_celsius - 15.0f) / 20.0f, -0.75f, 1.0f);
    forcing.top_temperature_target =
        forcing.surface_temperature_target - 0.45f;
    forcing.temperature_target_response_per_second = 0.025f;
    forcing.surface_vapor_target =
        std::clamp(0.05f + 0.42f * weather.relative_humidity *
                               (0.35f + 0.65f * coverage),
                   0.03f, 0.48f);
    forcing.top_vapor_target = forcing.surface_vapor_target * 0.35f;
    forcing.vapor_target_response_per_second =
        0.025f + 0.055f * coverage;
    forcing.lapse_cooling_per_second = 0.025f + 0.025f * coverage;
    forcing.condensation_per_second = 3.0f + 4.0f * coverage;
    forcing.evaporation_per_second = 0.7f + 2.4f * (1.0f - coverage);
    forcing.cloud_decay_per_second =
        0.002f + 0.035f * (1.0f - coverage) * (1.0f - coverage);
    forcing.vapor_decay_per_second =
        0.0005f + 0.003f * (1.0f - weather.relative_humidity);
    forcing.temperature_decay_per_second = 0.04f;
    forcing.precipitation_per_second =
        std::clamp(weather.precipitation_rate_mm_h / 40.0f, 0.0f, 1.5f);
    forcing.precipitation_threshold =
        std::clamp(0.78f - 0.35f * weather.convective_activity -
                       0.002f * optical_depth,
                   0.25f, 0.78f);
    return forcing;
  }

private:
  void advance(double fluid_dt, double wall_dt, bool advance_calendar) {
    if (!std::isfinite(fluid_dt) || fluid_dt < 0.0 || fluid_dt > 1.0 ||
        !std::isfinite(wall_dt) || wall_dt < 0.0 || wall_dt > 86400.0) {
      throw std::invalid_argument(
          "Sky environment fluid dt must be at most 1 second and wall dt at most one day");
    }
    fluid_time_seconds_ += fluid_dt;
    constexpr std::size_t maximum_weather_substeps = 60U;
    const std::size_t weather_substeps =
        wall_dt > 0.0
            ? std::min<std::size_t>(
                  maximum_weather_substeps,
                  static_cast<std::size_t>(std::ceil(wall_dt)))
            : 1U;
    const double weather_dt = wall_dt / static_cast<double>(weather_substeps);
    for (std::size_t step = 0; step < weather_substeps; ++step) {
      if (advance_calendar) {
        constexpr double minimum_utc = -2208988800.0;
        constexpr double maximum_utc = 4133980800.0;
        const double requested_utc =
            utc_unix_seconds_ + weather_dt * static_cast<double>(time_scale_);
        if (requested_utc <= minimum_utc) {
          utc_unix_seconds_ = minimum_utc;
          time_scale_ = 0.0f;
        } else if (requested_utc >= maximum_utc) {
          utc_unix_seconds_ = maximum_utc;
          time_scale_ = 0.0f;
        } else {
          utc_unix_seconds_ = requested_utc;
        }
      }
      weather_.advance(weather_dt, utc_unix_seconds_, location_);
      updateLightning(weather_dt);
    }
    validateUtc(utc_unix_seconds_);
    updateSnapshot(wall_dt);
  }
  static void validateUtc(double value) {
    // The truncated lunar model is intentionally bounded to the modern era.
    if (!std::isfinite(value) || value < -2208988800.0 ||
        value > 4133980800.0) {
      throw std::invalid_argument(
          "UTC must be a finite Unix timestamp between 1900 and 2100");
    }
  }

  static void validateTimeScale(float value) {
    if (!std::isfinite(value) || value < -86400.0f || value > 86400.0f) {
      throw std::invalid_argument("Time scale must be in [-86400, 86400]");
    }
  }

  static void validateLocation(const GeoLocation &value) {
    if (!std::isfinite(value.latitude_degrees) ||
        !std::isfinite(value.longitude_degrees) ||
        !std::isfinite(value.elevation_m) || value.latitude_degrees < -90.0 ||
        value.latitude_degrees > 90.0 || value.longitude_degrees < -180.0 ||
        value.longitude_degrees > 180.0 || value.elevation_m < -500.0 ||
        value.elevation_m > 100000.0) {
      throw std::invalid_argument(
          "Location must have valid latitude, longitude, and elevation");
    }
  }

  static void validateDomain(const WorldDomain &value) {
    if (!std::isfinite(value.horizontal_extent_m) ||
        !std::isfinite(value.vertical_extent_m) ||
        value.horizontal_extent_m < 100.0f ||
        value.horizontal_extent_m > 2000000.0f ||
        value.vertical_extent_m < 100.0f ||
        value.vertical_extent_m > 100000.0f) {
      throw std::invalid_argument(
          "World domain extents are outside the supported range");
    }
  }

  float randomUnit() {
    std::uint32_t value = random_state_;
    value ^= value << 13U;
    value ^= value >> 17U;
    value ^= value << 5U;
    random_state_ = value == 0U ? 1U : value;
    return static_cast<float>(random_state_ >> 8U) /
           static_cast<float>(1U << 24U);
  }

  void updateLightning(double real_dt) {
    const float activity = weather_.profile().weather.lightning_activity;
    if (activity < 0.02f) {
      lightning_flash_seconds_ =
          std::max(0.0, lightning_flash_seconds_ - real_dt);
      lightning_countdown_seconds_ = 0.0;
      return;
    }
    const double average_interval =
        24.0 - 21.5 * static_cast<double>(activity);
    if (lightning_countdown_seconds_ <= 0.0) {
      lightning_countdown_seconds_ =
          average_interval * (0.35 + 1.65 * randomUnit());
    }

    // A long process suspension must not replay an old flash at resume time.
    // Preserve a monotonic event revision approximately, then start a fresh
    // deterministic countdown at the final weather state.
    if (real_dt > 60.0) {
      lightning_flash_seconds_ = 0.0;
      if (real_dt >= lightning_countdown_seconds_) {
        const double remaining = real_dt - lightning_countdown_seconds_;
        const std::uint64_t events =
            1U + static_cast<std::uint64_t>(remaining / average_interval);
        lightning_event_id_ += static_cast<std::uint32_t>(events);
      }
      lightning_countdown_seconds_ =
          average_interval * (0.35 + 1.65 * randomUnit());
      return;
    }

    double remaining = real_dt;
    while (remaining >= lightning_countdown_seconds_) {
      lightning_flash_seconds_ = std::max(
          0.0, lightning_flash_seconds_ - lightning_countdown_seconds_);
      remaining -= lightning_countdown_seconds_;
      ++lightning_event_id_;
      // Keep the event visible for at least two default 5 Hz SKS1 snapshots.
      // Unreal may render shorter sub-flashes from the deterministic event id.
      lightning_flash_seconds_ = 0.35 + 0.15 * randomUnit();
      lightning_countdown_seconds_ =
          average_interval * (0.35 + 1.65 * randomUnit());
    }
    lightning_flash_seconds_ =
        std::max(0.0, lightning_flash_seconds_ - remaining);
    lightning_countdown_seconds_ -= remaining;
  }

  void updateSnapshot(double /*real_dt*/) {
    const auto sun = astronomy::solarCoordinates(utc_unix_seconds_, location_);
    const auto moon =
        astronomy::lunarCoordinates(utc_unix_seconds_, location_, sun);
    const WeatherProfile &profile = weather_.profile();

    ++snapshot_.sequence;
    snapshot_.fluid_time_seconds = fluid_time_seconds_;
    snapshot_.utc_unix_seconds = utc_unix_seconds_;
    snapshot_.time_scale = time_scale_;
    snapshot_.location = location_;
    snapshot_.domain = domain_;
    snapshot_.weather = profile.weather;
    snapshot_.cloud_layers = profile.cloud_layers;
    snapshot_.cloud_layer_count = profile.cloud_layer_count;
    snapshot_.lightning_event_id = lightning_event_id_;
    const float humidity_growth =
        1.0f + 1.8f * weather_detail::smoothstep(
                          (profile.weather.relative_humidity - 0.65f) / 0.34f);
    const float effective_aerosol_optical_depth = std::clamp(
        profile.weather.aerosol_optical_depth_550nm * humidity_growth,
        0.005f, 3.0f);
    snapshot_.weather.aerosol_optical_depth_550nm =
        effective_aerosol_optical_depth;

    CelestialState &celestial = snapshot_.celestial;
    celestial.sun_direction = sun.direction;
    celestial.sun_geometric_elevation_degrees = sun.elevation_degrees;
    celestial.sun_azimuth_degrees = sun.azimuth_degrees;
    celestial.sun_angular_radius_degrees = sun.angular_radius_degrees;
    const float elevation = std::max(-5.0f, sun.elevation_degrees);
    float air_mass = 40.0f;
    if (sun.elevation_degrees > -0.833f) {
      air_mass = 1.0f /
                 (std::sin(elevation *
                           static_cast<float>(astronomy::kDegreesToRadians)) +
                  0.50572f * std::pow(elevation + 6.07995f, -1.6364f));
    }
    const float sea_level_pressure_ratio =
        profile.weather.sea_level_pressure_pa / 101325.0f;
    const double observer_elevation_m = location_.elevation_m;
    double pressure_height_factor = 1.0;
    if (observer_elevation_m <= 11000.0) {
      pressure_height_factor = std::pow(
          std::max(0.01, 1.0 - 2.25577e-5 * observer_elevation_m),
          5.25588);
    } else {
      pressure_height_factor =
          0.223361 * std::exp(-(observer_elevation_m - 11000.0) / 6341.62);
    }
    const float observer_pressure_ratio = static_cast<float>(
        static_cast<double>(sea_level_pressure_ratio) *
        pressure_height_factor);
    const float aerosol_column_above_observer = std::clamp(
        std::exp(-static_cast<float>(observer_elevation_m) /
                 snapshot_.optics.mie_scale_height_m),
        0.0f, 3.0f);
    const float vertical_optical_depth =
        0.075f * observer_pressure_ratio +
        effective_aerosol_optical_depth * aerosol_column_above_observer;
    const float atmospheric_transmittance =
        sun.elevation_degrees > -0.833f
            ? std::exp(-vertical_optical_depth * air_mass)
            : 0.0f;
    celestial.sun_irradiance_w_m2 =
        1361.0f / (sun.distance_au * sun.distance_au) *
        atmospheric_transmittance;
    celestial.sun_direct_illuminance_lux =
        celestial.sun_irradiance_w_m2 * 105.0f;
    celestial.sun_color_temperature_kelvin =
        1900.0f + 4600.0f * weather_detail::smoothstep(
                              (sun.elevation_degrees + 2.0f) / 35.0f);

    celestial.moon_direction = moon.direction;
    celestial.moon_geometric_elevation_degrees = moon.elevation_degrees;
    celestial.moon_azimuth_degrees = moon.azimuth_degrees;
    celestial.moon_angular_radius_degrees = moon.angular_radius_degrees;
    celestial.moon_illuminated_fraction = moon.illuminated_fraction;
    const float moon_altitude_factor = weather_detail::smoothstep(
        (moon.elevation_degrees + 1.0f) / 18.0f);
    celestial.moon_illuminance_lux =
        0.30f * std::pow(moon.illuminated_fraction, 1.7f) *
        moon_altitude_factor *
        std::exp(-effective_aerosol_optical_depth * 2.0f);
    const float astronomical_night = weather_detail::smoothstep(
        (-sun.elevation_degrees - 6.0f) / 12.0f);
    celestial.star_visibility =
        astronomical_night *
        (1.0f - 0.55f * moon.illuminated_fraction * moon_altitude_factor) *
        std::clamp(profile.weather.visibility_m / 30000.0f, 0.08f, 1.0f);

    AtmosphereOptics &optics = snapshot_.optics;
    // These are atmosphere-bottom coefficients for the renderer LUT, not
    // local coefficients at the observer. Molecular column density is
    // proportional to sea-level pressure; the renderer applies scale height
    // from planet_bottom_radius + observer elevation exactly once.
    const float density_ratio = sea_level_pressure_ratio;
    constexpr float base_rayleigh[3] = {5.802e-6f, 13.558e-6f, 33.100e-6f};
    constexpr float base_ozone[3] = {0.650e-6f, 1.881e-6f, 0.085e-6f};
    const float mie_extinction_per_m =
        effective_aerosol_optical_depth / optics.mie_scale_height_m;
    for (std::size_t channel = 0; channel < 3; ++channel) {
      optics.rayleigh_scattering_per_m[channel] =
          base_rayleigh[channel] * density_ratio;
      optics.mie_scattering_per_m[channel] =
          mie_extinction_per_m * 0.90f;
      optics.mie_absorption_per_m[channel] =
          mie_extinction_per_m * 0.10f;
      optics.ozone_absorption_per_m[channel] =
          base_ozone[channel] * profile.weather.ozone_dobson_units / 300.0f;
    }
    optics.aerosol_optical_depth_550nm =
        effective_aerosol_optical_depth;
    optics.ozone_dobson_units = profile.weather.ozone_dobson_units;
    optics.mie_anisotropy =
        std::clamp(0.76f + 0.08f * humidity_growth / 2.8f, 0.76f, 0.84f);
    if (profile.weather.precipitation_rate_mm_h > 0.001f &&
        profile.weather.snow_fraction > 0.8f) {
      optics.ground_albedo = {0.78f, 0.82f, 0.86f};
    } else {
      optics.ground_albedo = {0.18f, 0.18f, 0.18f};
    }

    snapshot_.weather.lightning_flash =
        lightning_flash_seconds_ > 0.0
            ? static_cast<float>(std::clamp(lightning_flash_seconds_ / 0.50,
                                            0.0, 1.0))
            : 0.0f;
    snapshot_.flags = 0U;
    if (sun.elevation_degrees > -0.833f) {
      snapshot_.flags |= kSkyFlagSunAboveHorizon;
    }
    if (moon.elevation_degrees > 0.0f) {
      snapshot_.flags |= kSkyFlagMoonAboveHorizon;
    }
    if (snapshot_.weather.precipitation_rate_mm_h > 0.001f) {
      snapshot_.flags |= kSkyFlagPrecipitation;
    }
    if (snapshot_.weather.fog_extinction_per_m > 0.0f) {
      snapshot_.flags |= kSkyFlagFog;
    }
    if (snapshot_.weather.lightning_flash > 0.0f) {
      snapshot_.flags |= kSkyFlagLightningFlash;
    }
    if (snapshot_.weather.snow_fraction > 0.15f &&
        snapshot_.weather.precipitation_rate_mm_h > 0.001f) {
      snapshot_.flags |= kSkyFlagSnow;
    }
  }

  double utc_unix_seconds_ = 0.0;
  double fluid_time_seconds_ = 0.0;
  float time_scale_ = 1.0f;
  GeoLocation location_{};
  WorldDomain domain_{};
  WeatherDirector weather_{};
  EnvironmentSnapshot snapshot_{};
  std::uint32_t random_state_ = 1U;
  std::uint32_t lightning_event_id_ = 0U;
  double lightning_countdown_seconds_ = 0.0;
  double lightning_flash_seconds_ = 0.0;
};

} // namespace cloud
