#include "ebml.h"

#include <cstring>
#include <string>

namespace vp::ebml {
namespace {

// Both varint encodings share one leading byte: the number of leading zero
// bits before the first 1 says how many EXTRA bytes follow. 0x80 is one byte
// total, 0x40 two, 0x20 three, and so on to eight.
int lengthFromLeadingByte(uint8_t b) {
    for (int i = 0; i < 8; ++i)
        if (b & (0x80 >> i)) return i + 1;
    return 0;  // 0x00 — invalid here
}

}  // namespace

uint64_t readId(std::istream& in) {
    int first = in.get();
    if (first == std::istream::traits_type::eof()) return 0;
    const uint8_t b = static_cast<uint8_t>(first);
    const int len = lengthFromLeadingByte(b);
    if (len == 0) return 0;

    uint64_t id = b;  // marker bit RETAINED — the ID is the whole encoding
    for (int i = 1; i < len; ++i) {
        int c = in.get();
        if (c == std::istream::traits_type::eof()) return 0;
        id = (id << 8) | static_cast<uint8_t>(c);
    }
    return id;
}

uint64_t readSize(std::istream& in) {
    int first = in.get();
    if (first == std::istream::traits_type::eof()) return kUnknownSize;
    const uint8_t b = static_cast<uint8_t>(first);
    const int len = lengthFromLeadingByte(b);
    if (len == 0) return kUnknownSize;

    uint64_t value = b & (0xFFu >> len);  // marker bit STRIPPED
    uint64_t allOnes = (0xFFu >> len);
    for (int i = 1; i < len; ++i) {
        int c = in.get();
        if (c == std::istream::traits_type::eof()) return kUnknownSize;
        value = (value << 8) | static_cast<uint8_t>(c);
        allOnes = (allOnes << 8) | 0xFFu;
    }
    // An all-ones payload is Matroska's "I do not know yet" — live muxers
    // write it on the Segment and on Clusters. Not an error, and emphatically
    // not a real size of 2^56-1 to seek past.
    return value == allOnes ? kUnknownSize : value;
}

uint64_t readUInt(std::istream& in, uint64_t len) {
    uint64_t v = 0;
    for (uint64_t i = 0; i < len && i < 8; ++i) {
        int c = in.get();
        if (c == std::istream::traits_type::eof()) return v;
        v = (v << 8) | static_cast<uint8_t>(c);
    }
    return v;
}

int64_t readInt(std::istream& in, uint64_t len) {
    if (len == 0 || len > 8) return 0;
    uint64_t v = readUInt(in, len);
    const uint64_t signBit = 1ULL << (len * 8 - 1);
    if (v & signBit) v |= ~((signBit << 1) - 1);  // sign-extend to 64
    return static_cast<int64_t>(v);
}

double readFloat(std::istream& in, uint64_t len) {
    if (len == 4) {
        uint32_t bits = static_cast<uint32_t>(readUInt(in, 4));
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }
    if (len == 8) {
        uint64_t bits = readUInt(in, 8);
        double d;
        std::memcpy(&d, &bits, 8);
        return d;
    }
    in.seekg(static_cast<std::streamoff>(len), std::ios::cur);
    return 0.0;
}

std::string readString(std::istream& in, uint64_t len) {
    std::string s(static_cast<size_t>(len), '\0');
    if (len) in.read(&s[0], static_cast<std::streamsize>(len));
    // Matroska pads with NULs; "und\0" must compare equal to "und".
    const size_t end = s.find('\0');
    if (end != std::string::npos) s.resize(end);
    return s;
}

Element readElement(std::istream& in) {
    Element e;
    e.startPos = static_cast<uint64_t>(in.tellg());
    e.id = readId(in);
    if (e.id == 0 || !in) return e;
    e.size = readSize(in);
    e.dataPos = static_cast<uint64_t>(in.tellg());
    e.ok = static_cast<bool>(in);
    return e;
}

}  // namespace vp::ebml
