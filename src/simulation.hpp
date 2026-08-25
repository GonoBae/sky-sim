#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace cloud {

struct SimulationStats {
  float minimum = 0.0f;
  float maximum = 0.0f;
  float mean = 0.0f;
};

struct QuantizedVolume {
  int size_x = 0;
  int size_y = 0;
  int size_z = 0;
  std::uint8_t channels = 1;
  float value_scale = 1.0f;
  float value_bias = 0.0f;
  std::vector<std::uint8_t> bytes;
};

struct SimulationVector {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

struct CloudWindLevel {
  float normalized_height = 0.0f;
  SimulationVector velocity_cells_per_second{};
};

enum class CloudForcingLayerKind : std::uint8_t {
  Stratiform = 1,
  Convective = 2,
  Cirrus = 3,
  Fog = 4,
};

struct CloudForcingLayer {
  CloudForcingLayerKind kind = CloudForcingLayerKind::Stratiform;
  float normalized_base = 0.0f;
  float normalized_top = 1.0f;
  float coverage = 0.0f;
  float optical_depth = 0.0f;
  float convective_activity = 0.0f;
};

struct CloudForcing {
  bool enabled = false;

  // The profile is sampled linearly by normalized grid height. Keeping the
  // heights explicit lets a weather model concentrate shear near a boundary
  // layer without making CloudSimulation aware of world-space units.
  std::array<CloudWindLevel, 4> wind_profile{{
      {0.0f, {0.55f, 0.0f, 0.0f}},
      {0.25f, {0.7125f, 0.15f, 0.0f}},
      {0.70f, {1.005f, -0.1427f, 0.0f}},
      {1.0f, {1.20f, 0.0f, 0.0f}},
  }};
  float wind_response_per_second = 0.18f;

  // Cloud layers are clipped to the simulated vertical domain and kept
  // contiguous. They make the fluid density agree with the SKS1 layer
  // metadata instead of injecting every cloud near the ground.
  std::array<CloudForcingLayer, 4> cloud_layers{};
  std::uint8_t cloud_layer_count = 0;

  float thermal_source_multiplier = 1.0f;
  float vapor_source_multiplier = 1.0f;
  // Vertical acceleration is stored in grid cells/s^2. The environment
  // converts its physical m/s^2 targets once, using the active domain and
  // grid height, so changing resolution does not change the weather scale.
  float updraft_acceleration_cells_per_second_squared = 1.0f;
  float buoyancy_acceleration_cells_per_second_squared = 1.0f;

  float surface_temperature_target = 0.0f;
  float top_temperature_target = 0.0f;
  float temperature_target_response_per_second = 0.0f;
  float surface_vapor_target = 0.0f;
  float top_vapor_target = 0.0f;
  float vapor_target_response_per_second = 0.0f;

  float lapse_cooling_per_second = 0.035f;
  float condensation_per_second = 5.0f;
  float evaporation_per_second = 1.4f;
  float cloud_decay_per_second = 0.004f;
  float vapor_decay_per_second = 0.001f;
  float temperature_decay_per_second = 0.055f;
  float precipitation_per_second = 0.0f;
  float precipitation_threshold = 0.65f;
};

struct SimulationQuaternion {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float w = 1.0f;
};

enum class InteractorShape : std::uint8_t {
  Sphere = 1,
  Box = 2,
  CapsuleZ = 3,
  Ellipsoid = 4,
};

constexpr std::uint16_t kInteractorSolid = 1U << 0U;
constexpr std::uint16_t kInteractorDisplaceScalars = 1U << 1U;
constexpr std::uint16_t kInteractorGenerateWake = 1U << 2U;

struct CloudInteractor {
  std::uint64_t id = 0;
  InteractorShape shape = InteractorShape::Ellipsoid;
  std::uint16_t flags =
      kInteractorSolid | kInteractorDisplaceScalars | kInteractorGenerateWake;
  SimulationVector position{0.5f, 0.5f, 0.5f};
  SimulationVector previous_position{0.5f, 0.5f, 0.5f};
  SimulationQuaternion rotation{};
  SimulationQuaternion previous_rotation{};
  bool has_previous_transform = false;
  SimulationVector half_extents{0.03f, 0.03f, 0.03f};
  SimulationVector linear_velocity{};
  SimulationVector angular_velocity{};
  float transform_interval_seconds = 0.0f;
  float influence_radius = 0.04f;
  float displacement_strength = 1.0f;
  float wake_strength = 1.0f;
  float turbulence_strength = 0.35f;
};

struct InteractionStats {
  std::size_t active_interactors = 0;
  std::size_t affected_velocity_cells = 0;
  std::size_t wake_velocity_cells = 0;
  std::size_t displaced_scalar_cells = 0;
  std::size_t invalid_scalar_targets = 0;
  double displaced_cloud_mass = 0.0;
  bool work_budget_exhausted = false;
};

class CloudSimulation {
public:
  explicit CloudSimulation(int size, int pressure_iterations = 12)
      : n_(size), pressure_iterations_(pressure_iterations),
        count_(voxelCount(size)) {
    if (size < 16 || size > 192) {
      throw std::invalid_argument("Grid size must be between 16 and 192");
    }
    if (pressure_iterations < 1 || pressure_iterations > 80) {
      throw std::invalid_argument(
          "Pressure iterations must be between 1 and 80");
    }
    allocateFields();
    reset();
  }

  int size() const { return n_; }
  double time() const { return time_; }
  const InteractionStats &interactionStats() const {
    return interaction_stats_;
  }

  const CloudForcing &environmentalForcing() const {
    return environmental_forcing_;
  }

  void setEnvironmentalForcing(const CloudForcing &forcing) {
    validateEnvironmentalForcing(forcing);
    environmental_forcing_ = forcing;
  }

  void setInteractors(std::vector<CloudInteractor> interactors) {
    if (interactors.size() > 64U) {
      throw std::invalid_argument("At most 64 cloud interactors are supported");
    }
    for (CloudInteractor &interactor : interactors) {
      interactor.position.x = wrapUnit(interactor.position.x);
      interactor.position.y = wrapUnit(interactor.position.y);
      interactor.position.z = std::clamp(interactor.position.z, 0.0f, 1.0f);
      interactor.rotation = normalizeQuaternion(interactor.rotation);
      interactor.transform_interval_seconds =
          std::clamp(interactor.transform_interval_seconds, 0.0f, 0.25f);
      if (interactor.has_previous_transform) {
        interactor.previous_position.x =
            wrapUnit(interactor.previous_position.x);
        interactor.previous_position.y =
            wrapUnit(interactor.previous_position.y);
        interactor.previous_position.z =
            std::clamp(interactor.previous_position.z, 0.0f, 1.0f);
        interactor.previous_rotation =
            normalizeQuaternion(interactor.previous_rotation);
      } else {
        interactor.previous_position = interactor.position;
        interactor.previous_rotation = interactor.rotation;
      }
    }
    std::sort(interactors.begin(), interactors.end(),
              [](const CloudInteractor &a, const CloudInteractor &b) {
                return a.id < b.id;
              });
    interactors_ = std::move(interactors);
  }

  void reset() {
    clear(u_);
    clear(v_);
    clear(w_);
    clear(temperature_);
    clear(vapor_);
    clear(cloud_);
    clear(next_u_);
    clear(next_v_);
    clear(next_w_);
    clear(next_temperature_);
    clear(next_vapor_);
    clear(next_cloud_);
    clear(pressure_);
    clear(next_pressure_);
    clear(divergence_);
    std::fill(solid_mask_.begin(), solid_mask_.end(), 0U);
    std::fill(solid_owner_rank_.begin(), solid_owner_rank_.end(), 0U);
    std::fill(scalar_touched_mask_.begin(), scalar_touched_mask_.end(), 0U);
    scalar_touched_indices_.clear();
    interaction_stats_ = {};
    interaction_cell_budget_ = 0;
    time_ = 0.0f;
    constexpr std::array<float, 4> source_x_fractions{
        0.31f, 0.67f, 0.17f, 0.82f};
    constexpr std::array<float, 4> source_y_fractions{
        0.23f, 0.74f, 0.61f, 0.38f};
    for (std::size_t index = 0; index < layer_source_x_cells_.size();
         ++index) {
      layer_source_x_cells_[index] =
          source_x_fractions[index] * static_cast<float>(n_);
      layer_source_y_cells_[index] =
          source_y_fractions[index] * static_cast<float>(n_);
    }
  }

  void step(float dt) {
    if (!(dt > 0.0f) || dt > 0.25f) {
      throw std::invalid_argument("Simulation dt must be in (0, 0.25]");
    }

    interaction_stats_ = {};
    interaction_stats_.active_interactors = interactors_.size();
    rasterizeInteractorSolids();
    injectThermal(dt);
    applyForces(dt);
    applyInteractorForces(dt);

    advect(u_, next_u_, dt);
    advect(v_, next_v_, dt);
    advect(w_, next_w_, dt);

    u_.swap(next_u_);
    v_.swap(next_v_);
    w_.swap(next_w_);

    rasterizeInteractorSolids();
    projectVelocity();
    applyBoundaries();

    advect(temperature_, next_temperature_, dt);
    advect(vapor_, next_vapor_, dt);
    advect(cloud_, next_cloud_, dt);

    temperature_.swap(next_temperature_);
    vapor_.swap(next_vapor_);
    cloud_.swap(next_cloud_);

    applyThermodynamics(dt);
    displaceScalarsFromInteractors(dt);
    time_ += dt;
    if (!interactors_.empty()) {
      interaction_round_robin_ =
          (interaction_round_robin_ + 1U) % interactors_.size();
    }
  }

  std::vector<std::uint8_t> densityBytes() const {
    std::vector<std::uint8_t> result(count_);
    for (std::size_t i = 0; i < count_; ++i) {
      const float optical_density =
          1.0f - std::exp(-2.4f * std::max(0.0f, cloud_[i]));
      result[i] = static_cast<std::uint8_t>(
          std::lround(255.0f * std::clamp(optical_density, 0.0f, 1.0f)));
    }
    return result;
  }

  QuantizedVolume densityVolume16() const {
    QuantizedVolume result;
    result.size_x = n_;
    result.size_y = n_;
    result.size_z = n_;
    result.channels = 1;
    result.value_scale = positiveMaximum(cloud_, 1.0f);
    result.bytes.resize(count_ * sizeof(std::uint16_t));

    for (std::size_t i = 0; i < count_; ++i) {
      const float normalized =
          std::clamp(cloud_[i] / result.value_scale, 0.0f, 1.0f);
      const auto encoded = static_cast<std::uint16_t>(
          std::lround(normalized * static_cast<float>(UINT16_MAX)));
      result.bytes[i * 2] = static_cast<std::uint8_t>(encoded & 0xffU);
      result.bytes[i * 2 + 1] =
          static_cast<std::uint8_t>((encoded >> 8U) & 0xffU);
    }
    return result;
  }

