#pragma once

#include "environment_types.hpp"

#include <algorithm>
#include <cmath>

namespace cloud {
namespace astronomy {

constexpr double kPi = 3.1415926535897932384626433832795;
constexpr double kDegreesToRadians = kPi / 180.0;
constexpr double kRadiansToDegrees = 180.0 / kPi;

inline double normalizeDegrees(double value) {
  value = std::fmod(value, 360.0);
  return value < 0.0 ? value + 360.0 : value;
}

inline double normalizeRadians(double value) {
  value = std::fmod(value, 2.0 * kPi);
  return value < 0.0 ? value + 2.0 * kPi : value;
}

inline double julianDay(double unix_seconds) {
  return unix_seconds / 86400.0 + 2440587.5;
}

inline double utcMinutesOfDay(double unix_seconds) {
  double seconds = std::fmod(unix_seconds, 86400.0);
  if (seconds < 0.0) {
    seconds += 86400.0;
  }
  return seconds / 60.0;
}

inline EnvironmentVector horizontalDirection(double declination_radians,
                                              double hour_angle_radians,
                                              double latitude_radians) {
  const double cos_declination = std::cos(declination_radians);
  EnvironmentVector direction;
  direction.east = static_cast<float>(
      -cos_declination * std::sin(hour_angle_radians));
  direction.north = static_cast<float>(
      std::sin(declination_radians) * std::cos(latitude_radians) -
      cos_declination * std::sin(latitude_radians) *
          std::cos(hour_angle_radians));
  direction.up = static_cast<float>(
      std::sin(declination_radians) * std::sin(latitude_radians) +
      cos_declination * std::cos(latitude_radians) *
          std::cos(hour_angle_radians));
  return direction;
}

inline void directionAngles(const EnvironmentVector &direction,
                            float &elevation_degrees,
                            float &azimuth_degrees) {
  const double up = std::clamp(static_cast<double>(direction.up), -1.0, 1.0);
  elevation_degrees =
      static_cast<float>(std::asin(up) * kRadiansToDegrees);
  azimuth_degrees = static_cast<float>(normalizeDegrees(
      std::atan2(static_cast<double>(direction.east),
                 static_cast<double>(direction.north)) *
      kRadiansToDegrees));
}

struct SolarCoordinates {
  EnvironmentVector direction{};
  float elevation_degrees = -90.0f;
  float azimuth_degrees = 0.0f;
  float distance_au = 1.0f;
  float angular_radius_degrees = 0.2666f;
};

inline SolarCoordinates solarCoordinates(double unix_seconds,
                                         const GeoLocation &location) {
  const double jd = julianDay(unix_seconds);
  const double centuries = (jd - 2451545.0) / 36525.0;
  const double mean_longitude = normalizeDegrees(
      280.46646 + centuries * (36000.76983 + centuries * 0.0003032));
  const double mean_anomaly_degrees = normalizeDegrees(
      357.52911 + centuries * (35999.05029 - 0.0001537 * centuries));
  const double mean_anomaly = mean_anomaly_degrees * kDegreesToRadians;
  const double eccentricity =
      0.016708634 - centuries * (0.000042037 + 0.0000001267 * centuries);
  const double equation_of_center =
      std::sin(mean_anomaly) *
          (1.914602 - centuries * (0.004817 + 0.000014 * centuries)) +
      std::sin(2.0 * mean_anomaly) * (0.019993 - 0.000101 * centuries) +
      std::sin(3.0 * mean_anomaly) * 0.000289;
  const double true_longitude = mean_longitude + equation_of_center;
  const double true_anomaly =
      (mean_anomaly_degrees + equation_of_center) * kDegreesToRadians;
  const double omega = (125.04 - 1934.136 * centuries) * kDegreesToRadians;
  const double apparent_longitude =
      (true_longitude - 0.00569 - 0.00478 * std::sin(omega)) *
      kDegreesToRadians;
  const double mean_obliquity =
      23.0 +
      (26.0 +
       (21.448 -
        centuries * (46.815 + centuries * (0.00059 - 0.001813 * centuries))) /
           60.0) /
          60.0;
  const double obliquity =
      (mean_obliquity + 0.00256 * std::cos(omega)) * kDegreesToRadians;
  const double declination =
      std::asin(std::sin(obliquity) * std::sin(apparent_longitude));

  const double y = std::tan(obliquity * 0.5) *
                   std::tan(obliquity * 0.5);
  const double equation_of_time =
      4.0 * kRadiansToDegrees *
      (y * std::sin(2.0 * mean_longitude * kDegreesToRadians) -
       2.0 * eccentricity * std::sin(mean_anomaly) +
       4.0 * eccentricity * y * std::sin(mean_anomaly) *
           std::cos(2.0 * mean_longitude * kDegreesToRadians) -
       0.5 * y * y * std::sin(4.0 * mean_longitude * kDegreesToRadians) -
       1.25 * eccentricity * eccentricity * std::sin(2.0 * mean_anomaly));
  double true_solar_minutes =
      std::fmod(utcMinutesOfDay(unix_seconds) + equation_of_time +
                    4.0 * location.longitude_degrees,
                1440.0);
  if (true_solar_minutes < 0.0) {
    true_solar_minutes += 1440.0;
  }
  const double hour_angle =
      (true_solar_minutes / 4.0 - 180.0) * kDegreesToRadians;
  const double latitude = location.latitude_degrees * kDegreesToRadians;

  SolarCoordinates result;
  result.direction = horizontalDirection(declination, hour_angle, latitude);
  directionAngles(result.direction, result.elevation_degrees,
                  result.azimuth_degrees);
  const double distance_au =
      (1.000001018 * (1.0 - eccentricity * eccentricity)) /
      (1.0 + eccentricity * std::cos(true_anomaly));
  result.distance_au = static_cast<float>(distance_au);
  result.angular_radius_degrees =
      static_cast<float>(0.2666 / std::max(0.95, distance_au));
  return result;
}

struct LunarCoordinates {
  EnvironmentVector direction{};
  float elevation_degrees = -90.0f;
  float azimuth_degrees = 0.0f;
  float distance_km = 385000.56f;
  float angular_radius_degrees = 0.2725f;
  float illuminated_fraction = 0.0f;
};

inline LunarCoordinates lunarCoordinates(double unix_seconds,
                                         const GeoLocation &location,
                                         const SolarCoordinates &sun) {
  // Truncated Meeus lunar series. Its error is small enough for a rendered
  // lunar disk while keeping the high-precision ephemeris provider replaceable.
  const double jd = julianDay(unix_seconds);
  const double centuries = (jd - 2451545.0) / 36525.0;
  const double mean_longitude = normalizeDegrees(
      218.3164477 + 481267.88123421 * centuries) * kDegreesToRadians;
  const double elongation = normalizeDegrees(
      297.8501921 + 445267.1114034 * centuries) * kDegreesToRadians;
  const double solar_anomaly = normalizeDegrees(
      357.5291092 + 35999.0502909 * centuries) * kDegreesToRadians;
  const double lunar_anomaly = normalizeDegrees(
      134.9633964 + 477198.8675055 * centuries) * kDegreesToRadians;
  const double argument_latitude = normalizeDegrees(
      93.2720950 + 483202.0175233 * centuries) * kDegreesToRadians;

  const double longitude =
      mean_longitude +
      (6.289 * std::sin(lunar_anomaly) +
       1.274 * std::sin(2.0 * elongation - lunar_anomaly) +
       0.658 * std::sin(2.0 * elongation) +
       0.214 * std::sin(2.0 * lunar_anomaly) -
       0.186 * std::sin(solar_anomaly) -
       0.114 * std::sin(2.0 * argument_latitude)) *
          kDegreesToRadians;
  const double latitude =
      (5.128 * std::sin(argument_latitude) +
       0.280 * std::sin(lunar_anomaly + argument_latitude) +
       0.277 * std::sin(lunar_anomaly - argument_latitude) +
       0.173 * std::sin(2.0 * elongation - argument_latitude) +
       0.055 * std::sin(2.0 * elongation + argument_latitude - lunar_anomaly) +
       0.046 * std::sin(2.0 * elongation - argument_latitude - lunar_anomaly)) *
      kDegreesToRadians;
  const double distance_km =
      385000.56 - 20905.0 * std::cos(lunar_anomaly) -
      3699.0 * std::cos(2.0 * elongation - lunar_anomaly) -
      2956.0 * std::cos(2.0 * elongation) -
      570.0 * std::cos(2.0 * lunar_anomaly);

  const double obliquity =
      (23.439291 - 0.0130042 * centuries) * kDegreesToRadians;
  const double ecliptic_x = std::cos(latitude) * std::cos(longitude);
  const double ecliptic_y = std::cos(latitude) * std::sin(longitude);
  const double ecliptic_z = std::sin(latitude);
  const double equatorial_x = ecliptic_x;
  const double equatorial_y =
      ecliptic_y * std::cos(obliquity) - ecliptic_z * std::sin(obliquity);
  const double equatorial_z =
      ecliptic_y * std::sin(obliquity) + ecliptic_z * std::cos(obliquity);
  const double right_ascension = std::atan2(equatorial_y, equatorial_x);
  const double declination = std::asin(std::clamp(equatorial_z, -1.0, 1.0));

  const double days_since_epoch = jd - 2451545.0;
  const double sidereal_degrees = normalizeDegrees(
      280.46061837 + 360.98564736629 * days_since_epoch +
      0.000387933 * centuries * centuries -
      centuries * centuries * centuries / 38710000.0 +
      location.longitude_degrees);
  const double hour_angle =
      sidereal_degrees * kDegreesToRadians - right_ascension;

  LunarCoordinates result;
  result.direction = horizontalDirection(
      declination, hour_angle,
      location.latitude_degrees * kDegreesToRadians);
  directionAngles(result.direction, result.elevation_degrees,
                  result.azimuth_degrees);
  result.distance_km = static_cast<float>(distance_km);
  result.angular_radius_degrees = static_cast<float>(
      0.2725 * 385000.56 / std::max(350000.0, distance_km));
  const double dot = std::clamp(
      static_cast<double>(sun.direction.east) * result.direction.east +
          static_cast<double>(sun.direction.north) * result.direction.north +
          static_cast<double>(sun.direction.up) * result.direction.up,
      -1.0, 1.0);
  result.illuminated_fraction = static_cast<float>((1.0 - dot) * 0.5);
  return result;
}

inline double localSiderealAngleRadians(double unix_seconds,
                                        double longitude_degrees) {
  const double jd = julianDay(unix_seconds);
  const double centuries = (jd - 2451545.0) / 36525.0;
  return normalizeRadians(
      (280.46061837 + 360.98564736629 * (jd - 2451545.0) +
       0.000387933 * centuries * centuries -
       centuries * centuries * centuries / 38710000.0 + longitude_degrees) *
      kDegreesToRadians);
}

inline std::array<float, 4>
celestialToEnuQuaternion(double unix_seconds, const GeoLocation &location) {
  const double theta =
      localSiderealAngleRadians(unix_seconds, location.longitude_degrees);
  const double latitude = location.latitude_degrees * kDegreesToRadians;
  const double sin_theta = std::sin(theta);
  const double cos_theta = std::cos(theta);
  const double sin_latitude = std::sin(latitude);
  const double cos_latitude = std::cos(latitude);
  const double m00 = -sin_theta;
  const double m01 = cos_theta;
  const double m02 = 0.0;
  const double m10 = -sin_latitude * cos_theta;
  const double m11 = -sin_latitude * sin_theta;
  const double m12 = cos_latitude;
  const double m20 = cos_latitude * cos_theta;
  const double m21 = cos_latitude * sin_theta;
  const double m22 = sin_latitude;

  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  double w = 1.0;
  const double trace = m00 + m11 + m22;
  if (trace > 0.0) {
    const double scale = std::sqrt(trace + 1.0) * 2.0;
    w = 0.25 * scale;
    x = (m21 - m12) / scale;
    y = (m02 - m20) / scale;
    z = (m10 - m01) / scale;
  } else if (m00 > m11 && m00 > m22) {
    const double scale = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
    w = (m21 - m12) / scale;
    x = 0.25 * scale;
    y = (m01 + m10) / scale;
    z = (m02 + m20) / scale;
  } else if (m11 > m22) {
    const double scale = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
    w = (m02 - m20) / scale;
    x = (m01 + m10) / scale;
    y = 0.25 * scale;
    z = (m12 + m21) / scale;
  } else {
    const double scale = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
    w = (m10 - m01) / scale;
    x = (m02 + m20) / scale;
    y = (m12 + m21) / scale;
    z = 0.25 * scale;
  }
  const double inverse_length =
      1.0 / std::sqrt(x * x + y * y + z * z + w * w);
  return {static_cast<float>(x * inverse_length),
          static_cast<float>(y * inverse_length),
          static_cast<float>(z * inverse_length),
          static_cast<float>(w * inverse_length)};
}

} // namespace astronomy
} // namespace cloud
