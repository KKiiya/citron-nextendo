// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <zlib.h>

#include "core/hle/service/sockets/nextendo_uch.h"

namespace {

using namespace Service::Sockets::NextendoUch;

constexpr u64 OtherTitle = 0x0100b6e012ebe000ULL;

std::string Base64Url(const std::array<u8, 32>& bytes) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    u32 buffer = 0;
    int bits = 0;
    for (const u8 byte : bytes) {
        buffer = (buffer << 8) | byte;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out.push_back(alphabet[(buffer >> bits) & 63]);
        }
    }
    if (bits > 0) {
        out.push_back(alphabet[(buffer << (6 - bits)) & 63]);
    }
    return out;
}

std::array<u8, 32> SampleTicket() {
    std::array<u8, 32> ticket{};
    for (std::size_t i = 0; i < ticket.size(); ++i) {
        ticket[i] = static_cast<u8>(i + 1);
    }
    return ticket;
}

s64 NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string LoginBody(const std::string& ticket, s64 expires_ms, const std::string& relay_ip = "203.0.113.10",
                      int relay_port = 18888) {
    return R"({"nextendoTransport":{"protocol":"NXU1","ticket":")" + ticket +
           R"(","expiresMs":)" + std::to_string(expires_ms) + R"(,"relayIP":")" + relay_ip +
           R"(","relayPort":)" + std::to_string(relay_port) + "}}";
}

void WriteBigEndian32(std::array<u8, 19>& packet, std::size_t offset, u32 value) {
    packet[offset] = static_cast<u8>(value >> 24);
    packet[offset + 1] = static_cast<u8>(value >> 16);
    packet[offset + 2] = static_cast<u8>(value >> 8);
    packet[offset + 3] = static_cast<u8>(value);
}

// The game's UNET connection request: type 1, then the version and the config checksum, big endian.
std::array<u8, 19> ConnectRequest() {
    std::array<u8, 19> packet{};
    packet[2] = 1;
    WriteBigEndian32(packet, 11, 16777984U);
    WriteBigEndian32(packet, 15, 2944057210U);
    return packet;
}

std::vector<u8> Bytes(const std::string& text) {
    return std::vector<u8>(text.begin(), text.end());
}

std::vector<u8> Gzip(const std::string& text) {
    z_stream stream{};
    deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY);
    std::vector<u8> out(deflateBound(&stream, static_cast<uLong>(text.size())) + 32);
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(text.data()));
    stream.avail_in = static_cast<uInt>(text.size());
    stream.next_out = out.data();
    stream.avail_out = static_cast<uInt>(out.size());
    deflate(&stream, Z_FINISH);
    out.resize(stream.total_out);
    deflateEnd(&stream);
    return out;
}

} // Anonymous namespace

TEST_CASE("Nextendo UCH: socket selector is UDP only for the title and that exact value",
          "[core][nextendo]") {
    REQUIRE(IsUdpSelector(TitleId, 0x10000000U));
    REQUIRE_FALSE(IsUdpSelector(TitleId, 0x10000001U));
    REQUIRE_FALSE(IsUdpSelector(TitleId, 2U));
    REQUIRE_FALSE(IsUdpSelector(OtherTitle, 0x10000000U));
}

TEST_CASE("Nextendo UCH: service hosts are exactly the four UCH names", "[core][nextendo]") {
    for (const char* host : {"api.braincloudservers.com", "na.ultimatechickenhorseserver.com",
                             "EU.ultimatechickenhorseserver.com",
                             "ap.ultimatechickenhorseserver.com"}) {
        REQUIRE(IsServiceHost(host));
    }
    for (const char* host : {"", "braincloudservers.com", "example.com",
                             "public-ubiservices.ubi.com", "x.api.braincloudservers.com"}) {
        REQUIRE_FALSE(IsServiceHost(host));
    }
}

TEST_CASE("Nextendo UCH: the ticket frame precedes the connect request, to the relay only",
          "[core][nextendo]") {
    ClearTicket();
    const auto ticket = SampleTicket();
    REQUIRE(ObserveLoginBody(LoginBody(Base64Url(ticket), NowMs() + 20000)));

    const std::array<u8, 4> relay{203, 0, 113, 10};
    const auto connect = ConnectRequest();
    std::array<u8, FrameSize> frame{};

    // Other titles and other packets are untouched.
    REQUIRE(Bootstrap(OtherTitle, connect, relay, 18888, frame) == BootstrapResult::NotApplicable);
    const std::array<u8, 19> not_a_connect{};
    REQUIRE(Bootstrap(TitleId, not_a_connect, relay, 18888, frame) == BootstrapResult::NotApplicable);

    // A connect request to another address or port is refused rather than leaking the ticket.
    REQUIRE(Bootstrap(TitleId, connect, std::array<u8, 4>{198, 51, 100, 7}, 18888, frame) ==
            BootstrapResult::Refused);
    REQUIRE(Bootstrap(TitleId, connect, relay, 18889, frame) == BootstrapResult::Refused);

    REQUIRE(Bootstrap(TitleId, connect, relay, 18888, frame) == BootstrapResult::SendFrame);
    REQUIRE(std::string(frame.begin(), frame.begin() + 4) == "NXU1");
    REQUIRE(std::equal(ticket.begin(), ticket.end(), frame.begin() + 4));

    ClearTicket();
    REQUIRE(Bootstrap(TitleId, connect, relay, 18888, frame) == BootstrapResult::Refused);
}