  QuantizedVolume velocityVolumeSnorm16() const {
    QuantizedVolume result;
    result.size_x = n_;
    result.size_y = n_;
    result.size_z = n_;
    result.channels = 3;
    result.value_scale =
        std::max(1.0f, std::max({absoluteMaximum(u_), absoluteMaximum(v_),
                                 absoluteMaximum(w_)}));
    result.bytes.resize(count_ * result.channels * sizeof(std::int16_t));

    for (std::size_t i = 0; i < count_; ++i) {
      const std::int16_t encoded[3] = {
          encodeSnorm16(u_[i] / result.value_scale),
          encodeSnorm16(v_[i] / result.value_scale),
          encodeSnorm16(w_[i] / result.value_scale),
      };
      for (std::size_t channel = 0; channel < 3; ++channel) {
        const auto bits = static_cast<std::uint16_t>(encoded[channel]);
        const std::size_t offset = (i * 3 + channel) * 2;
        result.bytes[offset] = static_cast<std::uint8_t>(bits & 0xffU);
        result.bytes[offset + 1] =
            static_cast<std::uint8_t>((bits >> 8U) & 0xffU);
      }
    }
    return result;
  }

  QuantizedVolume temperatureVolume8() const {
    return encodeUnorm8(temperature_, -1.0f, 3.0f);
  }

  QuantizedVolume vaporVolume8() const {
    const float maximum = positiveMaximum(vapor_, 1.0f);
    return encodeUnorm8(vapor_, 0.0f, maximum);
  }

  QuantizedVolume occupancyVolume8(int brick_size) const {
    if (brick_size < 1 || brick_size > n_) {
      throw std::invalid_argument("Occupancy brick size is outside the grid");
    }

    QuantizedVolume result;
    result.size_x = (n_ + brick_size - 1) / brick_size;
    result.size_y = result.size_x;
    result.size_z = result.size_x;
    result.channels = 1;
    result.value_scale = positiveMaximum(cloud_, 1.0f);
    result.bytes.resize(static_cast<std::size_t>(result.size_x) *
                        static_cast<std::size_t>(result.size_y) *
                        static_cast<std::size_t>(result.size_z));

    for (int bz = 0; bz < result.size_z; ++bz) {
      for (int by = 0; by < result.size_y; ++by) {
        for (int bx = 0; bx < result.size_x; ++bx) {
          float maximum = 0.0f;
          const int max_z = std::min(n_, (bz + 1) * brick_size);
          const int max_y = std::min(n_, (by + 1) * brick_size);
          const int max_x = std::min(n_, (bx + 1) * brick_size);
          for (int z = bz * brick_size; z < max_z; ++z) {
            for (int y = by * brick_size; y < max_y; ++y) {
              for (int x = bx * brick_size; x < max_x; ++x) {
                maximum = std::max(maximum, cloud_[index(x, y, z)]);
              }
            }
          }
          const std::size_t output_index =
              (static_cast<std::size_t>(bz) *
                   static_cast<std::size_t>(result.size_y) +
               static_cast<std::size_t>(by)) *
                  static_cast<std::size_t>(result.size_x) +
              static_cast<std::size_t>(bx);
          result.bytes[output_index] = static_cast<std::uint8_t>(std::lround(
              255.0f * std::clamp(maximum / result.value_scale, 0.0f, 1.0f)));
        }
      }
    }
    return result;
  }

  SimulationStats densityStats() const {
    SimulationStats stats{};
    stats.minimum = std::numeric_limits<float>::max();
    stats.maximum = std::numeric_limits<float>::lowest();
    double sum = 0.0;
    for (const float value : cloud_) {
      stats.minimum = std::min(stats.minimum, value);
      stats.maximum = std::max(stats.maximum, value);
      sum += value;
    }
    stats.mean = static_cast<float>(sum / static_cast<double>(count_));
    return stats;
  }

  float densityCenterOfMassNormalizedHeight() const {
    double weighted_height = 0.0;
    double total_mass = 0.0;
    for (int z = 0; z < n_; ++z) {
      const double normalized_height =
          static_cast<double>(z) / static_cast<double>(n_ - 1);
      for (int y = 0; y < n_; ++y) {
        for (int x = 0; x < n_; ++x) {
          const double mass =
              std::max(0.0, static_cast<double>(cloud_[index(x, y, z)]));
          weighted_height += normalized_height * mass;
          total_mass += mass;
        }
      }
    }
    return total_mass > 1.0e-12
               ? static_cast<float>(weighted_height / total_mass)
               : -1.0f;
  }

  bool allFinite() const {
    return finiteField(u_) && finiteField(v_) && finiteField(w_) &&
           finiteField(temperature_) && finiteField(vapor_) &&
           finiteField(cloud_);
  }

private:
  static std::size_t voxelCount(int n) {
    return static_cast<std::size_t>(n) * static_cast<std::size_t>(n) *
           static_cast<std::size_t>(n);
  }

  void allocateFields() {
    auto allocate = [this](std::vector<float> &field) {
      field.resize(count_, 0.0f);
    };
    allocate(u_);
    allocate(v_);
    allocate(w_);
    allocate(temperature_);
    allocate(vapor_);
    allocate(cloud_);
    allocate(next_u_);
    allocate(next_v_);
    allocate(next_w_);
    allocate(next_temperature_);
    allocate(next_vapor_);
    allocate(next_cloud_);
    allocate(pressure_);
    allocate(next_pressure_);
    allocate(divergence_);
    solid_mask_.resize(count_, 0U);
    solid_owner_rank_.resize(count_, 0U);
    scalar_touched_mask_.resize(count_, 0U);
  }

  static void clear(std::vector<float> &field) {
    std::fill(field.begin(), field.end(), 0.0f);
  }

  static float wrapUnit(float value) {
    value -= std::floor(value);
    return value < 0.0f ? value + 1.0f : value;
  }

  static void validateEnvironmentalForcing(const CloudForcing &forcing) {
    if (!forcing.enabled) {
      return;
    }

    float previous_height = -1.0f;
    for (const CloudWindLevel &level : forcing.wind_profile) {
      if (!std::isfinite(level.normalized_height) ||
          level.normalized_height < 0.0f || level.normalized_height > 1.0f ||
          level.normalized_height <= previous_height ||
          !std::isfinite(level.velocity_cells_per_second.x) ||
          !std::isfinite(level.velocity_cells_per_second.y) ||
          !std::isfinite(level.velocity_cells_per_second.z)) {
        throw std::invalid_argument(
            "Cloud forcing wind levels must be finite, ordered, and in [0, 1]");
      }
      previous_height = level.normalized_height;
    }

    if (forcing.cloud_layer_count > forcing.cloud_layers.size()) {
      throw std::invalid_argument("Cloud forcing supports at most four layers");
    }
    for (std::size_t index = 0; index < forcing.cloud_layer_count; ++index) {
      const CloudForcingLayer &layer = forcing.cloud_layers[index];
      const auto kind = static_cast<std::uint8_t>(layer.kind);
      if (kind < static_cast<std::uint8_t>(
                     CloudForcingLayerKind::Stratiform) ||
          kind > static_cast<std::uint8_t>(CloudForcingLayerKind::Fog) ||
          !std::isfinite(layer.normalized_base) ||
          !std::isfinite(layer.normalized_top) ||
          !std::isfinite(layer.coverage) ||
          !std::isfinite(layer.optical_depth) ||
          !std::isfinite(layer.convective_activity) ||
          layer.normalized_base < 0.0f ||
          layer.normalized_top <= layer.normalized_base ||
          layer.normalized_top > 1.0f || layer.coverage < 0.0f ||
          layer.coverage > 1.0f || layer.optical_depth < 0.0f ||
          layer.optical_depth > 500.0f ||
          layer.convective_activity < 0.0f ||
          layer.convective_activity > 1.0f) {
        throw std::invalid_argument(
            "Cloud forcing layers must have valid kind, height, and strength");
      }
    }

    if (!std::isfinite(forcing.surface_temperature_target) ||
        !std::isfinite(forcing.top_temperature_target)) {
      throw std::invalid_argument(
          "Cloud forcing temperature targets must be finite");
    }

    const std::array<float, 17> nonnegative_values{{
        forcing.wind_response_per_second,
        forcing.thermal_source_multiplier,
        forcing.vapor_source_multiplier,
        forcing.updraft_acceleration_cells_per_second_squared,
        forcing.buoyancy_acceleration_cells_per_second_squared,
        forcing.temperature_target_response_per_second,
        forcing.surface_vapor_target,
        forcing.top_vapor_target,
        forcing.vapor_target_response_per_second,
        forcing.lapse_cooling_per_second,
        forcing.condensation_per_second,
        forcing.evaporation_per_second,
        forcing.cloud_decay_per_second,
        forcing.vapor_decay_per_second,
        forcing.temperature_decay_per_second,
        forcing.precipitation_per_second,
        forcing.precipitation_threshold,
    }};
    for (const float value : nonnegative_values) {
      if (!std::isfinite(value) || value < 0.0f) {
        throw std::invalid_argument(
            "Cloud forcing rates, multipliers, and vapor values must be finite and non-negative");
      }
    }
  }

  static SimulationVector sampleWindProfile(const CloudForcing &forcing,
                                            float normalized_height) {
    const auto &profile = forcing.wind_profile;
    if (normalized_height <= profile.front().normalized_height) {
      return profile.front().velocity_cells_per_second;
    }
    for (std::size_t i = 1; i < profile.size(); ++i) {
      if (normalized_height <= profile[i].normalized_height) {
        const CloudWindLevel &lower = profile[i - 1U];
        const CloudWindLevel &upper = profile[i];
        const float span = upper.normalized_height - lower.normalized_height;
        const float alpha =
            (normalized_height - lower.normalized_height) / span;
        return {
            lower.velocity_cells_per_second.x +
                (upper.velocity_cells_per_second.x -
                 lower.velocity_cells_per_second.x) *
                    alpha,
            lower.velocity_cells_per_second.y +
                (upper.velocity_cells_per_second.y -
                 lower.velocity_cells_per_second.y) *
                    alpha,
            lower.velocity_cells_per_second.z +
                (upper.velocity_cells_per_second.z -
                 lower.velocity_cells_per_second.z) *
                    alpha,
        };
      }
    }
    return profile.back().velocity_cells_per_second;
  }

  static float smoothUnit(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    return value * value * (3.0f - 2.0f * value);
  }

