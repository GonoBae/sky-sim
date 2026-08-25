#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace cloud {

constexpr std::uint16_t kProtocolV1 = 1;
constexpr std::uint16_t kProtocolV2 = 2;
constexpr std::size_t kV1HeaderBytes = 40;
constexpr std::size_t kV2HeaderBytes = 64;
constexpr std::size_t kPayloadBytes = 1200;
constexpr std::uint16_t kFlagKeyframe = 1U << 0U;
constexpr std::uint16_t kOccupancyBrickShift = 8;

enum class FieldId : std::uint8_t {
  Density = 1,
  Velocity = 2,
  Temperature = 3,
  Vapor = 4,
  Occupancy = 5,
};

enum class VoxelFormat : std::uint8_t {
  UNorm8 = 1,
  UNorm16 = 2,
  SNorm8 = 3,
  SNorm16 = 4,
};

enum class Compression : std::uint8_t {
  None = 0,
  Rle = 1,
};

enum class CompressionMode {
  None,
  Rle,
  Auto,
};

struct FieldPayload {
  std::uint16_t grid_x = 0;
  std::uint16_t grid_y = 0;
  std::uint16_t grid_z = 0;
  VoxelFormat voxel_format = VoxelFormat::UNorm8;
  FieldId field_id = FieldId::Density;
  std::uint8_t channel_count = 1;
  std::uint16_t flags = kFlagKeyframe;
  float value_scale = 1.0f;
  float value_bias = 0.0f;
  std::vector<std::uint8_t> bytes;
};

struct PacketHeaderV2 {
  std::uint32_t frame_id = 0;
  std::uint32_t field_crc32 = 0;
  double simulation_time = 0.0;
  std::uint16_t grid_x = 0;
  std::uint16_t grid_y = 0;
  std::uint16_t grid_z = 0;
  VoxelFormat voxel_format = VoxelFormat::UNorm8;
  FieldId field_id = FieldId::Density;
  std::uint8_t channel_count = 1;
  Compression compression = Compression::None;
  std::uint16_t chunk_index = 0;
  std::uint16_t chunk_count = 0;
  std::uint16_t payload_bytes = 0;
  std::uint16_t field_mask = 0;
  std::uint16_t flags = 0;
  std::uint32_t payload_offset = 0;
  std::uint32_t encoded_field_bytes = 0;
  std::uint32_t decoded_field_bytes = 0;
  float value_scale = 1.0f;
  float value_bias = 0.0f;
};

static_assert(sizeof(float) == 4, "Protocol v2 requires 32-bit float");
static_assert(sizeof(double) == 8, "Protocol v2 requires 64-bit double");

inline std::uint16_t fieldBit(FieldId field_id) {
  const auto value = static_cast<unsigned>(field_id);
  if (value < 1 || value > 16) {
    throw std::invalid_argument(
        "Field id cannot be represented in the field mask");
  }
  return static_cast<std::uint16_t>(1U << (value - 1U));
}

inline std::size_t bytesPerChannel(VoxelFormat format) {
  switch (format) {
  case VoxelFormat::UNorm8:
  case VoxelFormat::SNorm8:
    return 1;
  case VoxelFormat::UNorm16:
  case VoxelFormat::SNorm16:
    return 2;
  }
  throw std::invalid_argument("Unsupported voxel format");
}

namespace detail {

inline void writeU16(std::uint8_t *output, std::size_t offset,
                     std::uint16_t value) {
  output[offset] = static_cast<std::uint8_t>(value & 0xffU);
  output[offset + 1] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
}

inline void writeU32(std::uint8_t *output, std::size_t offset,
                     std::uint32_t value) {
  output[offset] = static_cast<std::uint8_t>(value & 0xffU);
  output[offset + 1] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
  output[offset + 2] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
  output[offset + 3] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
}

inline void writeU64(std::uint8_t *output, std::size_t offset,
                     std::uint64_t value) {
  for (std::size_t byte = 0; byte < 8; ++byte) {
    output[offset + byte] =
        static_cast<std::uint8_t>((value >> (byte * 8U)) & 0xffU);
  }
}

inline void writeF32(std::uint8_t *output, std::size_t offset, float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  writeU32(output, offset, bits);
}

inline void writeF64(std::uint8_t *output, std::size_t offset, double value) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  writeU64(output, offset, bits);
}