TEST_CASE("Nextendo UCH: invalid or expired tickets are not stored", "[core][nextendo]") {
    ClearTicket();
    const auto good = Base64Url(SampleTicket());
    REQUIRE_FALSE(ObserveLoginBody(LoginBody(good, NowMs() - 5000)));          // already expired
    REQUIRE_FALSE(ObserveLoginBody(LoginBody(good, NowMs() + 600000)));        // too far ahead
    REQUIRE_FALSE(ObserveLoginBody(LoginBody(good.substr(1), NowMs() + 20000))); // wrong length
    REQUIRE_FALSE(ObserveLoginBody(LoginBody(good, NowMs() + 20000, "0.0.0.0")));
    REQUIRE_FALSE(ObserveLoginBody(LoginBody(good, NowMs() + 20000, "not-an-ip")));
    REQUIRE_FALSE(ObserveLoginBody(LoginBody(good, NowMs() + 20000, "203.0.113.10", 0)));
    REQUIRE_FALSE(ObserveLoginBody(LoginBody(good, NowMs() + 20000, "203.0.113.10", 70000)));
    REQUIRE_FALSE(ObserveLoginBody("{}"));
    REQUIRE_FALSE(ObserveLoginBody("not json"));

    std::array<u8, FrameSize> frame{};
    REQUIRE(Bootstrap(TitleId, ConnectRequest(), std::array<u8, 4>{203, 0, 113, 10}, 18888, frame) ==
            BootstrapResult::Refused);
}

TEST_CASE("Nextendo UCH: response observer reads a chunked body split across reads",
          "[core][nextendo]") {
    ClearTicket();
    const std::string body = LoginBody(Base64Url(SampleTicket()), NowMs() + 20000);
    char size[16];
    std::snprintf(size, sizeof(size), "%zx", body.size());
    const auto response = Bytes("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" +
                                std::string(size) + "\r\n" + body + "\r\n0\r\n\r\n");

    ResponseObserver observer;
    observer.Feed({response.data(), 30});

    std::array<u8, FrameSize> frame{};
    REQUIRE(Bootstrap(TitleId, ConnectRequest(), std::array<u8, 4>{203, 0, 113, 10}, 18888, frame) ==
            BootstrapResult::Refused); // a partial response must not store anything

    observer.Feed({response.data() + 30, response.size() - 30});
    REQUIRE(Bootstrap(TitleId, ConnectRequest(), std::array<u8, 4>{203, 0, 113, 10}, 18888, frame) ==
            BootstrapResult::SendFrame);
    ClearTicket();
}

TEST_CASE("Nextendo UCH: response observer reads a Content-Length body and ignores garbage",
          "[core][nextendo]") {
    ClearTicket();
    const std::string body = LoginBody(Base64Url(SampleTicket()), NowMs() + 20000);

    ResponseObserver observer;
    observer.Feed(Bytes("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"));
    observer.Feed(Bytes("\x00\x01\x02\x03\xff"));

    std::array<u8, FrameSize> frame{};
    REQUIRE(Bootstrap(TitleId, ConnectRequest(), std::array<u8, 4>{203, 0, 113, 10}, 18888, frame) ==
            BootstrapResult::Refused);

    observer.Feed(Bytes("HTTP/1.1 200 OK\r\ncontent-length: " + std::to_string(body.size()) +
                        "\r\n\r\n" + body));
    REQUIRE(Bootstrap(TitleId, ConnectRequest(), std::array<u8, 4>{203, 0, 113, 10}, 18888, frame) ==
            BootstrapResult::SendFrame);
    ClearTicket();
}

TEST_CASE("Nextendo UCH: response observer reads a gzip-compressed body", "[core][nextendo]") {
    ClearTicket();
    const std::string body = LoginBody(Base64Url(SampleTicket()), NowMs() + 20000);
    const std::vector<u8> compressed = Gzip(body);
    std::vector<u8> response = Bytes("HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: " +
                                     std::to_string(compressed.size()) + "\r\n\r\n");
    response.insert(response.end(), compressed.begin(), compressed.end());

    ResponseObserver observer;
    observer.Feed(response);

    std::array<u8, FrameSize> frame{};
    REQUIRE(Bootstrap(TitleId, ConnectRequest(), std::array<u8, 4>{203, 0, 113, 10}, 18888, frame) ==
            BootstrapResult::SendFrame);

    // A corrupt compressed body is dropped without storing anything.
    ClearTicket();
    std::vector<u8> corrupt = Bytes("HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: 6\r\n\r\n");
    for (const u8 byte : {1, 2, 3, 4, 5, 6}) {
        corrupt.push_back(byte);
    }
    ResponseObserver second;
    second.Feed(corrupt);
    REQUIRE(Bootstrap(TitleId, ConnectRequest(), std::array<u8, 4>{203, 0, 113, 10}, 18888, frame) ==
            BootstrapResult::Refused);
}