  static float layerSupportAtHeight(const CloudForcing &forcing,
                                    float normalized_height,
                                    float minimum_feather) {
    float support = 0.0f;
    for (std::size_t index = 0; index < forcing.cloud_layer_count; ++index) {
      const CloudForcingLayer &layer = forcing.cloud_layers[index];
      const float feather = minimum_feather;
      const float lower = smoothUnit(
          (normalized_height - (layer.normalized_base - feather)) / feather);
      const float upper = smoothUnit(
          ((layer.normalized_top + feather) - normalized_height) / feather);
      const float coverage_support =
          std::sqrt(std::clamp(layer.coverage, 0.0f, 1.0f));
      support = std::max(support, lower * upper * coverage_support);
    }
    return std::clamp(support, 0.0f, 1.0f);
  }

  static SimulationVector add(SimulationVector a, SimulationVector b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
  }

  static SimulationVector subtract(SimulationVector a, SimulationVector b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
  }

  static SimulationVector multiply(SimulationVector value, float scale) {
    return {value.x * scale, value.y * scale, value.z * scale};
  }

  static float dot(SimulationVector a, SimulationVector b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
  }

  static SimulationVector cross(SimulationVector a, SimulationVector b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
  }

  static float vectorLength(SimulationVector value) {
    return std::sqrt(dot(value, value));
  }

  static SimulationVector normalize(SimulationVector value,
                                    SimulationVector fallback = {1, 0, 0}) {
    const float length = vectorLength(value);
    return length > 0.000001f ? multiply(value, 1.0f / length) : fallback;
  }

  static SimulationQuaternion normalizeQuaternion(SimulationQuaternion value) {
    const float length = std::sqrt(value.x * value.x + value.y * value.y +
                                   value.z * value.z + value.w * value.w);
    if (length <= 0.000001f) {
      return {};
    }
    const float inverse = 1.0f / length;
    value.x *= inverse;
    value.y *= inverse;
    value.z *= inverse;
    value.w *= inverse;
    return value;
  }

  static SimulationVector rotateVector(const SimulationQuaternion &rotation,
                                       SimulationVector value) {
    const SimulationVector axis{rotation.x, rotation.y, rotation.z};
    const SimulationVector twice_cross = multiply(cross(axis, value), 2.0f);
    return add(value, add(multiply(twice_cross, rotation.w),
                          cross(axis, twice_cross)));
  }

  static SimulationVector
  inverseRotateVector(const SimulationQuaternion &rotation,
                      SimulationVector value) {
    const SimulationQuaternion inverse{-rotation.x, -rotation.y, -rotation.z,
                                       rotation.w};
    return rotateVector(inverse, value);
  }

  static SimulationQuaternion interpolateQuaternion(SimulationQuaternion a,
                                                    SimulationQuaternion b,
                                                    float t) {
    const float product = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (product < 0.0f) {
      b = {-b.x, -b.y, -b.z, -b.w};
    }
    return normalizeQuaternion({
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t,
        a.w + (b.w - a.w) * t,
    });
  }

  static SimulationQuaternion
  multiplyQuaternions(const SimulationQuaternion &a,
                      const SimulationQuaternion &b) {
    return normalizeQuaternion({
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    });
  }

  static SimulationQuaternion angleAxisQuaternion(SimulationVector axis,
                                                  float angle) {
    axis = normalize(axis);
    const float half_angle = 0.5f * angle;
    const float sine = std::sin(half_angle);
    return {axis.x * sine, axis.y * sine, axis.z * sine, std::cos(half_angle)};
  }

  static float shortestUnitDelta(float destination, float source) {
    float delta = destination - source;
    if (delta > 0.5f) {
      delta -= 1.0f;
    } else if (delta < -0.5f) {
      delta += 1.0f;
    }
    return delta;
  }

  static float motionAwareUnitDelta(float destination, float source,
                                    float reported_velocity,
                                    float elapsed_seconds) {
    const float raw_delta = destination - source;
    if (elapsed_seconds > 0.0f && std::isfinite(elapsed_seconds) &&
        std::isfinite(reported_velocity)) {
      const float expected_delta = reported_velocity * elapsed_seconds;
      return raw_delta + std::round(expected_delta - raw_delta);
    }
    return shortestUnitDelta(destination, source);
  }

  float maximumExtentCells(const CloudInteractor &interactor) const {
    const SimulationVector extent = interactorExtentCells(interactor);
    return std::max({extent.x, extent.y, extent.z});
  }

  float geometricBoundingRadiusCells(const CloudInteractor &interactor) const {
    const SimulationVector extent = interactorExtentCells(interactor);
    if (interactor.shape == InteractorShape::Box) {
      return vectorLength(extent);
    }
    return std::max({extent.x, extent.y, extent.z});
  }

  SimulationVector
  worldAabbExtentCells(const CloudInteractor &interactor) const {
    const SimulationVector extent = interactorExtentCells(interactor);
    const SimulationVector axis_x =
        rotateVector(interactor.rotation, {1.0f, 0.0f, 0.0f});
    const SimulationVector axis_y =
        rotateVector(interactor.rotation, {0.0f, 1.0f, 0.0f});
    const SimulationVector axis_z =
        rotateVector(interactor.rotation, {0.0f, 0.0f, 1.0f});
    if (interactor.shape == InteractorShape::Sphere) {
      const float radius = std::max({extent.x, extent.y, extent.z});
      return {radius, radius, radius};
    }
    if (interactor.shape == InteractorShape::CapsuleZ) {
      const float radius = std::max(1.5f, 0.5f * (extent.x + extent.y));
      const float half_segment = std::max(0.0f, extent.z - radius);
      return {
          radius + std::abs(axis_z.x) * half_segment,
          radius + std::abs(axis_z.y) * half_segment,
          radius + std::abs(axis_z.z) * half_segment,
      };
    }
    if (interactor.shape == InteractorShape::Ellipsoid) {
      return {
          std::sqrt(axis_x.x * axis_x.x * extent.x * extent.x +
                    axis_y.x * axis_y.x * extent.y * extent.y +
                    axis_z.x * axis_z.x * extent.z * extent.z),
          std::sqrt(axis_x.y * axis_x.y * extent.x * extent.x +
                    axis_y.y * axis_y.y * extent.y * extent.y +
                    axis_z.y * axis_z.y * extent.z * extent.z),
          std::sqrt(axis_x.z * axis_x.z * extent.x * extent.x +
                    axis_y.z * axis_y.z * extent.y * extent.y +
                    axis_z.z * axis_z.z * extent.z * extent.z),
      };
    }
    return {
        std::abs(axis_x.x) * extent.x + std::abs(axis_y.x) * extent.y +
            std::abs(axis_z.x) * extent.z,
        std::abs(axis_x.y) * extent.x + std::abs(axis_y.y) * extent.y +
            std::abs(axis_z.y) * extent.z,
        std::abs(axis_x.z) * extent.x + std::abs(axis_y.z) * extent.y +
            std::abs(axis_z.z) * extent.z,
    };
  }

  SimulationVector
  interactorExtentCells(const CloudInteractor &interactor) const {
    return {
        std::max(1.5f, interactor.half_extents.x * static_cast<float>(n_)),
        std::max(1.5f, interactor.half_extents.y * static_cast<float>(n_)),
        std::max(1.5f, interactor.half_extents.z * static_cast<float>(n_)),
    };
  }

  template <typename Function>
  void forEachSweptInteractor(const CloudInteractor &interactor,
                              bool current_first, Function &&function) const {
    if (!interactor.has_previous_transform) {
      function(interactor, 1.0f, true);
      return;
    }
    const float elapsed_seconds = interactor.transform_interval_seconds;
    const SimulationVector normalized_delta{
        motionAwareUnitDelta(interactor.position.x,
                             interactor.previous_position.x,
                             interactor.linear_velocity.x, elapsed_seconds),
        motionAwareUnitDelta(interactor.position.y,
                             interactor.previous_position.y,
                             interactor.linear_velocity.y, elapsed_seconds),
        interactor.position.z - interactor.previous_position.z,
    };
    const float translation_distance_cells = vectorLength({
        normalized_delta.x * static_cast<float>(n_),
        normalized_delta.y * static_cast<float>(n_),
        normalized_delta.z * static_cast<float>(n_ - 1),
    });
    const float quaternion_product =
        std::abs(interactor.previous_rotation.x * interactor.rotation.x +
                 interactor.previous_rotation.y * interactor.rotation.y +
                 interactor.previous_rotation.z * interactor.rotation.z +
                 interactor.previous_rotation.w * interactor.rotation.w);
    const float rotation_angle =
        2.0f * std::acos(std::clamp(quaternion_product, 0.0f, 1.0f));
    const float angular_speed = vectorLength(interactor.angular_velocity);
    const float integrated_rotation_angle = angular_speed * elapsed_seconds;
    const bool use_integrated_rotation =
        elapsed_seconds > 0.0f && angular_speed > 0.0001f &&
        integrated_rotation_angle > rotation_angle + 0.25f;
    const float swept_rotation_angle =
        use_integrated_rotation ? integrated_rotation_angle : rotation_angle;
    const float rotation_distance_cells =
        swept_rotation_angle * geometricBoundingRadiusCells(interactor);
    const float distance_cells =
        translation_distance_cells + rotation_distance_cells;
    const float timing_window =
        elapsed_seconds > 0.0f ? elapsed_seconds : 0.15f;
    const float reported_travel =
        vectorLength(interactor.linear_velocity) * timing_window +
        2.0f * std::max({
                   interactor.half_extents.x,
                   interactor.half_extents.y,
                   interactor.half_extents.z,
               }) +
        2.0f / static_cast<float>(n_);
    const float observed_travel = vectorLength(normalized_delta);
    const float reported_rotation =
        vectorLength(interactor.angular_velocity) * timing_window + 0.25f;
    const bool teleport =
        observed_travel > std::max(0.20f, reported_travel) ||
        swept_rotation_angle > std::max(0.50f, reported_rotation);
    const SimulationVector extent_cells = interactorExtentCells(interactor);
    const float sweep_spacing = std::max(
        0.75f,
        0.5f * std::min({extent_cells.x, extent_cells.y, extent_cells.z}));
    const int sample_count =
        teleport ? 1
                 : std::clamp(static_cast<int>(
                                  std::ceil(distance_cells / sweep_spacing)) +
                                  1,
                              1, 48);
    for (int sample = 0; sample < sample_count; ++sample) {
      const int chronological_sample =
          current_first ? (sample == 0 ? sample_count - 1 : sample - 1)
                        : sample;
      const float t = sample_count == 1
                          ? 1.0f
                          : static_cast<float>(chronological_sample) /
                                static_cast<float>(sample_count - 1);
      CloudInteractor swept = interactor;
      swept.position = {
          wrapUnit(interactor.previous_position.x + normalized_delta.x * t),
          wrapUnit(interactor.previous_position.y + normalized_delta.y * t),
          std::clamp(interactor.previous_position.z + normalized_delta.z * t,
                     0.0f, 1.0f),
      };
      if (use_integrated_rotation && t < 1.0f) {
        const SimulationQuaternion delta_rotation = angleAxisQuaternion(
            interactor.angular_velocity, integrated_rotation_angle * t);
        swept.rotation =
            multiplyQuaternions(delta_rotation, interactor.previous_rotation);
      } else {
        swept.rotation = interpolateQuaternion(interactor.previous_rotation,
                                               interactor.rotation, t);
      }
      swept.has_previous_transform = false;
      function(swept, 1.0f / static_cast<float>(sample_count),
               chronological_sample == sample_count - 1);
    }
  }

