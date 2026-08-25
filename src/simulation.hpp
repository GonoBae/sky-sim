#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>

namespace cloud {

struct SimulationStats {
    float minimum = 0.0f;
    float maximum = 0.0f;
    float mean = 0.0f;
};

class CloudSimulation {
public:
    explicit CloudSimulation(int size, int pressure_iterations = 12)
        : n_(size), pressure_iterations_(pressure_iterations), count_(voxelCount(size)) {
        if (size < 16 || size > 192) {
            throw std::invalid_argument("Grid size must be between 16 and 192");
        }
        if (pressure_iterations < 1 || pressure_iterations > 80) {
            throw std::invalid_argument("Pressure iterations must be between 1 and 80");
        }
        allocateFields();
        reset();
    }

    int size() const { return n_; }
    float time() const { return time_; }

    void reset() {
        clear(u_); clear(v_); clear(w_);
        clear(temperature_); clear(vapor_); clear(cloud_);
        clear(next_u_); clear(next_v_); clear(next_w_);
        clear(next_temperature_); clear(next_vapor_); clear(next_cloud_);
        clear(pressure_); clear(next_pressure_); clear(divergence_);
        time_ = 0.0f;
    }

    void step(float dt) {
        if (!(dt > 0.0f) || dt > 0.25f) {
            throw std::invalid_argument("Simulation dt must be in (0, 0.25]");
        }

        injectThermal(dt);
        applyForces(dt);

        advect(u_, next_u_, dt);
        advect(v_, next_v_, dt);
        advect(w_, next_w_, dt);
        advect(temperature_, next_temperature_, dt);
        advect(vapor_, next_vapor_, dt);
        advect(cloud_, next_cloud_, dt);

        u_.swap(next_u_);
        v_.swap(next_v_);
        w_.swap(next_w_);
        temperature_.swap(next_temperature_);
        vapor_.swap(next_vapor_);
        cloud_.swap(next_cloud_);

        applyThermodynamics(dt);
        projectVelocity();
        applyBoundaries();
        time_ += dt;
    }

