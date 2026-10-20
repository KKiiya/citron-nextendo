// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>
#include <zlib.h>

#include "core/hle/service/sockets/nextendo_uch.h"

namespace Service::Sockets::NextendoUch {

namespace {

constexpr std::size_t ResponseLimit = 131072;
constexpr s64 MaxTicketLifetimeMs = 35000;

// UNET connection request fingerprint of UCH 1.13.13.765: packet type 1, version and config checksum.
constexpr std::size_t ConnectRequestSize = 19;
constexpr u32 ConnectVersion = 16777984;
constexpr u32 ConnectConfigChecksum = 2944057210U;

constexpr std::array<std::string_view, 4> ServiceHosts{
    "api.braincloudservers.com",
    "na.ultimatechickenhorseserver.com",
    "eu.ultimatechickenhorseserver.com",
    "ap.ultimatechickenhorseserver.com",
};

struct Ticket {
    bool valid = false;
    std::array<u8, FrameSize> frame{};
    std::array<u8, 4> relay_ip{};
    u16 relay_port = 0;
    s64 expires_ms = 0;
};

std::mutex ticket_mutex;
Ticket stored_ticket;

s64 NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool EqualsNoCase(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
               return std::tolower(x) == std::tolower(y);
           });
}

bool StartsWithNoCase(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && EqualsNoCase(text.substr(0, prefix.size()), prefix);
}

bool ContainsNoCase(std::string_view text, std::string_view needle) {
    return std::search(text.begin(), text.end(), needle.begin(), needle.end(),
                       [](unsigned char x, unsigned char y) {
                           return std::tolower(x) == std::tolower(y);
                       }) != text.end();
}

std::string_view Trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

// Unpadded base64url of exactly 32 bytes (43 characters).
std::optional<std::array<u8, 32>> DecodeTicket(std::string_view text) {
    if (text.size() != 43) {
        return std::nullopt;
    }
    std::array<u8, 32> out{};
    std::size_t out_index = 0;
    u32 buffer = 0;
    int bits = 0;
    for (const char c : text) {
        u32 value;
        if (c >= 'A' && c <= 'Z') {
            value = static_cast<u32>(c - 'A');
        } else if (c >= 'a' && c <= 'z') {
            value = static_cast<u32>(c - 'a' + 26);
        } else if (c >= '0' && c <= '9') {
            value = static_cast<u32>(c - '0' + 52);
        } else if (c == '-') {
            value = 62;
        } else if (c == '_') {
            value = 63;
        } else {
            return std::nullopt;
        }
        buffer = (buffer << 6) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out_index >= out.size()) {
                return std::nullopt;
            }
            out[out_index++] = static_cast<u8>((buffer >> bits) & 0xFF);
        }
    }
    if (out_index != out.size()) {
        return std::nullopt;
    }
    return out;
}

std::optional<std::array<u8, 4>> ParseIPv4(std::string_view text) {
    std::array<u8, 4> ip{};
    for (std::size_t i = 0; i < ip.size(); ++i) {
        const std::size_t dot = text.find('.');
        const bool last = i == ip.size() - 1;
        if ((dot == std::string_view::npos) != last) {
            return std::nullopt;
        }
        const std::string_view part = last ? text : text.substr(0, dot);
        if (part.empty() || part.size() > 3) {
            return std::nullopt;
        }
        unsigned value = 0;
        const auto [ptr, ec] = std::from_chars(part.data(), part.data() + part.size(), value);
        if (ec != std::errc{} || ptr != part.data() + part.size() || value > 255) {
            return std::nullopt;
        }
        ip[i] = static_cast<u8>(value);
        if (!last) {
            text.remove_prefix(dot + 1);
        }
    }
    if (ip == std::array<u8, 4>{0, 0, 0, 0}) {
        return std::nullopt;
    }
    return ip;
}

u32 ReadBigEndian32(std::span<const u8> packet, std::size_t offset) {
    return (static_cast<u32>(packet[offset]) << 24) | (static_cast<u32>(packet[offset + 1]) << 16) |
           (static_cast<u32>(packet[offset + 2]) << 8) | static_cast<u32>(packet[offset + 3]);
}