  static float positiveMaximum(const std::vector<float> &field,
                               float fallback) {
    const float maximum = *std::max_element(field.begin(), field.end());
    return maximum > 0.000001f ? maximum : fallback;
  }

  static float absoluteMaximum(const std::vector<float> &field) {
    float maximum = 0.0f;
    for (const float value : field) {
      maximum = std::max(maximum, std::abs(value));
    }
    return maximum;
  }

  static std::int16_t encodeSnorm16(float value) {
    return static_cast<std::int16_t>(
        std::lround(32767.0f * std::clamp(value, -1.0f, 1.0f)));
  }

  QuantizedVolume encodeUnorm8(const std::vector<float> &field, float minimum,
                               float maximum) const {
    QuantizedVolume result;
    result.size_x = n_;
    result.size_y = n_;
    result.size_z = n_;
    result.channels = 1;
    result.value_scale = maximum - minimum;
    result.value_bias = minimum;
    result.bytes.resize(count_);

    const float inverse_range = 1.0f / (maximum - minimum);
    for (std::size_t i = 0; i < count_; ++i) {
      const float normalized =
          std::clamp((field[i] - minimum) * inverse_range, 0.0f, 1.0f);
      result.bytes[i] =
          static_cast<std::uint8_t>(std::lround(255.0f * normalized));
    }
    return result;
  }

  int wrapHorizontal(int value) const {
    value %= n_;
    return value < 0 ? value + n_ : value;
  }

  float wrapHorizontalCoordinate(float value) const {
    value = std::fmod(value, static_cast<float>(n_));
    return value < 0.0f ? value + static_cast<float>(n_) : value;
  }

  int clampVertical(int value) const { return std::clamp(value, 0, n_ - 1); }

  std::size_t index(int x, int y, int z) const {
    x = wrapHorizontal(x);
    y = wrapHorizontal(y);
    z = clampVertical(z);
    return (static_cast<std::size_t>(z) * static_cast<std::size_t>(n_) +
            static_cast<std::size_t>(y)) *
               static_cast<std::size_t>(n_) +
           static_cast<std::size_t>(x);
  }

  struct ShapeSample {
    float signed_distance = 0.0f;
    SimulationVector normal{1.0f, 0.0f, 0.0f};
    SimulationVector delta{};
  };

  float periodicCellDelta(float coordinate, float center) const {
    float delta = coordinate - center;
    const float half_size = static_cast<float>(n_) * 0.5f;
    if (delta > half_size) {
      delta -= static_cast<float>(n_);
    } else if (delta < -half_size) {
      delta += static_cast<float>(n_);
    }
    return delta;
  }

  ShapeSample sampleInteractorShape(const CloudInteractor &interactor, int x,
                                    int y, int z) const {
    const SimulationVector center{
        interactor.position.x * static_cast<float>(n_),
        interactor.position.y * static_cast<float>(n_),
        interactor.position.z * static_cast<float>(n_ - 1),
    };
    ShapeSample sample;
    sample.delta = {
        periodicCellDelta(static_cast<float>(x), center.x),
        periodicCellDelta(static_cast<float>(y), center.y),
        static_cast<float>(z) - center.z,
    };
    const SimulationVector local =
        inverseRotateVector(interactor.rotation, sample.delta);
    const SimulationVector extent = interactorExtentCells(interactor);

    SimulationVector local_normal;
    if (interactor.shape == InteractorShape::Sphere) {
      const float radius = std::max({extent.x, extent.y, extent.z});
      const float distance = vectorLength(local);
      sample.signed_distance = distance - radius;
      local_normal = normalize(local);
    } else if (interactor.shape == InteractorShape::CapsuleZ) {
      const float radius = std::max(1.5f, 0.5f * (extent.x + extent.y));
      const float half_segment = std::max(0.0f, extent.z - radius);
      const SimulationVector closest{
          0.0f, 0.0f, std::clamp(local.z, -half_segment, half_segment)};
      const SimulationVector from_axis = subtract(local, closest);
      sample.signed_distance = vectorLength(from_axis) - radius;
      local_normal = normalize(from_axis, {1.0f, 0.0f, 0.0f});
    } else if (interactor.shape == InteractorShape::Box) {
      const SimulationVector outside{
          std::max(std::abs(local.x) - extent.x, 0.0f),
          std::max(std::abs(local.y) - extent.y, 0.0f),
          std::max(std::abs(local.z) - extent.z, 0.0f),
      };
      const SimulationVector q{
          std::abs(local.x) - extent.x,
          std::abs(local.y) - extent.y,
          std::abs(local.z) - extent.z,
      };
      sample.signed_distance =
          vectorLength(outside) + std::min(std::max({q.x, q.y, q.z}), 0.0f);
      if (vectorLength(outside) > 0.000001f) {
        local_normal = normalize({
            std::copysign(outside.x, local.x),
            std::copysign(outside.y, local.y),
            std::copysign(outside.z, local.z),
        });
      } else if (q.x >= q.y && q.x >= q.z) {
        local_normal = {std::copysign(1.0f, local.x), 0.0f, 0.0f};
      } else if (q.y >= q.z) {
        local_normal = {0.0f, std::copysign(1.0f, local.y), 0.0f};
      } else {
        local_normal = {0.0f, 0.0f, std::copysign(1.0f, local.z)};
      }
    } else {
      const SimulationVector normalized_local{
          local.x / extent.x, local.y / extent.y, local.z / extent.z};
      const float ellipsoid_radius = vectorLength(normalized_local);
      sample.signed_distance =
          (ellipsoid_radius - 1.0f) * std::min({extent.x, extent.y, extent.z});
      SimulationVector center_fallback{1.0f, 0.0f, 0.0f};
      if (extent.y <= extent.x && extent.y <= extent.z) {
        center_fallback = {0.0f, 1.0f, 0.0f};
      } else if (extent.z <= extent.x && extent.z <= extent.y) {
        center_fallback = {0.0f, 0.0f, 1.0f};
      }
      local_normal = normalize(
          {
              local.x / (extent.x * extent.x),
              local.y / (extent.y * extent.y),
              local.z / (extent.z * extent.z),
          },
          center_fallback);
    }
    sample.normal = normalize(rotateVector(interactor.rotation, local_normal));
    return sample;
  }

  float scalarPushDistance(const CloudInteractor &interactor,
                           const ShapeSample &shape) const {
    if (interactor.shape != InteractorShape::Ellipsoid) {
      return -shape.signed_distance + 1.5f;
    }
    const SimulationVector extent = interactorExtentCells(interactor);
    const SimulationVector local =
        inverseRotateVector(interactor.rotation, shape.delta);
    const SimulationVector direction =
        normalize(inverseRotateVector(interactor.rotation, shape.normal));
    const float inverse_x2 = 1.0f / (extent.x * extent.x);
    const float inverse_y2 = 1.0f / (extent.y * extent.y);
    const float inverse_z2 = 1.0f / (extent.z * extent.z);
    const float a = direction.x * direction.x * inverse_x2 +
                    direction.y * direction.y * inverse_y2 +
                    direction.z * direction.z * inverse_z2;
    const float b = 2.0f * (local.x * direction.x * inverse_x2 +
                            local.y * direction.y * inverse_y2 +
                            local.z * direction.z * inverse_z2);
    const float c = local.x * local.x * inverse_x2 +
                    local.y * local.y * inverse_y2 +
                    local.z * local.z * inverse_z2 - 1.0f;
    const float discriminant = std::max(0.0f, b * b - 4.0f * a * c);
    const float exit_distance =
        a > 0.000001f ? (-b + std::sqrt(discriminant)) / (2.0f * a)
                      : -shape.signed_distance;
    return std::max(0.0f, exit_distance) + 1.5f;
  }

  SimulationVector
  interactorLinearVelocityCells(const CloudInteractor &interactor) const {
    return {
        interactor.linear_velocity.x * static_cast<float>(n_),
        interactor.linear_velocity.y * static_cast<float>(n_),
        interactor.linear_velocity.z * static_cast<float>(n_),
    };
  }

  SimulationVector interactorPointVelocity(const CloudInteractor &interactor,
                                           const ShapeSample &sample) const {
    return add(interactorLinearVelocityCells(interactor),
               cross(interactor.angular_velocity, sample.delta));
  }

  template <typename Function>
  void forEachCellInBounds(const CloudInteractor &interactor,
                           SimulationVector radius, Function &&function) {
    const float center_x = interactor.position.x * static_cast<float>(n_);
    const float center_y = interactor.position.y * static_cast<float>(n_);
    const float center_z = interactor.position.z * static_cast<float>(n_ - 1);
    radius.x = std::clamp(radius.x, 1.0f, static_cast<float>(n_) * 0.49f);
    radius.y = std::clamp(radius.y, 1.0f, static_cast<float>(n_) * 0.49f);
    radius.z = std::clamp(radius.z, 1.0f, static_cast<float>(n_));
    int min_x = static_cast<int>(std::floor(center_x - radius.x));
    int max_x = static_cast<int>(std::ceil(center_x + radius.x));
    int min_y = static_cast<int>(std::floor(center_y - radius.y));
    int max_y = static_cast<int>(std::ceil(center_y + radius.y));
    const int min_z =
        std::max(0, static_cast<int>(std::floor(center_z - radius.z)));
    const int max_z =
        std::min(n_ - 1, static_cast<int>(std::ceil(center_z + radius.z)));
    if (max_x - min_x + 1 > n_) {
      max_x = min_x + n_ - 1;
    }
    if (max_y - min_y + 1 > n_) {
      max_y = min_y + n_ - 1;
    }
    for (int z = min_z; z <= max_z; ++z) {
      for (int y = min_y; y <= max_y; ++y) {
        for (int x = min_x; x <= max_x; ++x) {
          if (!consumeInteractionCellWork()) {
            return;
          }
          function(x, y, z);
        }
      }
    }
  }

  bool consumeInteractionCellWork() {
    if (interaction_cell_budget_ == 0U) {
      interaction_stats_.work_budget_exhausted = true;
      return false;
    }
    --interaction_cell_budget_;
    return true;
  }

  void beginInteractionWorkBudget(std::size_t volume_multiples) {
    if (volume_multiples == 0U ||
        count_ > std::numeric_limits<std::size_t>::max() / volume_multiples) {
      interaction_cell_budget_ = std::numeric_limits<std::size_t>::max();
      return;
    }
    interaction_cell_budget_ = count_ * volume_multiples;
  }