inline std::uint16_t readU16(const std::uint8_t *input, std::size_t offset) {
  return static_cast<std::uint16_t>(input[offset]) |
         static_cast<std::uint16_t>(
             static_cast<std::uint16_t>(input[offset + 1]) << 8U);
}

inline std::uint32_t readU32(const std::uint8_t *input, std::size_t offset) {
  return static_cast<std::uint32_t>(input[offset]) |
         (static_cast<std::uint32_t>(input[offset + 1]) << 8U) |
         (static_cast<std::uint32_t>(input[offset + 2]) << 16U) |
         (static_cast<std::uint32_t>(input[offset + 3]) << 24U);
}

inline std::uint64_t readU64(const std::uint8_t *input, std::size_t offset) {
  std::uint64_t value = 0;
  for (std::size_t byte = 0; byte < 8; ++byte) {
    value |= static_cast<std::uint64_t>(input[offset + byte]) << (byte * 8U);
  }
  return value;
}

inline float readF32(const std::uint8_t *input, std::size_t offset) {
  const std::uint32_t bits = readU32(input, offset);
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

inline double readF64(const std::uint8_t *input, std::size_t offset) {
  const std::uint64_t bits = readU64(input, offset);
  double value = 0.0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

} // namespace detail

inline std::array<std::uint8_t, kV2HeaderBytes>
serializeHeaderV2(const PacketHeaderV2 &header) {
  std::array<std::uint8_t, kV2HeaderBytes> output{};
  std::memcpy(output.data(), "CLD2", 4);
  detail::writeU16(output.data(), 4, kProtocolV2);
  detail::writeU16(output.data(), 6,
                   static_cast<std::uint16_t>(kV2HeaderBytes));
  detail::writeU32(output.data(), 8, header.frame_id);
  detail::writeU32(output.data(), 12, header.field_crc32);
  detail::writeF64(output.data(), 16, header.simulation_time);
  detail::writeU16(output.data(), 24, header.grid_x);
  detail::writeU16(output.data(), 26, header.grid_y);
  detail::writeU16(output.data(), 28, header.grid_z);
  output[30] = static_cast<std::uint8_t>(header.voxel_format);
  output[31] = static_cast<std::uint8_t>(header.field_id);
  output[32] = header.channel_count;
  output[33] = static_cast<std::uint8_t>(header.compression);
  detail::writeU16(output.data(), 34, header.chunk_index);
  detail::writeU16(output.data(), 36, header.chunk_count);
  detail::writeU16(output.data(), 38, header.payload_bytes);
  detail::writeU16(output.data(), 40, header.field_mask);
  detail::writeU16(output.data(), 42, header.flags);
  detail::writeU32(output.data(), 44, header.payload_offset);
  detail::writeU32(output.data(), 48, header.encoded_field_bytes);
  detail::writeU32(output.data(), 52, header.decoded_field_bytes);
  detail::writeF32(output.data(), 56, header.value_scale);
  detail::writeF32(output.data(), 60, header.value_bias);
  return output;
}

inline bool deserializeHeaderV2(const std::uint8_t *data, std::size_t size,
                                PacketHeaderV2 &output) {
  if (size < kV2HeaderBytes || std::memcmp(data, "CLD2", 4) != 0 ||
      detail::readU16(data, 4) != kProtocolV2 ||
      detail::readU16(data, 6) != kV2HeaderBytes) {
    return false;
  }

  output.frame_id = detail::readU32(data, 8);
  output.field_crc32 = detail::readU32(data, 12);
  output.simulation_time = detail::readF64(data, 16);
  output.grid_x = detail::readU16(data, 24);
  output.grid_y = detail::readU16(data, 26);
  output.grid_z = detail::readU16(data, 28);
  output.voxel_format = static_cast<VoxelFormat>(data[30]);
  output.field_id = static_cast<FieldId>(data[31]);
  output.channel_count = data[32];
  output.compression = static_cast<Compression>(data[33]);
  output.chunk_index = detail::readU16(data, 34);
  output.chunk_count = detail::readU16(data, 36);
  output.payload_bytes = detail::readU16(data, 38);
  output.field_mask = detail::readU16(data, 40);
  output.flags = detail::readU16(data, 42);
  output.payload_offset = detail::readU32(data, 44);
  output.encoded_field_bytes = detail::readU32(data, 48);
  output.decoded_field_bytes = detail::readU32(data, 52);
  output.value_scale = detail::readF32(data, 56);
  output.value_bias = detail::readF32(data, 60);
  return true;
}

inline std::uint32_t crc32(const std::uint8_t *data, std::size_t size) {
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> values{};
    for (std::uint32_t index = 0; index < values.size(); ++index) {
      std::uint32_t value = index;
      for (int bit = 0; bit < 8; ++bit) {
        value = (value & 1U) != 0U ? (value >> 1U) ^ 0xedb88320U : value >> 1U;
      }
      values[index] = value;
    }
    return values;
  }();

  std::uint32_t crc = 0xffffffffU;
  for (std::size_t index = 0; index < size; ++index) {
    crc = table[(crc ^ data[index]) & 0xffU] ^ (crc >> 8U);
  }
  return crc ^ 0xffffffffU;
}