std::optional<std::vector<u8>> Gunzip(const std::vector<u8>& body) {
    z_stream stream{};
    if (inflateInit2(&stream, 47) != Z_OK) { // 32 + 15: zlib or gzip header
        return std::nullopt;
    }
    std::vector<u8> out;
    std::array<u8, 4096> block{};
    stream.next_in = const_cast<Bytef*>(body.data());
    stream.avail_in = static_cast<uInt>(body.size());
    int result = Z_OK;
    while (result == Z_OK) {
        stream.next_out = block.data();
        stream.avail_out = static_cast<uInt>(block.size());
        result = inflate(&stream, Z_NO_FLUSH);
        const std::size_t produced = block.size() - stream.avail_out;
        if (out.size() + produced > ResponseLimit) {
            inflateEnd(&stream);
            return std::nullopt;
        }
        out.insert(out.end(), block.begin(), block.begin() + produced);
    }
    inflateEnd(&stream);
    if (result != Z_STREAM_END) {
        return std::nullopt;
    }
    return out;
}

} // Anonymous namespace

bool IsTitle(u64 program_id) {
    return program_id == TitleId;
}

bool IsServiceHost(std::string_view host) {
    return std::any_of(ServiceHosts.begin(), ServiceHosts.end(),
                       [host](std::string_view candidate) { return EqualsNoCase(host, candidate); });
}

bool IsUdpSelector(u64 program_id, u32 requested_type) {
    return IsTitle(program_id) && requested_type == 0x10000000U;
}

bool ObserveLoginBody(std::string_view json_body) {
    const nlohmann::json root = nlohmann::json::parse(json_body, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return false;
    }
    const auto extension = root.find("nextendoTransport");
    if (extension == root.end() || !extension->is_object()) {
        return false;
    }

    const auto protocol = extension->find("protocol");
    const auto ticket_field = extension->find("ticket");
    const auto expires_field = extension->find("expiresMs");
    const auto relay_ip_field = extension->find("relayIP");
    const auto relay_port_field = extension->find("relayPort");
    if (protocol == extension->end() || !protocol->is_string() ||
        protocol->get<std::string>() != "NXU1" || ticket_field == extension->end() ||
        !ticket_field->is_string() || expires_field == extension->end() ||
        !expires_field->is_number_integer() || relay_ip_field == extension->end() ||
        !relay_ip_field->is_string() || relay_port_field == extension->end() ||
        !relay_port_field->is_number_integer()) {
        return false;
    }

    const auto decoded = DecodeTicket(ticket_field->get<std::string>());
    const auto relay_ip = ParseIPv4(relay_ip_field->get<std::string>());
    const s64 expires_ms = expires_field->get<s64>();
    const s64 relay_port = relay_port_field->get<s64>();
    const s64 now = NowMs();
    if (!decoded || !relay_ip || expires_ms <= now || expires_ms > now + MaxTicketLifetimeMs ||
        relay_port < 1 || relay_port > 65535) {
        return false;
    }

    Ticket ticket;
    ticket.valid = true;
    ticket.frame = {'N', 'X', 'U', '1'};
    std::copy(decoded->begin(), decoded->end(), ticket.frame.begin() + 4);
    ticket.relay_ip = *relay_ip;
    ticket.relay_port = static_cast<u16>(relay_port);
    ticket.expires_ms = expires_ms;

    std::scoped_lock lock{ticket_mutex};
    stored_ticket.frame.fill(0); // wipe the previous ticket before it is replaced
    stored_ticket = ticket;
    return true;
}

BootstrapResult Bootstrap(u64 program_id, std::span<const u8> packet, const std::array<u8, 4>& ip,
                          u16 port, std::array<u8, FrameSize>& frame) {
    if (!IsTitle(program_id) || packet.size() != ConnectRequestSize || packet[0] != 0 ||
        packet[1] != 0 || packet[2] != 1 || ReadBigEndian32(packet, 11) != ConnectVersion ||
        ReadBigEndian32(packet, 15) != ConnectConfigChecksum) {
        return BootstrapResult::NotApplicable;
    }

    std::scoped_lock lock{ticket_mutex};
    if (!stored_ticket.valid || stored_ticket.expires_ms <= NowMs() ||
        stored_ticket.relay_port != port || stored_ticket.relay_ip != ip) {
        return BootstrapResult::Refused;
    }
    frame = stored_ticket.frame;
    return BootstrapResult::SendFrame;
}

void ClearTicket() {
    std::scoped_lock lock{ticket_mutex};
    stored_ticket.frame.fill(0);
    stored_ticket = Ticket{};
}