  float sample(const std::vector<float> &field, float x, float y,
               float z) const {
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const int z0 = static_cast<int>(std::floor(z));
    const int x1 = x0 + 1;
    const int y1 = y0 + 1;
    const int z1 = z0 + 1;
    const float tx = x - std::floor(x);
    const float ty = y - std::floor(y);
    const float tz = z - std::floor(z);

    const auto mix = [](float a, float b, float t) { return a + (b - a) * t; };
    const float c000 = field[index(x0, y0, z0)];
    const float c100 = field[index(x1, y0, z0)];
    const float c010 = field[index(x0, y1, z0)];
    const float c110 = field[index(x1, y1, z0)];
    const float c001 = field[index(x0, y0, z1)];
    const float c101 = field[index(x1, y0, z1)];
    const float c011 = field[index(x0, y1, z1)];
    const float c111 = field[index(x1, y1, z1)];

    const float c00 = mix(c000, c100, tx);
    const float c10 = mix(c010, c110, tx);
    const float c01 = mix(c001, c101, tx);
    const float c11 = mix(c011, c111, tx);
    return mix(mix(c00, c10, ty), mix(c01, c11, ty), tz);
  }

  void advect(const std::vector<float> &source, std::vector<float> &destination,
              float dt) {
    for (int z = 0; z < n_; ++z) {
      for (int y = 0; y < n_; ++y) {
        for (int x = 0; x < n_; ++x) {
          const std::size_t i = index(x, y, z);
          const float back_x = static_cast<float>(x) - dt * u_[i];
          const float back_y = static_cast<float>(y) - dt * v_[i];
          const float back_z = std::clamp(static_cast<float>(z) - dt * w_[i],
                                          0.0f, static_cast<float>(n_ - 1));
          destination[i] = sample(source, back_x, back_y, back_z);
        }
      }
    }
  }

  void rasterizeInteractorSolids() {
    beginInteractionWorkBudget(4U);
    std::fill(solid_mask_.begin(), solid_mask_.end(), 0U);
    std::fill(solid_owner_rank_.begin(), solid_owner_rank_.end(), 0U);
    std::fill(divergence_.begin(), divergence_.end(),
              std::numeric_limits<float>::max());
    for (std::size_t offset = 0; offset < interactors_.size(); ++offset) {
      const std::size_t interactor_index =
          (interaction_round_robin_ + offset) % interactors_.size();
      const CloudInteractor &interactor = interactors_[interactor_index];
      const auto owner_rank = static_cast<std::uint8_t>(interactor_index + 1U);
      if ((interactor.flags & kInteractorSolid) == 0U) {
        continue;
      }
      const SimulationVector bounds =
          add(worldAabbExtentCells(interactor), {1.0f, 1.0f, 1.0f});
      forEachCellInBounds(interactor, bounds, [&](int x, int y, int z) {
        const ShapeSample shape = sampleInteractorShape(interactor, x, y, z);
        if (shape.signed_distance >= 0.0f) {
          return;
        }
        const std::size_t i = index(x, y, z);
        if (solid_mask_[i] != 0U) {
          const float distance_difference =
              shape.signed_distance - divergence_[i];
          if (distance_difference > 0.000001f ||
              (std::abs(distance_difference) <= 0.000001f &&
               owner_rank >= solid_owner_rank_[i])) {
            return;
          }
        }
        const SimulationVector point_velocity =
            interactorPointVelocity(interactor, shape);
        solid_mask_[i] = 1U;
        solid_owner_rank_[i] = owner_rank;
        divergence_[i] = shape.signed_distance;
        u_[i] = point_velocity.x;
        v_[i] = point_velocity.y;
        w_[i] = point_velocity.z;
      });
    }
  }

  void injectThermal(float dt) {
    if (!environmental_forcing_.enabled) {
      const float cx = static_cast<float>(n_) * static_cast<float>(
                                                  0.50 + 0.08 *
                                                             std::sin(
                                                                 time_ *
                                                                 0.19));
      const float cy = static_cast<float>(n_) * static_cast<float>(
                                                  0.50 + 0.06 *
                                                             std::cos(
                                                                 time_ *
                                                                 0.17));
      const float cz = static_cast<float>(n_) * 0.10f;
      const float radius =
          std::max(2.0f, static_cast<float>(n_) * 0.10f);
      const float inverse_radius_squared = 1.0f / (radius * radius);
      const int min_x = static_cast<int>(std::floor(cx - radius));
      const int max_x = static_cast<int>(std::ceil(cx + radius));
      const int min_y = static_cast<int>(std::floor(cy - radius));
      const int max_y = static_cast<int>(std::ceil(cy + radius));
      const int min_z =
          std::max(1, static_cast<int>(std::floor(cz - radius * 0.6f)));
      const int max_z = std::min(
          n_ - 2, static_cast<int>(std::ceil(cz + radius * 0.6f)));
      for (int z = min_z; z <= max_z; ++z) {
        for (int y = min_y; y <= max_y; ++y) {
          for (int x = min_x; x <= max_x; ++x) {
            const float dx = static_cast<float>(x) - cx;
            const float dy = static_cast<float>(y) - cy;
            const float dz = (static_cast<float>(z) - cz) * 1.5f;
            const float r2 =
                (dx * dx + dy * dy + dz * dz) * inverse_radius_squared;
            if (r2 > 1.0f) {
              continue;
            }
            const float weight = std::exp(-3.0f * r2) * (1.0f - r2);
            const std::size_t i = index(x, y, z);
            if (solid_mask_[i] != 0U) {
              continue;
            }
            temperature_[i] += dt * 0.85f * weight;
            vapor_[i] += dt * 2.2f * weight;
            w_[i] += dt * 1.6f * weight;
          }
        }
      }
      return;
    }

    float total_layer_coverage = 0.0f;
    for (std::size_t layer_index = 0;
         layer_index < environmental_forcing_.cloud_layer_count;
         ++layer_index) {
      total_layer_coverage +=
          environmental_forcing_.cloud_layers[layer_index].coverage;
    }
    const float source_normalization =
        1.0f / std::sqrt(std::max(1.0f, total_layer_coverage));

    for (std::size_t layer_index = 0;
         layer_index < environmental_forcing_.cloud_layer_count;
         ++layer_index) {
      const CloudForcingLayer &layer =
          environmental_forcing_.cloud_layers[layer_index];
      float center_fraction = 0.35f;
      float thermal_factor = 0.25f;
      float vapor_factor = 0.90f;
      float updraft_factor = 0.15f;
      float activity_factor = 1.0f;
      switch (layer.kind) {
      case CloudForcingLayerKind::Convective:
        center_fraction = 0.16f;
        activity_factor = 0.25f + 0.75f * layer.convective_activity;
        thermal_factor = activity_factor;
        vapor_factor = 1.0f;
        updraft_factor = 0.10f + 0.90f * layer.convective_activity;
        break;
      case CloudForcingLayerKind::Cirrus:
        center_fraction = 0.50f;
        thermal_factor = 0.04f;
        vapor_factor = 0.28f;
        updraft_factor = 0.02f;
        break;
      case CloudForcingLayerKind::Fog:
        center_fraction = 0.30f;
        thermal_factor = 0.0f;
        vapor_factor = 1.10f;
        updraft_factor = 0.0f;
        break;
      case CloudForcingLayerKind::Stratiform:
        break;
      }
      const float normalized_center =
          layer.normalized_base +
          (layer.normalized_top - layer.normalized_base) * center_fraction;
      const SimulationVector source_wind =
          sampleWindProfile(environmental_forcing_, normalized_center);
      layer_source_x_cells_[layer_index] = wrapHorizontalCoordinate(
          layer_source_x_cells_[layer_index] + source_wind.x * dt);
      layer_source_y_cells_[layer_index] = wrapHorizontalCoordinate(
          layer_source_y_cells_[layer_index] + source_wind.y * dt);
      const float cx = layer_source_x_cells_[layer_index];
      const float cy = layer_source_y_cells_[layer_index];
      const float cz =
          normalized_center * static_cast<float>(n_ - 1);
      const float horizontal_radius = std::max(
          2.0f, static_cast<float>(n_) *
                    (0.055f + 0.070f * std::sqrt(layer.coverage)));
      const float vertical_radius = std::clamp(
          (layer.normalized_top - layer.normalized_base) *
              static_cast<float>(n_ - 1) * 0.35f,
          1.5f, static_cast<float>(n_) * 0.16f);
      const float optical_strength =
          1.0f - std::exp(-layer.optical_depth / 24.0f);
      const float source_strength =
          std::sqrt(layer.coverage) *
          (0.45f + 1.05f * optical_strength) * source_normalization;
      const float inverse_horizontal_radius_squared =
          1.0f / (horizontal_radius * horizontal_radius);
      const int min_x =
          static_cast<int>(std::floor(cx - horizontal_radius));
      const int max_x =
          static_cast<int>(std::ceil(cx + horizontal_radius));
      const int min_y =
          static_cast<int>(std::floor(cy - horizontal_radius));
      const int max_y =
          static_cast<int>(std::ceil(cy + horizontal_radius));
      const int min_z = std::max(
          1, static_cast<int>(std::floor(cz - vertical_radius)));
      const int max_z = std::min(
          n_ - 2, static_cast<int>(std::ceil(cz + vertical_radius)));
      for (int z = min_z; z <= max_z; ++z) {
        for (int y = min_y; y <= max_y; ++y) {
          for (int x = min_x; x <= max_x; ++x) {
            const float dx = static_cast<float>(x) - cx;
            const float dy = static_cast<float>(y) - cy;
            const float dz = (static_cast<float>(z) - cz) *
                             horizontal_radius / vertical_radius;
            const float r2 = (dx * dx + dy * dy + dz * dz) *
                             inverse_horizontal_radius_squared;
            if (r2 > 1.0f) {
              continue;
            }
            const float weight =
                source_strength * std::exp(-3.0f * r2) * (1.0f - r2);
            const std::size_t i = index(x, y, z);
            if (solid_mask_[i] != 0U) {
              continue;
            }
            temperature_[i] +=
                dt * 0.85f * weight * thermal_factor *
                environmental_forcing_.thermal_source_multiplier;
            vapor_[i] += dt * 2.2f * weight * vapor_factor *
                         environmental_forcing_.vapor_source_multiplier;
            w_[i] +=
                dt * weight * updraft_factor *
                environmental_forcing_
                    .updraft_acceleration_cells_per_second_squared;
          }
        }
      }
    }
  }