    std::vector<std::uint8_t> densityBytes() const {
        std::vector<std::uint8_t> result(count_);
        for (std::size_t i = 0; i < count_; ++i) {
            const float optical_density = 1.0f - std::exp(-2.4f * std::max(0.0f, cloud_[i]));
            result[i] = static_cast<std::uint8_t>(
                std::lround(255.0f * std::clamp(optical_density, 0.0f, 1.0f)));
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

    bool allFinite() const {
        return finiteField(u_) && finiteField(v_) && finiteField(w_) &&
               finiteField(temperature_) && finiteField(vapor_) && finiteField(cloud_);
    }

private:
    static std::size_t voxelCount(int n) {
        return static_cast<std::size_t>(n) * static_cast<std::size_t>(n) *
               static_cast<std::size_t>(n);
    }

    void allocateFields() {
        auto allocate = [this](std::vector<float>& field) { field.resize(count_, 0.0f); };
        allocate(u_); allocate(v_); allocate(w_);
        allocate(temperature_); allocate(vapor_); allocate(cloud_);
        allocate(next_u_); allocate(next_v_); allocate(next_w_);
        allocate(next_temperature_); allocate(next_vapor_); allocate(next_cloud_);
        allocate(pressure_); allocate(next_pressure_); allocate(divergence_);
    }

    static void clear(std::vector<float>& field) {
        std::fill(field.begin(), field.end(), 0.0f);
    }

    int wrapHorizontal(int value) const {
        value %= n_;
        return value < 0 ? value + n_ : value;
    }

    int clampVertical(int value) const {
        return std::clamp(value, 0, n_ - 1);
    }

    std::size_t index(int x, int y, int z) const {
        x = wrapHorizontal(x);
        y = wrapHorizontal(y);
        z = clampVertical(z);
        return (static_cast<std::size_t>(z) * static_cast<std::size_t>(n_) +
                static_cast<std::size_t>(y)) * static_cast<std::size_t>(n_) +
               static_cast<std::size_t>(x);
    }

    float sample(const std::vector<float>& field, float x, float y, float z) const {
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

    void advect(const std::vector<float>& source, std::vector<float>& destination, float dt) {
        for (int z = 0; z < n_; ++z) {
            for (int y = 0; y < n_; ++y) {
                for (int x = 0; x < n_; ++x) {
                    const std::size_t i = index(x, y, z);
                    const float back_x = static_cast<float>(x) - dt * u_[i];
                    const float back_y = static_cast<float>(y) - dt * v_[i];
                    const float back_z = std::clamp(
                        static_cast<float>(z) - dt * w_[i], 0.0f, static_cast<float>(n_ - 1));
                    destination[i] = sample(source, back_x, back_y, back_z);
                }
            }
        }
    }

    void injectThermal(float dt) {
        const float cx = static_cast<float>(n_) * (0.50f + 0.08f * std::sin(time_ * 0.19f));
        const float cy = static_cast<float>(n_) * (0.50f + 0.06f * std::cos(time_ * 0.17f));
        const float cz = static_cast<float>(n_) * 0.10f;
        const float radius = std::max(2.0f, static_cast<float>(n_) * 0.10f);
        const float inverse_radius_squared = 1.0f / (radius * radius);

        const int min_x = static_cast<int>(std::floor(cx - radius));
        const int max_x = static_cast<int>(std::ceil(cx + radius));
        const int min_y = static_cast<int>(std::floor(cy - radius));
        const int max_y = static_cast<int>(std::ceil(cy + radius));
        const int min_z = std::max(1, static_cast<int>(std::floor(cz - radius * 0.6f)));
        const int max_z = std::min(n_ - 2, static_cast<int>(std::ceil(cz + radius * 0.6f)));

        for (int z = min_z; z <= max_z; ++z) {
            for (int y = min_y; y <= max_y; ++y) {
                for (int x = min_x; x <= max_x; ++x) {
                    const float dx = static_cast<float>(x) - cx;
                    const float dy = static_cast<float>(y) - cy;
                    const float dz = (static_cast<float>(z) - cz) * 1.5f;
                    const float r2 = (dx * dx + dy * dy + dz * dz) * inverse_radius_squared;
                    if (r2 > 1.0f) {
                        continue;
                    }
                    const float weight = std::exp(-3.0f * r2) * (1.0f - r2);
                    const std::size_t i = index(x, y, z);
                    temperature_[i] += dt * 0.85f * weight;
                    vapor_[i] += dt * 2.2f * weight;
                    w_[i] += dt * 1.6f * weight;
                }
            }
        }
    }

    void applyForces(float dt) {
        for (int z = 0; z < n_; ++z) {
            const float normalized_height = static_cast<float>(z) / static_cast<float>(n_ - 1);
            for (int y = 0; y < n_; ++y) {
                for (int x = 0; x < n_; ++x) {
                    const std::size_t i = index(x, y, z);
                    const float buoyancy = 1.15f * temperature_[i] + 0.22f * vapor_[i] -
                                           0.10f * cloud_[i];
                    w_[i] += dt * buoyancy;

                    const float target_wind_x = 0.55f + 0.65f * normalized_height;
                    const float target_wind_y = 0.15f * std::sin(normalized_height * 6.28318f);
                    const float wind_response = 1.0f - std::exp(-0.18f * dt);
                    u_[i] += (target_wind_x - u_[i]) * wind_response;
                    v_[i] += (target_wind_y - v_[i]) * wind_response;
                }
            }
        }
    }

    void applyThermodynamics(float dt) {
        const float condense_response = 1.0f - std::exp(-5.0f * dt);
        const float evaporate_response = 1.0f - std::exp(-1.4f * dt);
        const float cloud_decay = std::exp(-0.004f * dt);
        const float vapor_decay = std::exp(-0.001f * dt);
        const float temperature_decay = std::exp(-0.055f * dt);

        for (int z = 0; z < n_; ++z) {
            const float normalized_height = static_cast<float>(z) / static_cast<float>(n_ - 1);
            for (int y = 0; y < n_; ++y) {
                for (int x = 0; x < n_; ++x) {
                    const std::size_t i = index(x, y, z);
                    temperature_[i] -= dt * 0.035f * normalized_height;
                    const float saturation = std::clamp(
                        0.20f + 0.25f * temperature_[i] - 0.055f * normalized_height,
                        0.06f,
                        0.75f);

                    if (vapor_[i] > saturation) {
                        const float amount = (vapor_[i] - saturation) * condense_response;
                        vapor_[i] -= amount;
                        cloud_[i] += amount;
                        temperature_[i] += amount * 0.10f;
                    } else if (cloud_[i] > 0.0f && vapor_[i] < saturation) {
                        const float amount = std::min(
                            cloud_[i], (saturation - vapor_[i]) * evaporate_response);
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
    }

    void projectVelocity() {
        clear(pressure_);
        for (int z = 0; z < n_; ++z) {
            for (int y = 0; y < n_; ++y) {
                for (int x = 0; x < n_; ++x) {
                    const std::size_t i = index(x, y, z);
                    divergence_[i] = -0.5f * (
                        u_[index(x + 1, y, z)] - u_[index(x - 1, y, z)] +
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
                        next_pressure_[i] = (
                            divergence_[i] +
                            pressure_[index(x + 1, y, z)] + pressure_[index(x - 1, y, z)] +
                            pressure_[index(x, y + 1, z)] + pressure_[index(x, y - 1, z)] +
                            pressure_[index(x, y, z + 1)] + pressure_[index(x, y, z - 1)]) /
                            6.0f;
                    }
                }
            }
            pressure_.swap(next_pressure_);
        }

        for (int z = 0; z < n_; ++z) {
            for (int y = 0; y < n_; ++y) {
                for (int x = 0; x < n_; ++x) {
                    const std::size_t i = index(x, y, z);
                    u_[i] -= 0.5f * (pressure_[index(x + 1, y, z)] - pressure_[index(x - 1, y, z)]);
                    v_[i] -= 0.5f * (pressure_[index(x, y + 1, z)] - pressure_[index(x, y - 1, z)]);
                    w_[i] -= 0.5f * (pressure_[index(x, y, z + 1)] - pressure_[index(x, y, z - 1)]);
                }
            }
        }
    }

    void applyBoundaries() {
        for (int y = 0; y < n_; ++y) {
            for (int x = 0; x < n_; ++x) {
                w_[index(x, y, 0)] = 0.0f;
                w_[index(x, y, n_ - 1)] = 0.0f;
            }
        }
    }

    static bool finiteField(const std::vector<float>& field) {
        return std::all_of(field.begin(), field.end(), [](float value) {
            return std::isfinite(value);
        });
    }

    int n_;
    int pressure_iterations_;
    std::size_t count_;
    float time_ = 0.0f;

    std::vector<float> u_, v_, w_;
    std::vector<float> temperature_, vapor_, cloud_;
    std::vector<float> next_u_, next_v_, next_w_;
    std::vector<float> next_temperature_, next_vapor_, next_cloud_;
    std::vector<float> pressure_, next_pressure_, divergence_;
};

} // namespace cloud