inline std::uint32_t crc32(const std::vector<std::uint8_t> &data) {
  return crc32(data.data(), data.size());
}

inline std::vector<std::uint8_t>
rleCompress(const std::vector<std::uint8_t> &input) {
  std::vector<std::uint8_t> output;
  output.reserve(input.size());
  std::size_t position = 0;

  auto runLength = [&](std::size_t start) {
    std::size_t length = 1;
    while (start + length < input.size() && length < 130 &&
           input[start + length] == input[start]) {
      ++length;
    }
    return length;
  };

  while (position < input.size()) {
    const std::size_t repeat = runLength(position);
    if (repeat >= 3) {
      output.push_back(static_cast<std::uint8_t>(0x80U | (repeat - 3U)));
      output.push_back(input[position]);
      position += repeat;
      continue;
    }

    const std::size_t literal_start = position;
    std::size_t literal_length = 0;
    while (position < input.size() && literal_length < 128) {
      const std::size_t next_repeat = runLength(position);
      if (next_repeat >= 3) {
        break;
      }
      const std::size_t take = std::min(next_repeat, 128U - literal_length);
      position += take;
      literal_length += take;
    }
    output.push_back(static_cast<std::uint8_t>(literal_length - 1U));
    output.insert(output.end(),
                  input.begin() + static_cast<std::ptrdiff_t>(literal_start),
                  input.begin() + static_cast<std::ptrdiff_t>(literal_start +
                                                              literal_length));
  }
  return output;
}

inline bool rleDecompress(const std::vector<std::uint8_t> &input,
                          std::size_t expected_size,
                          std::vector<std::uint8_t> &output) {
  output.clear();
  output.reserve(expected_size);
  std::size_t position = 0;
  while (position < input.size()) {
    const std::uint8_t control = input[position++];
    if ((control & 0x80U) != 0U) {
      if (position >= input.size()) {
        return false;
      }
      const std::size_t length = static_cast<std::size_t>(control & 0x7fU) + 3U;
      if (output.size() > expected_size ||
          length > expected_size - output.size()) {
        return false;
      }
      output.insert(output.end(), length, input[position++]);
    } else {
      const std::size_t length = static_cast<std::size_t>(control) + 1U;
      if (length > input.size() - position || output.size() > expected_size ||
          length > expected_size - output.size()) {
        return false;
      }
      output.insert(
          output.end(), input.begin() + static_cast<std::ptrdiff_t>(position),
          input.begin() + static_cast<std::ptrdiff_t>(position + length));
      position += length;
    }
  }
  return output.size() == expected_size;
}