  void applyForces(float dt) {
    if (!environmental_forcing_.enabled) {
      for (int z = 0; z < n_; ++z) {
        const float normalized_height =
            static_cast<float>(z) / static_cast<float>(n_ - 1);
        for (int y = 0; y < n_; ++y) {
          for (int x = 0; x < n_; ++x) {
            const std::size_t i = index(x, y, z);
            if (solid_mask_[i] != 0U) {
              continue;
            }
            const float buoyancy = 1.15f * temperature_[i] +
                                   0.22f * vapor_[i] - 0.10f * cloud_[i];
            w_[i] += dt * buoyancy;

            const float target_wind_x = 0.55f + 0.65f * normalized_height;
            const float target_wind_y =
                0.15f * std::sin(normalized_height * 6.28318f);
            const float wind_response = 1.0f - std::exp(-0.18f * dt);
            u_[i] += (target_wind_x - u_[i]) * wind_response;
            v_[i] += (target_wind_y - v_[i]) * wind_response;
          }
        }
      }
      return;
    }

    const float wind_response =
        1.0f -
        std::exp(-environmental_forcing_.wind_response_per_second * dt);
    for (int z = 0; z < n_; ++z) {
      const float normalized_height =
          static_cast<float>(z) / static_cast<float>(n_ - 1);
      const SimulationVector target_wind =
          sampleWindProfile(environmental_forcing_, normalized_height);
      const float layer_support = layerSupportAtHeight(
          environmental_forcing_, normalized_height,
          2.0f / static_cast<float>(n_ - 1));
      for (int y = 0; y < n_; ++y) {
        for (int x = 0; x < n_; ++x) {
          const std::size_t i = index(x, y, z);
          if (solid_mask_[i] != 0U) {
            continue;
          }
          const float buoyancy =
              1.15f * temperature_[i] + 0.22f * vapor_[i] - 0.10f * cloud_[i];
          w_[i] += dt * buoyancy *
                   environmental_forcing_
                       .buoyancy_acceleration_cells_per_second_squared;

          u_[i] += (target_wind.x - u_[i]) * wind_response;
          v_[i] += (target_wind.y - v_[i]) * wind_response;
          w_[i] +=
              (target_wind.z * layer_support - w_[i]) * wind_response;
        }
      }
    }
  }

  void applyInteractorForces(float dt) {
    beginInteractionWorkBudget(8U);
    for (std::size_t offset = 0; offset < interactors_.size(); ++offset) {
      const CloudInteractor &source_interactor =
          interactors_[(interaction_round_robin_ + offset) %
                       interactors_.size()];
      forEachSweptInteractor(
          source_interactor, true,
          [&](const CloudInteractor &interactor, float sweep_weight,
              bool /*is_current*/) {
            const float sweep_dt = dt * sweep_weight;
            const float maximum_extent = maximumExtentCells(interactor);
            const float influence =
                std::clamp(interactor.influence_radius * static_cast<float>(n_),
                           1.0f, static_cast<float>(n_) * 0.20f);
            const SimulationVector body_velocity =
                interactorLinearVelocityCells(interactor);
            const int center_x = static_cast<int>(
                std::lround(interactor.position.x * static_cast<float>(n_)));
            const int center_y = static_cast<int>(
                std::lround(interactor.position.y * static_cast<float>(n_)));
            const int center_z = static_cast<int>(std::lround(
                interactor.position.z * static_cast<float>(n_ - 1)));
            const int probe_radius = static_cast<int>(std::ceil(
                geometricBoundingRadiusCells(interactor) + influence + 1.0f));
            const std::array<SimulationVector, 6> probe_offsets = {
                SimulationVector{static_cast<float>(probe_radius), 0.0f, 0.0f},
                SimulationVector{-static_cast<float>(probe_radius), 0.0f, 0.0f},
                SimulationVector{0.0f, static_cast<float>(probe_radius), 0.0f},
                SimulationVector{0.0f, -static_cast<float>(probe_radius), 0.0f},
                SimulationVector{0.0f, 0.0f, static_cast<float>(probe_radius)},
                SimulationVector{0.0f, 0.0f, -static_cast<float>(probe_radius)},
            };
            SimulationVector ambient_fluid{};
            for (const SimulationVector &offset : probe_offsets) {
              const std::size_t probe_index =
                  index(center_x + static_cast<int>(offset.x),
                        center_y + static_cast<int>(offset.y),
                        center_z + static_cast<int>(offset.z));
              ambient_fluid.x += u_[probe_index];
              ambient_fluid.y += v_[probe_index];
              ambient_fluid.z += w_[probe_index];
            }
            ambient_fluid = multiply(ambient_fluid, 1.0f / 6.0f);
            const SimulationVector relative_motion =
                subtract(body_velocity, ambient_fluid);
            const float linear_relative_speed = vectorLength(relative_motion);
            const float rotational_tip_speed =
                vectorLength(interactor.angular_velocity) * maximum_extent;
            const float relative_speed =
                std::max(linear_relative_speed, 0.35f * rotational_tip_speed);
            const SimulationVector forward =
                linear_relative_speed > 0.05f
                    ? normalize(relative_motion)
                    : normalize(interactor.angular_velocity,
                                normalize(rotateVector(interactor.rotation,
                                                       {1.0f, 0.0f, 0.0f})));
            const SimulationVector extent_cells =
                interactorExtentCells(interactor);
            const std::array<SimulationVector, 3> local_axes = {
                normalize(
                    rotateVector(interactor.rotation, {1.0f, 0.0f, 0.0f})),
                normalize(
                    rotateVector(interactor.rotation, {0.0f, 1.0f, 0.0f})),
                normalize(
                    rotateVector(interactor.rotation, {0.0f, 0.0f, 1.0f})),
            };
            const std::array<float, 3> axis_extents = {
                extent_cells.x, extent_cells.y, extent_cells.z};
            std::size_t wing_axis_index = 0;
            float wing_axis_score = -1.0f;
            for (std::size_t axis = 0; axis < local_axes.size(); ++axis) {
              const float alignment =
                  std::clamp(dot(local_axes[axis], forward), -1.0f, 1.0f);
              const float score = axis_extents[axis] * axis_extents[axis] *
                                  (1.0f - alignment * alignment);
              if (score > wing_axis_score) {
                wing_axis_score = score;
                wing_axis_index = axis;
              }
            }
            const float minimum_extent =
                std::min({extent_cells.x, extent_cells.y, extent_cells.z});
            const bool generates_wingtip_pair =
                (interactor.shape == InteractorShape::Box ||
                 interactor.shape == InteractorShape::Ellipsoid) &&
                axis_extents[wing_axis_index] /
                        std::max(1.0f, minimum_extent) >=
                    2.0f;
            const SimulationVector wing_axis = local_axes[wing_axis_index];
            const float wing_half_span = axis_extents[wing_axis_index];
            const float vortex_core = std::max(1.5f, minimum_extent * 0.75f);

            if ((interactor.flags & kInteractorSolid) != 0U) {
              const SimulationVector solid_bounds =
                  add(worldAabbExtentCells(interactor),
                      {influence, influence, influence});
              forEachCellInBounds(
                  interactor, solid_bounds, [&](int x, int y, int z) {
                    const ShapeSample shape =
                        sampleInteractorShape(interactor, x, y, z);
                    if (shape.signed_distance > influence) {
                      return;
                    }

                    const float outside_fraction = std::clamp(
                        shape.signed_distance / influence, 0.0f, 1.0f);
                    const float smooth_outside =
                        outside_fraction * outside_fraction *
                        (3.0f - 2.0f * outside_fraction);
                    const float falloff = 1.0f - smooth_outside;
                    const bool inside = shape.signed_distance < 0.0f;
                    const std::size_t i = index(x, y, z);
                    const SimulationVector fluid{u_[i], v_[i], w_[i]};
                    const SimulationVector point_velocity =
                        interactorPointVelocity(interactor, shape);
                    const SimulationVector relative_at_point =
                        subtract(point_velocity, fluid);

                    const float normal_speed =
                        std::max(0.0f, dot(relative_at_point, shape.normal));
                    const float forward_alignment = dot(shape.normal, forward);
                    const SimulationVector lateral_component = subtract(
                        shape.normal, multiply(forward, forward_alignment));
                    const SimulationVector lateral_normal =
                        normalize(lateral_component, shape.normal);
                    const float axial = dot(shape.delta, forward);
                    const float front_weight = std::clamp(
                        0.5f + axial / (2.0f * std::max(1.0f, maximum_extent +
                                                                  influence)),
                        0.0f, 1.0f);
                    const float side_speed = relative_speed *
                                             interactor.displacement_strength *
                                             0.45f * front_weight;
                    SimulationVector target_velocity =
                        add(point_velocity,
                            add(multiply(shape.normal,
                                         normal_speed *
                                             interactor.displacement_strength),
                                multiply(lateral_normal, side_speed)));

                    if (interactor.turbulence_strength > 0.0f &&
                        relative_speed > 0.05f) {
                      const float phase =
                          static_cast<float>(interactor.id) * 0.173f +
                          static_cast<float>(x) * 0.731f +
                          static_cast<float>(y) * 1.117f +
                          static_cast<float>(z) * 0.419f +
                          static_cast<float>(time_) * 4.31f;
                      const SimulationVector noise{
                          std::sin(phase),
                          std::sin(phase * 1.37f + 2.1f),
                          std::sin(phase * 0.79f + 4.2f),
                      };
                      const SimulationVector perpendicular = subtract(
                          noise, multiply(forward, dot(noise, forward)));
                      target_velocity =
                          add(target_velocity,
                              multiply(normalize(perpendicular),
                                       interactor.turbulence_strength *
                                           relative_speed * 0.25f * falloff));
                    }

                    const float response = std::clamp(
                        sweep_dt *
                            (inside
                                 ? 12.0f +
                                       6.0f * interactor.displacement_strength
                                 : 2.0f * interactor.displacement_strength) *
                            falloff,
                        0.0f, 1.0f);
                    u_[i] += (target_velocity.x - u_[i]) * response;
                    v_[i] += (target_velocity.y - v_[i]) * response;
                    w_[i] += (target_velocity.z - w_[i]) * response;
                    if (response > 0.0001f) {
                      ++interaction_stats_.affected_velocity_cells;
                    }
                  });
            }

            if ((interactor.flags & kInteractorGenerateWake) == 0U ||
                interactor.wake_strength <= 0.0f || relative_speed <= 0.05f) {
              return;
            }

            const float wake_length =
                std::clamp(maximum_extent * 3.0f + relative_speed * 0.75f,
                           influence * 2.0f, static_cast<float>(n_) * 0.35f);
            const float wake_radius =
                generates_wingtip_pair
                    ? std::max(maximum_extent * 0.75f + influence,
                               wing_half_span + vortex_core * 2.5f)
                    : std::max(1.0f, maximum_extent * 0.75f + influence);
            CloudInteractor wake_region = interactor;
            wake_region.position.x = wrapUnit(interactor.position.x -
                                              forward.x * wake_length * 0.5f /
                                                  static_cast<float>(n_));
            wake_region.position.y = wrapUnit(interactor.position.y -
                                              forward.y * wake_length * 0.5f /
                                                  static_cast<float>(n_));
            wake_region.position.z = std::clamp(
                interactor.position.z -
                    forward.z * wake_length * 0.5f / static_cast<float>(n_ - 1),
                0.0f, 1.0f);
            const float maximum_wake_radius = wake_radius * 1.65f;
            const SimulationVector wake_bounds{
                std::abs(forward.x) * wake_length * 0.5f + maximum_wake_radius,
                std::abs(forward.y) * wake_length * 0.5f + maximum_wake_radius,
                std::abs(forward.z) * wake_length * 0.5f + maximum_wake_radius,
            };
            forEachCellInBounds(
                wake_region, wake_bounds, [&](int x, int y, int z) {
                  const ShapeSample shape =
                      sampleInteractorShape(interactor, x, y, z);
                  const float axial_distance = -dot(shape.delta, forward);
                  if (axial_distance <= 0.0f || axial_distance >= wake_length) {
                    return;
                  }
                  const SimulationVector radial_vector =
                      add(shape.delta, multiply(forward, axial_distance));
                  const float radial_distance = vectorLength(radial_vector);
                  const float length_fraction = axial_distance / wake_length;
                  const float radius_at_distance =
                      wake_radius * (1.0f + 0.65f * length_fraction);
                  if (radial_distance >= radius_at_distance) {
                    return;
                  }

                  const float radial_fraction =
                      radial_distance / radius_at_distance;
                  const float radial_falloff =
                      1.0f - radial_fraction * radial_fraction *
                                 (3.0f - 2.0f * radial_fraction);
                  const float falloff =
                      radial_falloff * (1.0f - 0.65f * length_fraction);
                  const std::size_t i = index(x, y, z);
                  const SimulationVector fluid{u_[i], v_[i], w_[i]};
                  const float drag_response = std::clamp(
                      sweep_dt * interactor.wake_strength * 0.8f * falloff,
                      0.0f, 0.75f);
                  SimulationVector wake_velocity =
                      add(fluid, multiply(subtract(body_velocity, fluid),
                                          drag_response));

                  if (interactor.turbulence_strength > 0.0f) {
                    const SimulationVector swirl_direction = normalize(
                        cross(forward, radial_vector), {0.0f, 0.0f, 1.0f});
                    const float phase =
                        static_cast<float>(interactor.id) * 0.317f +
                        axial_distance * 1.71f +
                        static_cast<float>(time_) *
                            (2.0f + relative_speed * 0.15f);
                    const float swirl = std::sin(phase) *
                                        interactor.turbulence_strength *
                                        relative_speed * falloff * sweep_dt;
                    wake_velocity =
                        add(wake_velocity, multiply(swirl_direction, swirl));
                  }
                  if (generates_wingtip_pair) {
                    for (const float side : {-1.0f, 1.0f}) {
                      const SimulationVector tip_offset =
                          multiply(wing_axis, side * wing_half_span);
                      const SimulationVector vortex_radial =
                          subtract(radial_vector, tip_offset);
                      const float vortex_radius = vectorLength(vortex_radial);
                      if (vortex_radius > vortex_core * 4.0f) {
                        continue;
                      }
                      const float radius_squared =
                          vortex_radius * vortex_radius;
                      const float core_squared = vortex_core * vortex_core;
                      const float core_profile =
                          (1.0f - std::exp(-radius_squared / core_squared)) /
                          std::max(0.35f, vortex_radius / vortex_core);
                      const float age_falloff =
                          std::exp(-1.8f * length_fraction);
                      const SimulationVector vortex_direction =
                          normalize(cross(forward, vortex_radial),
                                    normalize(cross(forward, wing_axis)));
                      const float vortex_impulse =
                          side * relative_speed * interactor.wake_strength *
                          0.16f * core_profile * age_falloff * sweep_dt;
                      wake_velocity =
                          add(wake_velocity,
                              multiply(vortex_direction, vortex_impulse));
                    }
                  }
                  u_[i] = wake_velocity.x;
                  v_[i] = wake_velocity.y;
                  w_[i] = wake_velocity.z;
                  ++interaction_stats_.affected_velocity_cells;
                  ++interaction_stats_.wake_velocity_cells;
                });
          });
    }
  }