void ResponseObserver::Feed(std::span<const u8> bytes) {
    if (pending.size() + bytes.size() > ResponseLimit) {
        pending.clear();
        return;
    }
    pending.insert(pending.end(), bytes.begin(), bytes.end());

    while (!pending.empty()) {
        const std::string_view text(reinterpret_cast<const char*>(pending.data()), pending.size());
        const std::size_t split = text.find("\r\n\r\n");
        if (split == std::string_view::npos) {
            return;
        }

        // Status line and the few headers that matter for framing.
        const std::string_view head = text.substr(0, split);
        const std::size_t first_end = head.find("\r\n");
        const std::string_view status_line =
            first_end == std::string_view::npos ? head : head.substr(0, first_end);
        int status = 0;
        if (!StartsWithNoCase(status_line, "HTTP/") || status_line.size() < 12 ||
            std::from_chars(status_line.data() + 9, status_line.data() + 12, status).ec !=
                std::errc{}) {
            pending.clear(); // not an HTTP response: nothing to observe
            return;
        }

        s64 content_length = -1;
        bool chunked = false;
        bool gzip = false;
        std::string_view rest =
            first_end == std::string_view::npos ? std::string_view{} : head.substr(first_end + 2);
        while (!rest.empty()) {
            const std::size_t line_end = rest.find("\r\n");
            const std::string_view line = rest.substr(0, line_end);
            rest = line_end == std::string_view::npos ? std::string_view{} : rest.substr(line_end + 2);
            if (StartsWithNoCase(line, "Content-Length:")) {
                const std::string_view value = Trim(line.substr(15));
                s64 parsed = -1;
                const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
                content_length = (result.ec == std::errc{} && parsed >= 0) ? parsed : -1;
            } else if (StartsWithNoCase(line, "Transfer-Encoding:")) {
                chunked = ContainsNoCase(line, "chunked");
            } else if (StartsWithNoCase(line, "Content-Encoding:")) {
                gzip = ContainsNoCase(line, "gzip");
            }
        }

        const std::size_t body_start = split + 4;

        // Interim responses have no body and precede the final response.
        if ((status >= 100 && status < 200) || status == 204 || status == 304) {
            pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(body_start));
            continue;
        }

        std::vector<u8> body;
        std::size_t consumed = 0;
        if (chunked) {
            std::size_t pos = body_start;
            while (true) {
                const std::size_t end = text.find("\r\n", pos);
                if (end == std::string_view::npos) {
                    return; // wait for more data
                }
                std::string size_line(text.substr(pos, end - pos));
                size_line = size_line.substr(0, size_line.find(';'));
                char* parse_end = nullptr;
                const unsigned long count = std::strtoul(size_line.c_str(), &parse_end, 16);
                if (size_line.empty() || *parse_end != '\0' || count > ResponseLimit) {
                    pending.clear();
                    return;
                }
                pos = end + 2;
                if (count == 0) {
                    if (text.size() < pos + 2) {
                        return;
                    }
                    if (text.substr(pos, 2) == "\r\n") {
                        consumed = pos + 2;
                    } else {
                        const std::size_t trailer_end = text.find("\r\n\r\n", pos);
                        if (trailer_end == std::string_view::npos) {
                            return;
                        }
                        consumed = trailer_end + 4;
                    }
                    break;
                }
                if (text.size() - pos < count + 2 || body.size() + count > ResponseLimit) {
                    if (body.size() + count > ResponseLimit) {
                        pending.clear();
                    }
                    return;
                }
                body.insert(body.end(), pending.begin() + static_cast<std::ptrdiff_t>(pos),
                            pending.begin() + static_cast<std::ptrdiff_t>(pos + count));
                pos += count + 2;
            }
        } else {
            if (content_length < 0 || static_cast<std::size_t>(content_length) > ResponseLimit) {
                pending.clear();
                return;
            }
            const auto length = static_cast<std::size_t>(content_length);
            if (text.size() - body_start < length) {
                return;
            }
            body.assign(pending.begin() + static_cast<std::ptrdiff_t>(body_start),
                        pending.begin() + static_cast<std::ptrdiff_t>(body_start + length));
            consumed = body_start + length;
        }

        if (gzip) {
            auto inflated = Gunzip(body);
            if (!inflated) {
                pending.clear();
                return;
            }
            body = std::move(*inflated);
        }

        ObserveLoginBody(std::string_view(reinterpret_cast<const char*>(body.data()), body.size()));
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(consumed));
    }
}

} // namespace Service::Sockets::NextendoUch