class UdpSender {
public:
  UdpSender(const std::string &host, std::uint16_t port) {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
      throw std::runtime_error("WSAStartup failed");
    }
#endif
    socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == invalidSocket()) {
      cleanupPlatform();
      throw std::runtime_error("Could not create UDP socket");
    }

    const int send_buffer_bytes = 4 * 1024 * 1024;
    setsockopt(socketHandle(), SOL_SOCKET, SO_SNDBUF,
               reinterpret_cast<const char *>(&send_buffer_bytes),
               sizeof(send_buffer_bytes));
    setNonBlocking();

    address_.sin_family = AF_INET;
    address_.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &address_.sin_addr) != 1) {
      closeSocket();
      cleanupPlatform();
      throw std::runtime_error("Destination must be an IPv4 address: " + host);
    }
  }

  ~UdpSender() {
    closeSocket();
    cleanupPlatform();
  }

  UdpSender(const UdpSender &) = delete;
  UdpSender &operator=(const UdpSender &) = delete;

  std::uint64_t totalBytesSent() const { return total_bytes_sent_; }
  std::uint64_t totalPacketsSent() const { return total_packets_sent_; }
  std::uint64_t droppedPackets() const { return dropped_packets_; }
  std::uint64_t droppedFrames() const { return dropped_frames_; }

  void sendDensityV1(const std::vector<std::uint8_t> &density, int grid_size,
                     std::uint32_t frame_id, float simulation_time) {
    if (density.empty() ||
        density.size() > std::numeric_limits<std::uint32_t>::max()) {
      throw std::runtime_error("Invalid protocol v1 density field size");
    }
    const std::size_t chunk_count_size =
        (density.size() + kPayloadBytes - 1) / kPayloadBytes;
    if (chunk_count_size > std::numeric_limits<std::uint16_t>::max()) {
      throw std::runtime_error("Frame is too large for protocol v1");
    }
    const auto chunk_count = static_cast<std::uint16_t>(chunk_count_size);
    std::vector<std::uint8_t> packet(kV1HeaderBytes + kPayloadBytes);

    for (std::uint16_t chunk = 0; chunk < chunk_count; ++chunk) {
      const std::size_t offset =
          static_cast<std::size_t>(chunk) * kPayloadBytes;
      const std::size_t payload_size =
          std::min(kPayloadBytes, density.size() - offset);
      std::fill(packet.begin(), packet.begin() + kV1HeaderBytes, 0U);
      std::memcpy(packet.data(), "CLD1", 4);
      detail::writeU16(packet.data(), 4, kProtocolV1);
      detail::writeU16(packet.data(), 6,
                       static_cast<std::uint16_t>(kV1HeaderBytes));
      detail::writeU32(packet.data(), 8, frame_id);
      detail::writeF32(packet.data(), 12, simulation_time);
      detail::writeU16(packet.data(), 16,
                       static_cast<std::uint16_t>(grid_size));
      detail::writeU16(packet.data(), 18,
                       static_cast<std::uint16_t>(grid_size));
      detail::writeU16(packet.data(), 20,
                       static_cast<std::uint16_t>(grid_size));
      packet[22] = static_cast<std::uint8_t>(VoxelFormat::UNorm8);
      packet[23] = static_cast<std::uint8_t>(FieldId::Density);
      detail::writeU16(packet.data(), 24, chunk);
      detail::writeU16(packet.data(), 26, chunk_count);
      detail::writeU16(packet.data(), 28,
                       static_cast<std::uint16_t>(payload_size));
      detail::writeU32(packet.data(), 32, static_cast<std::uint32_t>(offset));
      detail::writeU32(packet.data(), 36,
                       static_cast<std::uint32_t>(density.size()));
      std::memcpy(packet.data() + kV1HeaderBytes, density.data() + offset,
                  payload_size);
      if (!sendPacket(packet.data(), kV1HeaderBytes + payload_size)) {
        ++dropped_frames_;
        break;
      }
    }
  }

  void sendFrameV2(const std::vector<FieldPayload> &fields,
                   std::uint32_t frame_id, double simulation_time,
                   CompressionMode compression_mode) {
    if (fields.empty()) {
      throw std::invalid_argument("Protocol v2 frame has no fields");
    }

    std::uint16_t field_mask = 0;
    for (const FieldPayload &field : fields) {
      const std::uint16_t bit = fieldBit(field.field_id);
      if ((field_mask & bit) != 0U) {
        throw std::invalid_argument(
            "Protocol v2 frame contains a duplicate field");
      }
      field_mask = static_cast<std::uint16_t>(field_mask | bit);
      validateField(field);
    }

    for (const FieldPayload &field : fields) {
      if (!sendFieldV2(field, field_mask, frame_id, simulation_time,
                       compression_mode)) {
        ++dropped_frames_;
        break;
      }
    }
  }