  void displaceScalarsFromInteractors(float dt) {
    if (interactors_.empty()) {
      return;
    }
    clear(next_cloud_);
    clear(next_vapor_);
    clear(next_temperature_);
    std::vector<std::size_t> &touched = scalar_touched_indices_;
    touched.clear();
    touched.reserve(1024);

    const auto process_interactor = [&](const CloudInteractor &interactor,
                                        float sweep_weight) {
      touched.clear();
      const auto touch = [&](std::size_t i) {
        if (scalar_touched_mask_[i] == 0U) {
          scalar_touched_mask_[i] = 1U;
          touched.push_back(i);
        }
      };
      const float maximum_extent = maximumExtentCells(interactor);
      const SimulationVector scalar_bounds =
          add(worldAabbExtentCells(interactor), {3.5f, 3.5f, 3.5f});
      forEachCellInBounds(interactor, scalar_bounds, [&](int x, int y, int z) {
        const ShapeSample shape = sampleInteractorShape(interactor, x, y, z);
        if (shape.signed_distance >= 0.0f) {
          return;
        }
        const std::size_t source_index = index(x, y, z);
        const SimulationVector fluid{u_[source_index], v_[source_index],
                                     w_[source_index]};
        const SimulationVector point_velocity =
            interactorPointVelocity(interactor, shape);
        const float relative_speed =
            vectorLength(subtract(point_velocity, fluid));
        const float penetration =
            std::clamp(-shape.signed_distance / std::max(1.0f, maximum_extent),
                       0.0f, 1.0f);
        const float evacuation_rate = 35.0f +
                                      4.0f * interactor.displacement_strength +
                                      0.10f * relative_speed;
        const float fraction = std::clamp(
            penetration *
                (1.0f - std::exp(-evacuation_rate * dt * sweep_weight)),
            0.0f, 0.995f);
        if (fraction <= 0.0001f) {
          return;
        }

        const float outward_distance = scalarPushDistance(interactor, shape);
        struct ScalarTarget {
          std::size_t index = 0;
          float weight = 0.0f;
        };
        std::array<ScalarTarget, 24> targets{};
        std::size_t target_count = 0;
        float total_target_weight = 0.0f;
        bool target_work_available = true;
        constexpr std::array<float, 3> layer_offsets = {0.0f, 1.25f, 2.5f};
        constexpr std::array<float, 3> layer_weights = {0.50f, 0.30f, 0.20f};
        for (std::size_t layer = 0;
             layer < layer_offsets.size() && target_work_available; ++layer) {
          const float distance = outward_distance + layer_offsets[layer];
          const float target_x =
              static_cast<float>(x) + shape.normal.x * distance;
          const float target_y =
              static_cast<float>(y) + shape.normal.y * distance;
          const float target_z =
              static_cast<float>(z) + shape.normal.z * distance;
          const int base_x = static_cast<int>(std::floor(target_x));
          const int base_y = static_cast<int>(std::floor(target_y));
          const int base_z = static_cast<int>(std::floor(target_z));
          const float fraction_x = target_x - std::floor(target_x);
          const float fraction_y = target_y - std::floor(target_y);
          const float fraction_z = target_z - std::floor(target_z);
          for (int dz = 0; dz <= 1 && target_work_available; ++dz) {
            const int candidate_z = base_z + dz;
            if (candidate_z < 0 || candidate_z >= n_) {
              continue;
            }
            for (int dy = 0; dy <= 1 && target_work_available; ++dy) {
              for (int dx = 0; dx <= 1; ++dx) {
                const float weight_x = dx == 0 ? 1.0f - fraction_x : fraction_x;
                const float weight_y = dy == 0 ? 1.0f - fraction_y : fraction_y;
                const float weight_z = dz == 0 ? 1.0f - fraction_z : fraction_z;
                const float target_weight =
                    layer_weights[layer] * weight_x * weight_y * weight_z;
                if (target_weight <= 0.000001f) {
                  continue;
                }
                if (!consumeInteractionCellWork()) {
                  target_work_available = false;
                  break;
                }
                const int candidate_x = wrapHorizontal(base_x + dx);
                const int candidate_y = wrapHorizontal(base_y + dy);
                const std::size_t target_index =
                    index(candidate_x, candidate_y, candidate_z);
                if (target_index == source_index ||
                    solid_mask_[target_index] != 0U ||
                    sampleInteractorShape(interactor, candidate_x, candidate_y,
                                          candidate_z)
                            .signed_distance < 0.0f) {
                  continue;
                }
                targets[target_count++] = {target_index, target_weight};
                total_target_weight += target_weight;
              }
            }
          }
        }
        if (!target_work_available) {
          return;
        }
        if (total_target_weight <= 0.000001f) {
          ++interaction_stats_.invalid_scalar_targets;
          return;
        }

        const float moved_cloud = cloud_[source_index] * fraction;
        const float moved_vapor = vapor_[source_index] * fraction;
        const float moved_temperature = temperature_[source_index] * fraction;
        next_cloud_[source_index] -= moved_cloud;
        next_vapor_[source_index] -= moved_vapor;
        next_temperature_[source_index] -= moved_temperature;
        touch(source_index);
        for (std::size_t target = 0; target < target_count; ++target) {
          const float normalized_weight =
              targets[target].weight / total_target_weight;
          const std::size_t target_index = targets[target].index;
          next_cloud_[target_index] += moved_cloud * normalized_weight;
          next_vapor_[target_index] += moved_vapor * normalized_weight;
          next_temperature_[target_index] +=
              moved_temperature * normalized_weight;
          touch(target_index);
        }
        interaction_stats_.displaced_cloud_mass += moved_cloud;
        ++interaction_stats_.displaced_scalar_cells;
      });

      for (const std::size_t i : touched) {
        cloud_[i] = std::max(0.0f, cloud_[i] + next_cloud_[i]);
        vapor_[i] = std::max(0.0f, vapor_[i] + next_vapor_[i]);
        temperature_[i] =
            std::clamp(temperature_[i] + next_temperature_[i], -1.0f, 3.0f);
        next_cloud_[i] = 0.0f;
        next_vapor_[i] = 0.0f;
        next_temperature_[i] = 0.0f;
        scalar_touched_mask_[i] = 0U;
      }
    };

    std::vector<float> current_weights(interactors_.size(), 1.0f);
    beginInteractionWorkBudget(4U);
    for (std::size_t offset = 0; offset < interactors_.size(); ++offset) {
      const std::size_t interactor_index =
          (interaction_round_robin_ + offset) % interactors_.size();
      const CloudInteractor &source_interactor = interactors_[interactor_index];
      if ((source_interactor.flags & kInteractorDisplaceScalars) == 0U) {
        continue;
      }
      forEachSweptInteractor(source_interactor, false,
                             [&](const CloudInteractor &interactor,
                                 float sweep_weight, bool is_current) {
                               if (is_current) {
                                 current_weights[interactor_index] =
                                     sweep_weight;
                               } else {
                                 process_interactor(interactor, sweep_weight);
                               }
                             });
    }

    beginInteractionWorkBudget(4U);
    for (std::size_t offset = 0; offset < interactors_.size(); ++offset) {
      const std::size_t interactor_index =
          (interaction_round_robin_ + offset) % interactors_.size();
      const CloudInteractor &interactor = interactors_[interactor_index];
      if ((interactor.flags & kInteractorDisplaceScalars) != 0U) {
        process_interactor(interactor, current_weights[interactor_index]);
      }
    }
  }

