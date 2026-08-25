#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace cloud {

constexpr std::uint16_t kProtocolVersion = 1;
constexpr std::size_t kPayloadBytes = 1200;

enum class FieldId : std::uint8_t {
    Density = 1,
};

enum class VoxelFormat : std::uint8_t {
    UInt8 = 1,
};

#pragma pack(push, 1)
struct PacketHeader {
    char magic[4];                 // "CLD1"
    std::uint16_t version;
    std::uint16_t header_bytes;
    std::uint32_t frame_id;
    float simulation_time;
    std::uint16_t grid_x;
    std::uint16_t grid_y;
    std::uint16_t grid_z;
    std::uint8_t voxel_format;
    std::uint8_t field_id;
    std::uint16_t chunk_index;
    std::uint16_t chunk_count;
    std::uint16_t payload_bytes;
    std::uint16_t reserved;
    std::uint32_t payload_offset;
    std::uint32_t frame_bytes;
};
#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 40, "Protocol header size changed");

class UdpSender {
public:
    UdpSender(const std::string& host, std::uint16_t port) {
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

    UdpSender(const UdpSender&) = delete;
    UdpSender& operator=(const UdpSender&) = delete;

    void sendDensity(const std::vector<std::uint8_t>& density,
                     int grid_size,
                     std::uint32_t frame_id,
                     float simulation_time) {
        const std::size_t chunk_count_size =
            (density.size() + kPayloadBytes - 1) / kPayloadBytes;
        if (chunk_count_size > UINT16_MAX) {
            throw std::runtime_error("Frame is too large for protocol v1");
        }
        const auto chunk_count = static_cast<std::uint16_t>(chunk_count_size);
        std::vector<std::uint8_t> packet(sizeof(PacketHeader) + kPayloadBytes);

        for (std::uint16_t chunk = 0; chunk < chunk_count; ++chunk) {
            const std::size_t offset = static_cast<std::size_t>(chunk) * kPayloadBytes;
            const std::size_t remaining = density.size() - offset;
            const std::size_t payload_size = remaining < kPayloadBytes ? remaining : kPayloadBytes;

            PacketHeader header{};
            std::memcpy(header.magic, "CLD1", 4);
            header.version = kProtocolVersion;
            header.header_bytes = static_cast<std::uint16_t>(sizeof(PacketHeader));
            header.frame_id = frame_id;
            header.simulation_time = simulation_time;
            header.grid_x = static_cast<std::uint16_t>(grid_size);
            header.grid_y = static_cast<std::uint16_t>(grid_size);
            header.grid_z = static_cast<std::uint16_t>(grid_size);
            header.voxel_format = static_cast<std::uint8_t>(VoxelFormat::UInt8);
            header.field_id = static_cast<std::uint8_t>(FieldId::Density);
            header.chunk_index = chunk;
            header.chunk_count = chunk_count;
            header.payload_bytes = static_cast<std::uint16_t>(payload_size);
            header.payload_offset = static_cast<std::uint32_t>(offset);
            header.frame_bytes = static_cast<std::uint32_t>(density.size());

            std::memcpy(packet.data(), &header, sizeof(header));
            std::memcpy(packet.data() + sizeof(header), density.data() + offset, payload_size);

            const auto sent = ::sendto(
                socketHandle(),
                reinterpret_cast<const char*>(packet.data()),
                static_cast<int>(sizeof(header) + payload_size),
                0,
                reinterpret_cast<const sockaddr*>(&address_),
                static_cast<socklen_type>(sizeof(address_)));
            if (sent < 0) {
                throw std::runtime_error("UDP send failed");
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
};

} // namespace cloud
