#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

namespace car {

// Small dependency-free SHA-256 file binding (FIPS 180-4). No platform crypto ABI.
class Sha256 {
  public:
    void Update(const unsigned char *data, size_t length) {
        m_bytes += length;
        while (length) {
            const size_t count = std::min(length, m_block.size() - m_used);
            std::copy(data, data + count, m_block.begin() + m_used);
            m_used += count;
            data += count;
            length -= count;
            if (m_used == m_block.size()) { Transform(); m_used = 0; }
        }
    }

    std::string Finish() {
        const uint64_t bits = m_bytes * 8;
        const unsigned char mark = 0x80, zero = 0;
        Update(&mark, 1);
        while (m_used != 56) Update(&zero, 1);
        std::array<unsigned char, 8> size{};
        for (unsigned i = 0; i < 8; ++i) size[7 - i] = static_cast<unsigned char>(bits >> (8 * i));
        Update(size.data(), size.size());
        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for (uint32_t word : m_state) output << std::setw(8) << word;
        return output.str();
    }

    static std::string File(const std::string &path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("cannot hash file: " + path);
        Sha256 hash;
        std::array<unsigned char, 65536> bytes{};
        while (input) {
            input.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
            hash.Update(bytes.data(), static_cast<size_t>(input.gcount()));
        }
        if (!input.eof()) throw std::runtime_error("file hash read failed: " + path);
        return hash.Finish();
    }

  private:
    static uint32_t Rotate(uint32_t value, unsigned count) {
        return (value >> count) | (value << (32 - count));
    }
    void Transform() {
        static constexpr uint32_t constants[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        uint32_t schedule[64];
        for (unsigned i = 0; i < 16; ++i)
            schedule[i] = (uint32_t(m_block[4*i]) << 24) | (uint32_t(m_block[4*i+1]) << 16) |
                          (uint32_t(m_block[4*i+2]) << 8) | uint32_t(m_block[4*i+3]);
        for (unsigned i = 16; i < 64; ++i) {
            const uint32_t a = schedule[i-15], b = schedule[i-2];
            schedule[i] = schedule[i-16] + (Rotate(a,7)^Rotate(a,18)^(a>>3)) + schedule[i-7] +
                          (Rotate(b,17)^Rotate(b,19)^(b>>10));
        }
        uint32_t a=m_state[0], b=m_state[1], c=m_state[2], d=m_state[3];
        uint32_t e=m_state[4], f=m_state[5], g=m_state[6], h=m_state[7];
        for (unsigned i=0; i<64; ++i) {
            const uint32_t t1=h+(Rotate(e,6)^Rotate(e,11)^Rotate(e,25))+((e&f)^(~e&g))+constants[i]+schedule[i];
            const uint32_t t2=(Rotate(a,2)^Rotate(a,13)^Rotate(a,22))+((a&b)^(a&c)^(b&c));
            h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        m_state[0]+=a; m_state[1]+=b; m_state[2]+=c; m_state[3]+=d;
        m_state[4]+=e; m_state[5]+=f; m_state[6]+=g; m_state[7]+=h;
    }
    std::array<uint32_t, 8> m_state{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                  0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    std::array<unsigned char, 64> m_block{};
    size_t m_used{0};
    uint64_t m_bytes{0};
};
} // namespace car