  void applyThermodynamics(float dt) {
    if (!environmental_forcing_.enabled) {
      const float condense_response = 1.0f - std::exp(-5.0f * dt);
      const float evaporate_response = 1.0f - std::exp(-1.4f * dt);
      const float cloud_decay = std::exp(-0.004f * dt);
      const float vapor_decay = std::exp(-0.001f * dt);
      const float temperature_decay = std::exp(-0.055f * dt);

      for (int z = 0; z < n_; ++z) {
        const float normalized_height =
            static_cast<float>(z) / static_cast<float>(n_ - 1);
        for (int y = 0; y < n_; ++y) {
          for (int x = 0; x < n_; ++x) {
            const std::size_t i = index(x, y, z);
            if (solid_mask_[i] != 0U) {
              continue;
            }
            temperature_[i] -= dt * 0.035f * normalized_height;
            const float saturation =
                std::clamp(0.20f + 0.25f * temperature_[i] -
                               0.055f * normalized_height,
                           0.06f, 0.75f);

            if (vapor_[i] > saturation) {
              const float amount =
                  (vapor_[i] - saturation) * condense_response;
              vapor_[i] -= amount;
              cloud_[i] += amount;
              temperature_[i] += amount * 0.10f;
            } else if (cloud_[i] > 0.0f && vapor_[i] < saturation) {
              const float amount =
                  std::min(cloud_[i], (saturation - vapor_[i]) *
                                                evaporate_response);
              vapor_[i] += amount;
              cloud_[i] -= amount;
              temperature_[i] -= amount * 0.06f;
            }

            cloud_[i] = std::max(0.0f, cloud_[i] * cloud_decay);
            vapor_[i] = std::max(0.0f, vapor_[i] * vapor_decay);
            temperature_[i] = std::clamp(
                temperature_[i] * temperature_decay, -1.0f, 3.0f);
          }
        }
      }
      return;
    }

    const float vapor_decay =
        std::exp(-environmental_forcing_.vapor_decay_per_second * dt);
    const float temperature_decay =
        std::exp(-environmental_forcing_.temperature_decay_per_second * dt);
    const float temperature_target_response =
        1.0f - std::exp(
                   -environmental_forcing_
                        .temperature_target_response_per_second *
                   dt);
    const float vapor_target_response =
        1.0f - std::exp(
                   -environmental_forcing_.vapor_target_response_per_second *
                   dt);
    const float precipitation_response =
        1.0f -
        std::exp(-environmental_forcing_.precipitation_per_second * dt);

    for (int z = 0; z < n_; ++z) {
      const float normalized_height =
          static_cast<float>(z) / static_cast<float>(n_ - 1);
      const float layer_support = layerSupportAtHeight(
          environmental_forcing_, normalized_height,
          2.0f / static_cast<float>(n_ - 1));
      const float condense_response =
          1.0f - std::exp(
                     -environmental_forcing_.condensation_per_second *
                     (0.10f + 0.90f * layer_support) * dt);
      const float evaporate_response =
          1.0f - std::exp(
                     -environmental_forcing_.evaporation_per_second *
                     (1.25f - 0.35f * layer_support) * dt);
      const float cloud_decay = std::exp(
          -(environmental_forcing_.cloud_decay_per_second +
            0.12f * (1.0f - layer_support)) *
          dt);
      for (int y = 0; y < n_; ++y) {
        for (int x = 0; x < n_; ++x) {
          const std::size_t i = index(x, y, z);
          if (solid_mask_[i] != 0U) {
            continue;
          }
          const float temperature_target =
              environmental_forcing_.surface_temperature_target +
              (environmental_forcing_.top_temperature_target -
               environmental_forcing_.surface_temperature_target) *
                  normalized_height;
          const float background_vapor_target =
              environmental_forcing_.surface_vapor_target +
              (environmental_forcing_.top_vapor_target -
               environmental_forcing_.surface_vapor_target) *
                  normalized_height;
          const float vapor_target =
              background_vapor_target *
                  (0.20f + 0.80f * layer_support) +
              0.08f * layer_support;
          temperature_[i] -= dt *
                             environmental_forcing_.lapse_cooling_per_second *
                             normalized_height;
          temperature_[i] +=
              (temperature_target - temperature_[i]) *
              temperature_target_response;
          vapor_[i] += (vapor_target - vapor_[i]) * vapor_target_response;
          const float saturation = std::clamp(0.20f + 0.25f * temperature_[i] -
                                                  0.055f * normalized_height,
                                              0.06f, 0.75f);

          if (vapor_[i] > saturation) {
            const float amount = (vapor_[i] - saturation) * condense_response;
            vapor_[i] -= amount;
            cloud_[i] += amount;
            temperature_[i] += amount * 0.10f;
          } else if (cloud_[i] > 0.0f && vapor_[i] < saturation) {
            const float amount = std::min(cloud_[i], (saturation - vapor_[i]) *
                                                         evaporate_response);
            vapor_[i] += amount;
            cloud_[i] -= amount;
            temperature_[i] -= amount * 0.06f;
          }

          const float precipitable_cloud =
              std::max(0.0f, cloud_[i] -
                                 environmental_forcing_.precipitation_threshold);
          cloud_[i] -= precipitable_cloud * precipitation_response;

          cloud_[i] = std::max(0.0f, cloud_[i] * cloud_decay);
          vapor_[i] = std::max(0.0f, vapor_[i] * vapor_decay);
          temperature_[i] =
              std::clamp(temperature_[i] * temperature_decay, -1.0f, 3.0f);
        }
      }
    }
  }

  void projectVelocity() {
    clear(pressure_);
    for (int z = 0; z < n_; ++z) {
      for (int y = 0; y < n_; ++y) {
        for (int x = 0; x < n_; ++x) {
          const std::size_t i = index(x, y, z);
          if (solid_mask_[i] != 0U) {
            divergence_[i] = 0.0f;
            continue;
          }
          divergence_[i] =
              -0.5f * (u_[index(x + 1, y, z)] - u_[index(x - 1, y, z)] +
                       v_[index(x, y + 1, z)] - v_[index(x, y - 1, z)] +
                       w_[index(x, y, z + 1)] - w_[index(x, y, z - 1)]);
        }
      }
    }

    for (int iteration = 0; iteration < pressure_iterations_; ++iteration) {
      for (int z = 0; z < n_; ++z) {
        for (int y = 0; y < n_; ++y) {
          for (int x = 0; x < n_; ++x) {
            const std::size_t i = index(x, y, z);
            if (solid_mask_[i] != 0U) {
              next_pressure_[i] = 0.0f;
              continue;
            }
            float neighbor_pressure = 0.0f;
            int fluid_neighbors = 0;
            const std::size_t neighbors[] = {
                index(x + 1, y, z), index(x - 1, y, z), index(x, y + 1, z),
                index(x, y - 1, z), index(x, y, z + 1), index(x, y, z - 1),
            };
            for (const std::size_t neighbor : neighbors) {
              if (solid_mask_[neighbor] == 0U) {
                neighbor_pressure += pressure_[neighbor];
                ++fluid_neighbors;
              }
            }
            next_pressure_[i] = fluid_neighbors > 0
                                    ? (divergence_[i] + neighbor_pressure) /
                                          static_cast<float>(fluid_neighbors)
                                    : 0.0f;
          }
        }
      }
      pressure_.swap(next_pressure_);
    }

    for (int z = 0; z < n_; ++z) {
      for (int y = 0; y < n_; ++y) {
        for (int x = 0; x < n_; ++x) {
          const std::size_t i = index(x, y, z);
          if (solid_mask_[i] != 0U) {
            continue;
          }
          const auto pressureAt = [&](int neighbor_x, int neighbor_y,
                                      int neighbor_z) {
            const std::size_t neighbor =
                index(neighbor_x, neighbor_y, neighbor_z);
            return solid_mask_[neighbor] != 0U ? pressure_[i]
                                               : pressure_[neighbor];
          };
          u_[i] -= 0.5f * (pressureAt(x + 1, y, z) - pressureAt(x - 1, y, z));
          v_[i] -= 0.5f * (pressureAt(x, y + 1, z) - pressureAt(x, y - 1, z));
          w_[i] -= 0.5f * (pressureAt(x, y, z + 1) - pressureAt(x, y, z - 1));
        }
      }
    }
  }

  void applyBoundaries() {
    for (int y = 0; y < n_; ++y) {
      for (int x = 0; x < n_; ++x) {
        const std::size_t bottom = index(x, y, 0);
        const std::size_t top = index(x, y, n_ - 1);
        if (solid_mask_[bottom] == 0U) {
          w_[bottom] = 0.0f;
        }
        if (solid_mask_[top] == 0U) {
          w_[top] = 0.0f;
        }
      }
    }
  }

  static bool finiteField(const std::vector<float> &field) {
    return std::all_of(field.begin(), field.end(),
                       [](float value) { return std::isfinite(value); });
  }

  int n_;
  int pressure_iterations_;
  std::size_t count_;
  double time_ = 0.0;

  std::vector<float> u_, v_, w_;
  std::vector<float> temperature_, vapor_, cloud_;
  std::vector<float> next_u_, next_v_, next_w_;
  std::vector<float> next_temperature_, next_vapor_, next_cloud_;
  std::vector<float> pressure_, next_pressure_, divergence_;
  std::vector<std::uint8_t> solid_mask_;
  std::vector<std::uint8_t> solid_owner_rank_;
  std::vector<std::uint8_t> scalar_touched_mask_;
  std::vector<std::size_t> scalar_touched_indices_;
  std::vector<CloudInteractor> interactors_;
  std::array<float, 4> layer_source_x_cells_{};
  std::array<float, 4> layer_source_y_cells_{};
  CloudForcing environmental_forcing_{};
  InteractionStats interaction_stats_;
  std::size_t interaction_cell_budget_ = 0;
  std::size_t interaction_round_robin_ = 0;
};

} // namespace cloud