private:
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

  static void validateField(const FieldPayload &field) {
    if (field.grid_x == 0 || field.grid_y == 0 || field.grid_z == 0 ||
        field.channel_count == 0 || field.bytes.empty()) {
      throw std::invalid_argument(
          "Protocol v2 field dimensions and data must be non-zero");
    }
    const bool layout_is_valid =
        (field.field_id == FieldId::Density &&
         field.voxel_format == VoxelFormat::UNorm16 &&
         field.channel_count == 1) ||
        (field.field_id == FieldId::Velocity &&
         field.voxel_format == VoxelFormat::SNorm16 &&
         field.channel_count == 3) ||
        ((field.field_id == FieldId::Temperature ||
          field.field_id == FieldId::Vapor ||
          field.field_id == FieldId::Occupancy) &&
         field.voxel_format == VoxelFormat::UNorm8 && field.channel_count == 1);
    if (!layout_is_valid) {
      throw std::invalid_argument(
          "Protocol v2 field format does not match its definition");
    }
    const std::uint64_t expected = static_cast<std::uint64_t>(field.grid_x) *
                                   static_cast<std::uint64_t>(field.grid_y) *
                                   static_cast<std::uint64_t>(field.grid_z) *
                                   field.channel_count *
                                   bytesPerChannel(field.voxel_format);
    if (expected != field.bytes.size() ||
        expected > std::numeric_limits<std::uint32_t>::max()) {
      throw std::invalid_argument(
          "Protocol v2 field byte count does not match its shape");
    }
  }

  bool sendFieldV2(const FieldPayload &field, std::uint16_t field_mask,
                   std::uint32_t frame_id, double simulation_time,
                   CompressionMode compression_mode) {
    Compression compression = Compression::None;
    const std::vector<std::uint8_t> *encoded = &field.bytes;
    std::vector<std::uint8_t> compressed;
    if (compression_mode != CompressionMode::None) {
      compressed = rleCompress(field.bytes);
      if (compression_mode == CompressionMode::Rle ||
          compressed.size() + kV2HeaderBytes < field.bytes.size()) {
        compression = Compression::Rle;
        encoded = &compressed;
      }
    }

    if (encoded->empty() ||
        encoded->size() > std::numeric_limits<std::uint32_t>::max()) {
      throw std::runtime_error("Protocol v2 encoded field size is invalid");
    }
    const std::size_t chunk_count_size =
        (encoded->size() + kPayloadBytes - 1) / kPayloadBytes;
    if (chunk_count_size > std::numeric_limits<std::uint16_t>::max()) {
      throw std::runtime_error("Field is too large for protocol v2");
    }

    const auto chunk_count = static_cast<std::uint16_t>(chunk_count_size);
    const std::uint32_t checksum = crc32(field.bytes);
    std::vector<std::uint8_t> packet(kV2HeaderBytes + kPayloadBytes);
    for (std::uint16_t chunk = 0; chunk < chunk_count; ++chunk) {
      const std::size_t offset =
          static_cast<std::size_t>(chunk) * kPayloadBytes;
      const std::size_t payload_size =
          std::min(kPayloadBytes, encoded->size() - offset);
      PacketHeaderV2 header;
      header.frame_id = frame_id;
      header.field_crc32 = checksum;
      header.simulation_time = simulation_time;
      header.grid_x = field.grid_x;
      header.grid_y = field.grid_y;
      header.grid_z = field.grid_z;
      header.voxel_format = field.voxel_format;
      header.field_id = field.field_id;
      header.channel_count = field.channel_count;
      header.compression = compression;
      header.chunk_index = chunk;
      header.chunk_count = chunk_count;
      header.payload_bytes = static_cast<std::uint16_t>(payload_size);
      header.field_mask = field_mask;
      header.flags = field.flags;
      header.payload_offset = static_cast<std::uint32_t>(offset);
      header.encoded_field_bytes = static_cast<std::uint32_t>(encoded->size());
      header.decoded_field_bytes =
          static_cast<std::uint32_t>(field.bytes.size());
      header.value_scale = field.value_scale;
      header.value_bias = field.value_bias;

      const auto serialized = serializeHeaderV2(header);
      std::memcpy(packet.data(), serialized.data(), serialized.size());
      std::memcpy(packet.data() + kV2HeaderBytes, encoded->data() + offset,
                  payload_size);
      if (!sendPacket(packet.data(), kV2HeaderBytes + payload_size)) {
        return false;
      }
    }
    return true;
  }

  bool sendPacket(const std::uint8_t *data, std::size_t size) {
    const auto sent =
        ::sendto(socketHandle(), reinterpret_cast<const char *>(data),
                 static_cast<int>(size), 0,
                 reinterpret_cast<const sockaddr *>(&address_),
                 static_cast<socklen_type>(sizeof(address_)));
    if (sent < 0) {
      const int error = socketError();
      if (isTransientSendError(error)) {
        ++dropped_packets_;
        return false;
      }
      throw std::runtime_error("UDP send failed with socket error " +
                               std::to_string(error));
    }
    if (static_cast<std::size_t>(sent) != size) {
      throw std::runtime_error("UDP send returned a short datagram");
    }
    total_bytes_sent_ += size;
    ++total_packets_sent_;
    return true;
  }

  static int socketError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
  }

  static bool isTransientSendError(int error) {
#ifdef _WIN32
    return error == WSAEWOULDBLOCK || error == WSAENOBUFS || error == WSAEINTR;
#else
    return error == EAGAIN || error == EWOULDBLOCK || error == ENOBUFS ||
           error == EINTR;
#endif
  }

  void setNonBlocking() {
#ifdef _WIN32
    u_long enabled = 1;
    if (ioctlsocket(socket_, FIONBIO, &enabled) != 0) {
      closeSocket();
      cleanupPlatform();
      throw std::runtime_error("Could not make UDP sender non-blocking");
    }
#else
    const int flags = fcntl(socket_, F_GETFL, 0);
    if (flags < 0 || fcntl(socket_, F_SETFL, flags | O_NONBLOCK) != 0) {
      closeSocket();
      cleanupPlatform();
      throw std::runtime_error("Could not make UDP sender non-blocking");
    }
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

  static void cleanupPlatform() {
#ifdef _WIN32
    WSACleanup();
#endif
  }

  socket_type socket_ = invalidSocket();
  sockaddr_in address_{};
  std::uint64_t total_bytes_sent_ = 0;
  std::uint64_t total_packets_sent_ = 0;
  std::uint64_t dropped_packets_ = 0;
  std::uint64_t dropped_frames_ = 0;
};

} // namespace cloud
