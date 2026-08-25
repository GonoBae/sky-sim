#pragma once

#include "protocol.hpp"
#include "simulation.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <fcntl.h>
#endif

namespace cloud {

constexpr std::uint16_t kControlVersion = 2;
constexpr std::size_t kControlPacketBytes = 128;
constexpr std::size_t kControlCrcOffset = 124;

enum class ControlAction : std::uint8_t {
  Upsert = 1,
  Remove = 2,
  ClearSession = 3,
};

struct InteractorCommand {
  std::uint32_t session_id = 0;
  std::uint32_t sequence = 0;
  std::uint64_t interactor_id = 0;
  double client_time = 0.0;
  ControlAction action = ControlAction::Upsert;
  CloudInteractor interactor;
  std::uint32_t ttl_ms = 300;
  float strength = 1.0f;
};

inline std::array<std::uint8_t, kControlPacketBytes>
serializeInteractorCommand(const InteractorCommand &command) {
  std::array<std::uint8_t, kControlPacketBytes> output{};
  std::memcpy(output.data(), "CLC2", 4);
  detail::writeU16(output.data(), 4, kControlVersion);
  detail::writeU16(output.data(), 6,
                   static_cast<std::uint16_t>(kControlPacketBytes));
  detail::writeU32(output.data(), 8, command.session_id);
  detail::writeU32(output.data(), 12, command.sequence);
  detail::writeU64(output.data(), 16, command.interactor_id);
  detail::writeF64(output.data(), 24, command.client_time);
  output[32] = static_cast<std::uint8_t>(command.action);
  if (command.action == ControlAction::Upsert) {
    output[33] = static_cast<std::uint8_t>(command.interactor.shape);
    detail::writeU16(output.data(), 34, command.interactor.flags);
    detail::writeU32(output.data(), 36, command.ttl_ms);
    detail::writeF32(output.data(), 40, command.interactor.position.x);
    detail::writeF32(output.data(), 44, command.interactor.position.y);
    detail::writeF32(output.data(), 48, command.interactor.position.z);
    detail::writeF32(output.data(), 52, command.interactor.rotation.x);
    detail::writeF32(output.data(), 56, command.interactor.rotation.y);
    detail::writeF32(output.data(), 60, command.interactor.rotation.z);
    detail::writeF32(output.data(), 64, command.interactor.rotation.w);
    detail::writeF32(output.data(), 68, command.interactor.half_extents.x);
    detail::writeF32(output.data(), 72, command.interactor.half_extents.y);
    detail::writeF32(output.data(), 76, command.interactor.half_extents.z);
    detail::writeF32(output.data(), 80, command.interactor.linear_velocity.x);
    detail::writeF32(output.data(), 84, command.interactor.linear_velocity.y);
    detail::writeF32(output.data(), 88, command.interactor.linear_velocity.z);
    detail::writeF32(output.data(), 92, command.interactor.angular_velocity.x);
    detail::writeF32(output.data(), 96, command.interactor.angular_velocity.y);
    detail::writeF32(output.data(), 100, command.interactor.angular_velocity.z);
    detail::writeF32(output.data(), 104, command.strength);
  }
  detail::writeU32(output.data(), kControlCrcOffset,
                   crc32(output.data(), kControlCrcOffset));
  return output;
}

inline bool deserializeInteractorCommand(const std::uint8_t *data,
                                         std::size_t size,
                                         InteractorCommand &output) {
  if (size != kControlPacketBytes || std::memcmp(data, "CLC2", 4) != 0 ||
      detail::readU16(data, 4) != kControlVersion ||
      detail::readU16(data, 6) != kControlPacketBytes ||
      detail::readU32(data, kControlCrcOffset) !=
          crc32(data, kControlCrcOffset)) {
    return false;
  }
  for (std::size_t offset = 108; offset < kControlCrcOffset; ++offset) {
    if (data[offset] != 0U) {
      return false;
    }
  }

  output.session_id = detail::readU32(data, 8);
  output.sequence = detail::readU32(data, 12);
  output.interactor_id = detail::readU64(data, 16);
  output.client_time = detail::readF64(data, 24);
  output.action = static_cast<ControlAction>(data[32]);
  output.interactor.id = output.interactor_id;
  output.interactor.shape = static_cast<InteractorShape>(data[33]);
  output.interactor.flags = detail::readU16(data, 34);
  output.ttl_ms = detail::readU32(data, 36);
  output.interactor.position = {detail::readF32(data, 40),
                                detail::readF32(data, 44),
                                detail::readF32(data, 48)};
  output.interactor.rotation = {
      detail::readF32(data, 52), detail::readF32(data, 56),
      detail::readF32(data, 60), detail::readF32(data, 64)};
  output.interactor.half_extents = {detail::readF32(data, 68),
                                    detail::readF32(data, 72),
                                    detail::readF32(data, 76)};
  output.interactor.linear_velocity = {detail::readF32(data, 80),
                                       detail::readF32(data, 84),
                                       detail::readF32(data, 88)};
  output.interactor.angular_velocity = {detail::readF32(data, 92),
                                        detail::readF32(data, 96),
                                        detail::readF32(data, 100)};
  output.strength = detail::readF32(data, 104);
  output.interactor.displacement_strength = output.strength;
  output.interactor.wake_strength = output.strength;
  output.interactor.turbulence_strength = 0.35f * output.strength;
  return true;
}

class InteractorControlReceiver {
public:
  InteractorControlReceiver(const std::string &host, std::uint16_t port,
                            std::size_t max_interactors)
      : max_interactors_(max_interactors) {
    if (max_interactors == 0 || max_interactors > 64) {
      throw std::invalid_argument("Max interactors must be between 1 and 64");
    }
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
      throw std::runtime_error("Control WSAStartup failed");
    }
    platform_started_ = true;
#endif
    try {
      socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
      if (socket_ == invalidSocket()) {
        throw std::runtime_error("Could not create interactor control socket");
      }
      const int receive_buffer_bytes = 1024 * 1024;
      (void)setsockopt(socketHandle(), SOL_SOCKET, SO_RCVBUF,
                       reinterpret_cast<const char *>(&receive_buffer_bytes),
                       sizeof(receive_buffer_bytes));
      setNonBlocking();

      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_port = htons(port);
      if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        throw std::runtime_error("Control bind address must be IPv4: " + host);
      }
      if (::bind(socketHandle(), reinterpret_cast<const sockaddr *>(&address),
                 static_cast<socklen_type>(sizeof(address))) != 0) {
        throw std::runtime_error("Could not bind interactor control socket");
      }
    } catch (...) {
      closeSocket();
      cleanupPlatform();
      throw;
    }
  }

  ~InteractorControlReceiver() {
    closeSocket();
    cleanupPlatform();
  }

  InteractorControlReceiver(const InteractorControlReceiver &) = delete;
  InteractorControlReceiver &
  operator=(const InteractorControlReceiver &) = delete;

  void poll(std::size_t max_packets = 512) {
    expireEntries();
    std::array<std::uint8_t, 512> packet{};
    max_packets = std::min<std::size_t>(max_packets, 4096);
    for (std::size_t packet_index = 0; packet_index < max_packets;
         ++packet_index) {
      sockaddr_in source{};
      socklen_type source_size = static_cast<socklen_type>(sizeof(source));
      const int received = static_cast<int>(
          ::recvfrom(socketHandle(), reinterpret_cast<char *>(packet.data()),
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
        throw std::runtime_error("Interactor control receive failed");
      }

      InteractorCommand command;
      if (!deserializeInteractorCommand(
              packet.data(), static_cast<std::size_t>(received), command) ||
          !validateCommand(command)) {
        ++rejected_packets_;
        continue;
      }
      const SessionKey key{ntohl(source.sin_addr.s_addr),
                           ntohs(source.sin_port), command.session_id};
      if (!applyCommand(key, command)) {
        ++rejected_packets_;
        continue;
      }
      ++accepted_packets_;
    }
  }

  std::vector<CloudInteractor> activeInteractors() {
    expireEntries();
    std::vector<CloudInteractor> result;
    result.reserve(activeCount());
    for (auto &session_pair : sessions_) {
      const SessionKey &session_key = session_pair.first;
      for (auto &entry_pair : session_pair.second.entries) {
        Entry &entry = entry_pair.second;
        CloudInteractor interactor = entry.interactor;
        interactor.id = compositeId(session_key, entry_pair.first);
        if (entry.has_snapshot) {
          interactor.previous_position = entry.snapshot_position;
          interactor.previous_rotation = entry.snapshot_rotation;
          const double client_interval =
              entry.client_time - entry.snapshot_client_time;
          if (client_interval > 0.0 && client_interval <= 0.25) {
            interactor.transform_interval_seconds =
                static_cast<float>(client_interval);
            interactor.has_previous_transform = true;
          } else {
            interactor.previous_position = interactor.position;
            interactor.previous_rotation = interactor.rotation;
            interactor.transform_interval_seconds = 0.0f;
            interactor.has_previous_transform = false;
          }
        } else {
          interactor.previous_position = interactor.position;
          interactor.previous_rotation = interactor.rotation;
          interactor.transform_interval_seconds = 0.0f;
          interactor.has_previous_transform = false;
        }
        entry.snapshot_position = entry.interactor.position;
        entry.snapshot_rotation = entry.interactor.rotation;
        entry.snapshot_client_time = entry.client_time;
        entry.has_snapshot = true;
        result.push_back(interactor);
      }
    }
    std::sort(result.begin(), result.end(),
              [](const CloudInteractor &a, const CloudInteractor &b) {
                return a.id < b.id;
              });
    return result;
  }

  std::size_t activeCount() const {
    std::size_t count = 0;
    for (const auto &pair : sessions_) {
      count += pair.second.entries.size();
    }
    return count;
  }
  std::uint64_t acceptedPackets() const { return accepted_packets_; }
  std::uint64_t rejectedPackets() const { return rejected_packets_; }
  std::uint64_t stalePackets() const { return stale_packets_; }
  std::uint64_t limitedPackets() const { return limited_packets_; }
  std::uint64_t expiredInteractors() const { return expired_interactors_; }

private:
  using clock = std::chrono::steady_clock;

  struct SessionKey {
    std::uint32_t address = 0;
    std::uint16_t port = 0;
    std::uint32_t session_id = 0;
    bool operator==(const SessionKey &other) const {
      return address == other.address && port == other.port &&
             session_id == other.session_id;
    }
  };

  struct SessionKeyHash {
    std::size_t operator()(const SessionKey &key) const {
      std::uint64_t value =
          (static_cast<std::uint64_t>(key.address) << 32U) ^
          (static_cast<std::uint64_t>(key.session_id) * 0x9e3779b97f4a7c15ULL) ^
          key.port;
      value ^= value >> 33U;
      value *= 0xff51afd7ed558ccdULL;
      return static_cast<std::size_t>(value ^ (value >> 33U));
    }
  };

  struct Entry {
    CloudInteractor interactor;
    clock::time_point expires_at;
    std::uint32_t ttl_ms = 300;
    double client_time = 0.0;
    SimulationVector snapshot_position{};
    SimulationQuaternion snapshot_rotation{};
    double snapshot_client_time = 0.0;
    bool has_snapshot = false;
  };

  struct Session {
    std::unordered_map<std::uint64_t, Entry> entries;
    std::uint32_t last_sequence = 0;
    bool has_sequence = false;
    clock::time_point last_seen = clock::now();
  };

  struct SequenceTombstone {
    std::uint32_t sequence = 0;
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

  static bool finiteVector(SimulationVector value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
  }
  static bool finiteQuaternion(SimulationQuaternion value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z) && std::isfinite(value.w);
  }
  static bool newerSequence(std::uint32_t sequence, std::uint32_t previous) {
    const std::uint32_t difference = sequence - previous;
    return difference != 0U && difference < 0x80000000U;
  }
  static std::uint64_t compositeId(const SessionKey &key, std::uint64_t id) {
    std::uint64_t value =
        id ^ (static_cast<std::uint64_t>(key.address) << 32U) ^
        (static_cast<std::uint64_t>(key.session_id) << 1U) ^ key.port;
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
  }

  bool validateCommand(const InteractorCommand &command) const {
    if (command.session_id == 0 || !std::isfinite(command.client_time) ||
        command.client_time < 0.0) {
      return false;
    }
    const CloudInteractor &value = command.interactor;
    const bool canonical_empty_tail =
        static_cast<std::uint8_t>(value.shape) == 0U && value.flags == 0U &&
        command.ttl_ms == 0U && value.position.x == 0.0f &&
        value.position.y == 0.0f && value.position.z == 0.0f &&
        value.rotation.x == 0.0f && value.rotation.y == 0.0f &&
        value.rotation.z == 0.0f && value.rotation.w == 0.0f &&
        value.half_extents.x == 0.0f && value.half_extents.y == 0.0f &&
        value.half_extents.z == 0.0f && value.linear_velocity.x == 0.0f &&
        value.linear_velocity.y == 0.0f && value.linear_velocity.z == 0.0f &&
        value.angular_velocity.x == 0.0f && value.angular_velocity.y == 0.0f &&
        value.angular_velocity.z == 0.0f && command.strength == 0.0f;
    if (command.action == ControlAction::ClearSession) {
      return command.interactor_id == 0 && canonical_empty_tail;
    }
    if (command.action != ControlAction::Upsert &&
        command.action != ControlAction::Remove) {
      return false;
    }
    if (command.interactor_id == 0) {
      return false;
    }
    if (command.action == ControlAction::Remove) {
      return canonical_empty_tail;
    }

    const std::uint16_t supported_flags =
        kInteractorSolid | kInteractorDisplaceScalars | kInteractorGenerateWake;
    const float quaternion_length_squared =
        value.rotation.x * value.rotation.x +
        value.rotation.y * value.rotation.y +
        value.rotation.z * value.rotation.z +
        value.rotation.w * value.rotation.w;
    const float maximum_extent = std::max(
        {value.half_extents.x, value.half_extents.y, value.half_extents.z});
    const float bounding_extent =
        value.shape == InteractorShape::Box
            ? std::sqrt(value.half_extents.x * value.half_extents.x +
                        value.half_extents.y * value.half_extents.y +
                        value.half_extents.z * value.half_extents.z)
            : maximum_extent;
    const float linear_speed =
        std::sqrt(value.linear_velocity.x * value.linear_velocity.x +
                  value.linear_velocity.y * value.linear_velocity.y +
                  value.linear_velocity.z * value.linear_velocity.z);
    const float angular_speed =
        std::sqrt(value.angular_velocity.x * value.angular_velocity.x +
                  value.angular_velocity.y * value.angular_velocity.y +
                  value.angular_velocity.z * value.angular_velocity.z);
    const bool valid_shape = value.shape == InteractorShape::Sphere ||
                             value.shape == InteractorShape::Box ||
                             value.shape == InteractorShape::CapsuleZ ||
                             value.shape == InteractorShape::Ellipsoid;
    const bool sphere_dimensions =
        value.shape != InteractorShape::Sphere ||
        (std::abs(value.half_extents.x - value.half_extents.y) <= 0.0001f &&
         std::abs(value.half_extents.x - value.half_extents.z) <= 0.0001f);
    const bool capsule_dimensions =
        value.shape != InteractorShape::CapsuleZ ||
        (std::abs(value.half_extents.x - value.half_extents.y) <= 0.0001f &&
         value.half_extents.z >= value.half_extents.x);
    return valid_shape && sphere_dimensions && capsule_dimensions &&
           value.flags != 0U && (value.flags & ~supported_flags) == 0U &&
           finiteVector(value.position) && finiteQuaternion(value.rotation) &&
           finiteVector(value.half_extents) &&
           finiteVector(value.linear_velocity) &&
           finiteVector(value.angular_velocity) &&
           std::isfinite(command.strength) && value.position.x >= 0.0f &&
           value.position.x <= 1.0f && value.position.y >= 0.0f &&
           value.position.y <= 1.0f && value.position.z >= 0.0f &&
           value.position.z <= 1.0f && value.half_extents.x > 0.0f &&
           value.half_extents.y > 0.0f && value.half_extents.z > 0.0f &&
           bounding_extent <= 0.45f && linear_speed <= 2.0f &&
           std::abs(value.angular_velocity.x) <= 100.0f &&
           std::abs(value.angular_velocity.y) <= 100.0f &&
           std::abs(value.angular_velocity.z) <= 100.0f &&
           angular_speed * bounding_extent <= 2.0f &&
           quaternion_length_squared >= 0.81f &&
           quaternion_length_squared <= 1.21f && command.strength > 0.0f &&
           command.strength <= 4.0f && command.ttl_ms >= 50U &&
           command.ttl_ms <= 2000U;
  }

  bool applyCommand(const SessionKey &key, const InteractorCommand &command) {
    auto session_iterator = sessions_.find(key);
    if (session_iterator == sessions_.end()) {
      auto tombstone_iterator = sequence_tombstones_.find(key);
      if (tombstone_iterator != sequence_tombstones_.end() &&
          !newerSequence(command.sequence,
                         tombstone_iterator->second.sequence)) {
        ++stale_packets_;
        return false;
      }
      if (command.action != ControlAction::Upsert) {
        recordSequenceTombstone(key, command.sequence);
        return true;
      }
      discardIdleSessions();
      if (sessions_.size() >= kMaximumSessions) {
        ++limited_packets_;
        return false;
      }
      if (activeCount() >= max_interactors_) {
        ++limited_packets_;
        return false;
      }
      sequence_tombstones_.erase(key);
      session_iterator = sessions_.emplace(key, Session{}).first;
    }
    Session &session = session_iterator->second;
    if (session.has_sequence &&
        !newerSequence(command.sequence, session.last_sequence)) {
      ++stale_packets_;
      return false;
    }
    session.last_sequence = command.sequence;
    session.has_sequence = true;
    session.last_seen = clock::now();

    if (command.action == ControlAction::ClearSession) {
      session.entries.clear();
      recordSequenceTombstone(key, command.sequence);
      sessions_.erase(session_iterator);
      return true;
    }
    if (command.action == ControlAction::Remove) {
      session.entries.erase(command.interactor_id);
      if (session.entries.empty()) {
        recordSequenceTombstone(key, command.sequence);
        sessions_.erase(session_iterator);
      }
      return true;
    }

    auto entry_iterator = session.entries.find(command.interactor_id);
    if (entry_iterator == session.entries.end()) {
      if (activeCount() >= max_interactors_) {
        ++limited_packets_;
        return false;
      }
      entry_iterator =
          session.entries.emplace(command.interactor_id, Entry{}).first;
    }
    Entry &entry = entry_iterator->second;
    entry.interactor = command.interactor;
    entry.ttl_ms = command.ttl_ms;
    entry.client_time = command.client_time;
    entry.expires_at = clock::now() + std::chrono::milliseconds(command.ttl_ms);
    return true;
  }

  void expireEntries() {
    const auto now = clock::now();
    for (auto session_iterator = sessions_.begin();
         session_iterator != sessions_.end();) {
      auto &entries = session_iterator->second.entries;
      for (auto iterator = entries.begin(); iterator != entries.end();) {
        if (iterator->second.expires_at <= now) {
          iterator = entries.erase(iterator);
          ++expired_interactors_;
        } else {
          ++iterator;
        }
      }
      if (entries.empty()) {
        if (session_iterator->second.has_sequence) {
          recordSequenceTombstone(session_iterator->first,
                                  session_iterator->second.last_sequence);
        }
        session_iterator = sessions_.erase(session_iterator);
      } else {
        ++session_iterator;
      }
    }
    discardIdleSessions();
  }

  void discardIdleSessions() {
    const auto cutoff = clock::now() - std::chrono::seconds(30);
    for (auto iterator = sessions_.begin(); iterator != sessions_.end();) {
      if (iterator->second.entries.empty() &&
          iterator->second.last_seen < cutoff) {
        iterator = sessions_.erase(iterator);
      } else {
        ++iterator;
      }
    }
    for (auto iterator = sequence_tombstones_.begin();
         iterator != sequence_tombstones_.end();) {
      if (iterator->second.last_seen < cutoff) {
        iterator = sequence_tombstones_.erase(iterator);
      } else {
        ++iterator;
      }
    }
  }

  void recordSequenceTombstone(const SessionKey &key, std::uint32_t sequence) {
    if (sequence_tombstones_.size() >= kMaximumSequenceTombstones &&
        sequence_tombstones_.find(key) == sequence_tombstones_.end()) {
      const auto oldest = std::min_element(
          sequence_tombstones_.begin(), sequence_tombstones_.end(),
          [](const auto &a, const auto &b) {
            return a.second.last_seen < b.second.last_seen;
          });
      if (oldest != sequence_tombstones_.end()) {
        sequence_tombstones_.erase(oldest);
      }
    }
    sequence_tombstones_[key] = {sequence, clock::now()};
  }

  void setNonBlocking() {
#ifdef _WIN32
    u_long enabled = 1;
    if (ioctlsocket(socket_, FIONBIO, &enabled) != 0) {
      throw std::runtime_error("Could not make control socket non-blocking");
    }
#else
    const int flags = fcntl(socket_, F_GETFL, 0);
    if (flags < 0 || fcntl(socket_, F_SETFL, flags | O_NONBLOCK) != 0) {
      throw std::runtime_error("Could not make control socket non-blocking");
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

  static constexpr std::size_t kMaximumSessions = 32;
  static constexpr std::size_t kMaximumSequenceTombstones = 256;
  socket_type socket_ = invalidSocket();
  std::size_t max_interactors_ = 0;
  std::unordered_map<SessionKey, Session, SessionKeyHash> sessions_;
  std::unordered_map<SessionKey, SequenceTombstone, SessionKeyHash>
      sequence_tombstones_;
#ifdef _WIN32
  bool platform_started_ = false;
#endif
  std::uint64_t accepted_packets_ = 0;
  std::uint64_t rejected_packets_ = 0;
  std::uint64_t stale_packets_ = 0;
  std::uint64_t limited_packets_ = 0;
  std::uint64_t expired_interactors_ = 0;
};

} // namespace cloud
