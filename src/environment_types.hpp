#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace cloud {

// Environment coordinates use a right-handed local ENU frame: X=east,
// Y=north, Z=up. Physical environment values are always expressed in SI
// units; the legacy CLD2 voxel fields intentionally keep their existing
// normalized units.
struct EnvironmentVector {
  float east = 0.0f;
  float north = 0.0f;
  float up = 0.0f;
};

struct GeoLocation {
  double latitude_degrees = 37.5665;
  double longitude_degrees = 126.9780;
  double elevation_m = 38.0;
};

struct WorldDomain {
  float horizontal_extent_m = 20000.0f;
  float vertical_extent_m = 12000.0f;
};

enum class WeatherPreset : std::uint8_t {
  Natural = 0,
  Clear = 1,
  Cumulus = 2,
  Overcast = 3,
  Rain = 4,
  Storm = 5,
  Snow = 6,
  Fog = 7,
  Custom = 8,
};

enum class PrecipitationType : std::uint8_t {
  None = 0,
  Rain = 1,
  Snow = 2,
  Mixed = 3,
};

enum class CloudLayerKind : std::uint8_t {
  None = 0,
  Convective = 1,
  Stratiform = 2,
  Cirrus = 3,
  Fog = 4,
};

constexpr std::size_t kMaximumCloudLayers = 4;

struct CloudLayerState {
  CloudLayerKind kind = CloudLayerKind::None;
  // Internal weather profiles use altitude above the volume floor (AGL).
  // SKS1/SKC1 convert these values to/from absolute AMSL metres on the wire.
  float base_altitude_m = 0.0f;
  float top_altitude_m = 0.0f;
  float coverage = 0.0f;
  float optical_depth = 0.0f;
  float liquid_fraction = 1.0f;
  float precipitation_rate_mm_h = 0.0f;
  float convective_activity = 0.0f;
};

struct CelestialState {
  EnvironmentVector sun_direction{};
  float sun_geometric_elevation_degrees = -90.0f;
  float sun_azimuth_degrees = 0.0f;
  float sun_direct_illuminance_lux = 0.0f;
  float sun_irradiance_w_m2 = 0.0f;
  float sun_angular_radius_degrees = 0.2666f;
  float sun_color_temperature_kelvin = 6500.0f;

  EnvironmentVector moon_direction{};
  float moon_geometric_elevation_degrees = -90.0f;
  float moon_azimuth_degrees = 0.0f;
  float moon_illuminance_lux = 0.0f;
  float moon_angular_radius_degrees = 0.2725f;
  float moon_illuminated_fraction = 0.0f;
  float star_visibility = 0.0f;
};

struct AtmosphereOptics {
  float planet_bottom_radius_m = 6371000.0f;
  float atmosphere_top_radius_m = 6471000.0f;
  float rayleigh_scale_height_m = 8000.0f;
  float mie_scale_height_m = 1200.0f;
  std::array<float, 3> rayleigh_scattering_per_m{
      5.802e-6f, 13.558e-6f, 33.100e-6f};
  std::array<float, 3> mie_scattering_per_m{
      3.996e-6f, 3.996e-6f, 3.996e-6f};
  std::array<float, 3> mie_absorption_per_m{
      4.400e-7f, 4.400e-7f, 4.400e-7f};
  std::array<float, 3> ozone_absorption_per_m{
      0.650e-6f, 1.881e-6f, 0.085e-6f};
  std::array<float, 3> ground_albedo{0.18f, 0.18f, 0.18f};
  float mie_anisotropy = 0.80f;
  float aerosol_optical_depth_550nm = 0.10f;
  float ozone_dobson_units = 300.0f;
};

struct WeatherState {
  WeatherPreset preset = WeatherPreset::Cumulus;
  PrecipitationType precipitation_type = PrecipitationType::None;
  float transition_progress = 1.0f;

  float surface_temperature_kelvin = 293.15f;
  float sea_level_pressure_pa = 101325.0f;
  float relative_humidity = 0.55f;
  float dew_point_kelvin = 283.90f;
  float visibility_m = 35000.0f;
  EnvironmentVector wind_m_s{4.0f, 1.0f, 0.0f};
  float gust_speed_m_s = 6.0f;
  float precipitation_rate_mm_h = 0.0f;
  float snow_fraction = 0.0f;
  float fog_extinction_per_m = 0.0f;
  float lightning_activity = 0.0f;
  float lightning_flash = 0.0f;
  float surface_wetness = 0.0f;
  float convective_activity = 0.45f;
  float aerosol_optical_depth_550nm = 0.10f;
  float ozone_dobson_units = 300.0f;
};

constexpr std::uint32_t kSkyFlagSunAboveHorizon = 1U << 0U;
constexpr std::uint32_t kSkyFlagMoonAboveHorizon = 1U << 1U;
constexpr std::uint32_t kSkyFlagPrecipitation = 1U << 2U;
constexpr std::uint32_t kSkyFlagFog = 1U << 3U;
constexpr std::uint32_t kSkyFlagLightningFlash = 1U << 4U;
constexpr std::uint32_t kSkyFlagSnow = 1U << 5U;
constexpr std::uint32_t kKnownSkyFlags =
    kSkyFlagSunAboveHorizon | kSkyFlagMoonAboveHorizon |
    kSkyFlagPrecipitation | kSkyFlagFog | kSkyFlagLightningFlash |
    kSkyFlagSnow;

struct EnvironmentSnapshot {
  std::uint32_t sequence = 0;
  std::uint32_t flags = 0;
  std::uint32_t lightning_event_id = 0;
  double fluid_time_seconds = 0.0;
  double utc_unix_seconds = 0.0;
  float time_scale = 1.0f;
  GeoLocation location{};
  WorldDomain domain{};
  CelestialState celestial{};
  WeatherState weather{};
  AtmosphereOptics optics{};
  std::array<CloudLayerState, kMaximumCloudLayers> cloud_layers{};
  std::uint8_t cloud_layer_count = 0;
};

} // namespace cloud
