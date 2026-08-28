// mkv_test — the Matroska reader, over a Matroska file this test builds itself.
//
// Debug-only, plain assert(), no framework. It links src/mkv.cpp + src/ebml.cpp
// + src/demux.cpp and NOTHING else — no player, no clock, no OS. A container
// regression must never need a phone, or a two-gigabyte remux, to reproduce.
//
// The fixture is synthesized here rather than checked in, so what is asserted
// and what is written sit on the same screen: when a Colour assertion fails you
// can see the exact bytes that were supposed to produce it.
//
//   ./build/linux_debug/core/mkv_test

#undef NDEBUG
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core/demux.h"
#include "core/mkv.h"

using namespace vp;

namespace {

using Bytes = std::vector<uint8_t>;

void put(Bytes& b, const Bytes& o) { b.insert(b.end(), o.begin(), o.end()); }

// An element ID, written as the bytes it already is (IDs keep their marker).
Bytes id(uint64_t v) {
    Bytes out;
    for (int shift = 56; shift >= 0; shift -= 8) {
        uint8_t byte = static_cast<uint8_t>(v >> shift);
        if (out.empty() && byte == 0) continue;
        out.push_back(byte);
    }
    if (out.empty()) out.push_back(0);
    return out;
}

// Always the 8-byte size encoding (0x01 + 7 payload bytes). Wasteful and
// completely legal — the point of this fixture is the parser's behaviour, not
// the muxer's compactness.
Bytes size(uint64_t n) {
    Bytes out{0x01};
    for (int shift = 48; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(n >> shift));
    return out;
}

Bytes elem(uint64_t elemId, const Bytes& payload) {
    Bytes out = id(elemId);
    put(out, size(payload.size()));
    put(out, payload);
    return out;
}

Bytes uintPayload(uint64_t v) {
    Bytes out;
    for (int shift = 56; shift >= 0; shift -= 8) {
        uint8_t byte = static_cast<uint8_t>(v >> shift);
        if (out.empty() && byte == 0) continue;
        out.push_back(byte);
    }
    if (out.empty()) out.push_back(0);
    return out;
}

Bytes doublePayload(double d) {
    uint64_t bits;
    std::memcpy(&bits, &d, 8);
    Bytes out;
    for (int shift = 56; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(bits >> shift));
    return out;
}

// A fixed 8-byte unsigned payload. Matroska allows any width, and uintPayload
// above picks the shortest — which is right for a muxer and wrong for
// CueClusterPosition here, where the ENCODED LENGTH feeds back into the value
// being encoded (see buildCues). Fixed width breaks that circularity.
Bytes uint64Payload(uint64_t v) {
    Bytes out;
    for (int shift = 56; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(v >> shift));
    return out;
}

Bytes strPayload(const std::string& s) { return Bytes(s.begin(), s.end()); }

Bytes uintElem(uint64_t elemId, uint64_t v) { return elem(elemId, uintPayload(v)); }
Bytes dblElem(uint64_t elemId, double v)    { return elem(elemId, doublePayload(v)); }
Bytes strElem(uint64_t elemId, const std::string& v) { return elem(elemId, strPayload(v)); }
Bytes uint64Elem(uint64_t elemId, uint64_t v) { return elem(elemId, uint64Payload(v)); }

// IDs, in their own namespace because several of them share a name with the
// TYPES they describe (TrackEntry, Cluster, Colour...). Restated here rather
// than including src/ebml.h: this test asserts what
// the parser reads out of a FILE, and sharing the constant table with the
// parser would let a wrong ID agree with itself.
namespace ids {
constexpr uint64_t EBMLHeader = 0x1A45DFA3, Segment = 0x18538067;
constexpr uint64_t Info = 0x1549A966, TimecodeScale = 0x2AD7B1, Duration = 0x4489;
constexpr uint64_t Tracks = 0x1654AE6B, TrackEntry = 0xAE, TrackNumber = 0xD7;
constexpr uint64_t TrackType = 0x83, CodecID = 0x86, Language = 0x22B59C;
constexpr uint64_t Video = 0xE0, PixelWidth = 0xB0, PixelHeight = 0xBA;
constexpr uint64_t Audio = 0xE1, SamplingFreq = 0xB5, Channels = 0x9F;
constexpr uint64_t Colour = 0x55B0, MatrixCoeffs = 0x55B1, BitsPerChannel = 0x55B2;
constexpr uint64_t TransferChar = 0x55BA, Primaries = 0x55BB, MaxCLL = 0x55BC;
constexpr uint64_t MasteringMeta = 0x55D0, LuminanceMax = 0x55D9;
constexpr uint64_t Cluster = 0x1F43B675, Timecode = 0xE7, SimpleBlock = 0xA3;
constexpr uint64_t Cues = 0x1C53BB6B, CuePoint = 0xBB, CueTime = 0xB3;
constexpr uint64_t CueTrackPosition = 0xB7, CueTrack = 0xF7, CueClusterPos = 0xF1;
}  // namespace ids

// A SimpleBlock body: track number (one-byte size varint), int16 relative
// timecode, flags, then the payload.
Bytes simpleBlock(uint8_t track, int16_t relTime, bool keyframe, const Bytes& payload) {
    using namespace ids;
    Bytes body{static_cast<uint8_t>(0x80 | track)};
    body.push_back(static_cast<uint8_t>(relTime >> 8));
    body.push_back(static_cast<uint8_t>(relTime & 0xFF));
    body.push_back(keyframe ? 0x80 : 0x00);
    put(body, payload);
    return elem(SimpleBlock, body);
}

std::string writeFixture() {
    using namespace ids;
    // Track 1: HEVC Main10, HDR10 — PQ + BT.2020, 10-bit, with mastering data.
    Bytes colour;
    put(colour, uintElem(MatrixCoeffs, 9));    // BT.2020 non-constant luminance
    put(colour, uintElem(BitsPerChannel, 10));
    put(colour, uintElem(TransferChar, 16));   // ST 2084 PQ
    put(colour, uintElem(Primaries, 9));       // BT.2020
    put(colour, uintElem(MaxCLL, 1000));
    put(colour, elem(MasteringMeta, dblElem(LuminanceMax, 1000.0)));

    Bytes video;
    put(video, uintElem(PixelWidth, 3840));
    put(video, uintElem(PixelHeight, 2160));
    put(video, elem(Colour, colour));

    Bytes videoTrack;
    put(videoTrack, uintElem(TrackNumber, 1));
    put(videoTrack, uintElem(TrackType, 1));   // video
    put(videoTrack, strElem(CodecID, "V_MPEGH/ISO/HEVC"));
    put(videoTrack, strElem(Language, std::string("und\0", 4)));  // NUL-padded
    put(videoTrack, elem(Video, video));

    Bytes audio;
    put(audio, dblElem(SamplingFreq, 48000.0));
    put(audio, uintElem(Channels, 2));

    Bytes audioTrack;
    put(audioTrack, uintElem(TrackNumber, 2));
    put(audioTrack, uintElem(TrackType, 2));   // audio
    put(audioTrack, strElem(CodecID, "A_FLAC"));
    put(audioTrack, elem(Audio, audio));

    // Track 3: something we do not decode. Must be listed and must not be
    // chosen — a file with an AV1 track alongside HEVC still plays the HEVC.
    Bytes subTrack;
    put(subTrack, uintElem(TrackNumber, 3));
    put(subTrack, uintElem(TrackType, 17));    // subtitle
    put(subTrack, strElem(CodecID, "S_TEXT/UTF8"));

    Bytes tracks;
    put(tracks, elem(TrackEntry, videoTrack));
    put(tracks, elem(TrackEntry, audioTrack));
    put(tracks, elem(TrackEntry, subTrack));

    Bytes info;
    put(info, uintElem(TimecodeScale, 1000000));   // 1 ms per tick
    put(info, dblElem(Duration, 2000.0));          // 2000 ticks == 2 s

    // Two Clusters, so seeking has somewhere to land.
    Bytes cluster0;
    // A CRC-32 element AHEAD of the Timecode, which is what a real recorder
    // writes and what this parser used to trip over: it read exactly one child
    // and gave up if that child was not the Timecode, so every cluster was
    // timed from zero and a 42-second file never reported a timestamp past
    // one second.
    put(cluster0, elem(0xBF, Bytes{0xDE, 0xAD, 0xBE, 0xEF}));
    put(cluster0, uintElem(Timecode, 0));
    put(cluster0, simpleBlock(1, 0, true,  Bytes{0xAA, 0xBB, 0xCC}));
    put(cluster0, simpleBlock(2, 0, true,  Bytes{0x11, 0x22}));
    put(cluster0, simpleBlock(1, 42, false, Bytes{0xDD}));

    Bytes cluster1;
    put(cluster1, elem(0xBF, Bytes{0xDE, 0xAD, 0xBE, 0xEF}));
    put(cluster1, uintElem(Timecode, 1000));       // 1000 ms
    put(cluster1, simpleBlock(1, 0, true, Bytes{0xEE, 0xFF}));

    // Cluster offsets are relative to the Segment's payload, and the Cues
    // element sits between Tracks and the first Cluster — so its own encoded
    // length is part of every offset it contains. Build it twice: once with
    // placeholder offsets to learn that length, then again for real. Every
    // field in this writer is fixed-width, so the two lengths are equal; the
    // assert below is what makes that a checked fact rather than a hope.
    auto buildCues = [&](uint64_t off0, uint64_t off1) {
        auto cuePoint = [&](uint64_t timeTicks, uint64_t clusterOff) {
            Bytes pos;
            put(pos, uintElem(CueTrack, 1));
            put(pos, uint64Elem(CueClusterPos, clusterOff));
            Bytes cp;
            put(cp, uintElem(CueTime, timeTicks));
            put(cp, elem(CueTrackPosition, pos));
            return elem(CuePoint, cp);
        };
        Bytes cues;
        put(cues, cuePoint(0, off0));
        put(cues, cuePoint(1000, off1));
        return elem(Cues, cues);
    };

    Bytes headBeforeCues;
    put(headBeforeCues, elem(Info, info));
    put(headBeforeCues, elem(Tracks, tracks));

    const Bytes probe = buildCues(0, 0);
    const uint64_t cluster0Off = headBeforeCues.size() + probe.size();
    const uint64_t cluster1Off = cluster0Off + elem(Cluster, cluster0).size();
    const Bytes cuesElem = buildCues(cluster0Off, cluster1Off);
    assert(cuesElem.size() == probe.size());

    Bytes body;
    put(body, headBeforeCues);
    put(body, cuesElem);
    put(body, elem(Cluster, cluster0));
    put(body, elem(Cluster, cluster1));

    Bytes file;
    put(file, elem(EBMLHeader, strPayload("\x42\x82")));  // contents unread
    put(file, elem(Segment, body));

    const std::string path = "mkv_test_fixture.mkv";
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(file.data()),
              static_cast<std::streamsize>(file.size()));
    out.close();
    return path;
}

}  // namespace

int main() {
    const std::string path = writeFixture();

    Demuxer d;
    assert(d.open(path) && d.error().empty());

    // ── Segment info ──────────────────────────────────────────────────────
    assert(d.info().timecodeScaleNs == 1000000);
    assert(d.info().durationUs() == 2000000);   // 2000 ticks x 1 ms

    // ── Tracks ────────────────────────────────────────────────────────────
    assert(d.tracks().size() == 3);

    const TrackEntry* v = d.videoTrack();
    assert(v && v->number == 1);
    assert(v->codec == Codec::HEVC);
    assert(v->width == 3840 && v->height == 2160);
    // NUL padding stripped — "und\0" must not survive as a four-byte string.
    assert(v->language == "und");

    const TrackEntry* a = d.audioTrack();
    assert(a && a->number == 2);
    assert(a->codec == Codec::FLAC);
    assert(a->sampleRate == 48000.0 && a->channels == 2);

    // The subtitle track is listed but never chosen: pickTrack() skips every
    // codec this player does not decode.
    assert(d.tracks()[2].codec == Codec::Unknown);

    // ── Colour — the whole reason this parser exists ──────────────────────
    const ColourInfo& c = v->colour;
    assert(c.transfer == Transfer::PQ);
    assert(c.primaries == Primaries::BT2020);
    assert(c.matrix == MatrixCoeffs::BT2020NCL);
    assert(c.bitDepth == 10);
    assert(c.maxCLL == 1000);
    assert(c.mastering.present && c.mastering.maxLuminance == 1000.0);
    assert(c.isHdr10());

    // isHdr10() takes BOTH conditions. PQ under BT.709 primaries is not HDR10,
    // and a wide-gamut SDR file is not either.
    {
        ColourInfo sdrWideGamut = c;
        sdrWideGamut.transfer = Transfer::BT709;
        assert(!sdrWideGamut.isHdr10());
        ColourInfo pq709 = c;
        pq709.primaries = Primaries::BT709;
        assert(!pq709.isHdr10());
    }

    // ── Packets ───────────────────────────────────────────────────────────
    Packet p;
    assert(d.nextPacket(1, p));
    assert(p.trackNumber == 1 && p.ptsUs == 0 && p.keyframe);
    assert(p.bytes.size() == 3 && p.bytes[0] == 0xAA);

    // The audio Block sat between the two video Blocks in the file. Pulling
    // video first must not have dropped it — it was queued.
    Packet ap;
    assert(d.nextPacket(2, ap));
    assert(ap.trackNumber == 2 && ap.bytes.size() == 2 && ap.bytes[0] == 0x11);

    assert(d.nextPacket(1, p));
    assert(p.ptsUs == 42000 && !p.keyframe);   // 42 ms into cluster 0

    // Crossing into the second Cluster: the timestamp is the CLUSTER's
    // timecode plus the block's relative one, not the relative one alone.
    assert(d.nextPacket(1, p));
    assert(p.ptsUs == 1000000);
    assert(p.bytes.size() == 2 && p.bytes[0] == 0xEE);

    assert(!d.nextPacket(1, p));   // end of stream

    // ── Seeking ───────────────────────────────────────────────────────────
    assert(d.seekable());
    // Asking for 1.5 s lands on the 1.0 s keyframe: never past what was asked
    // for, because there is no keyframe behind that point to decode from.
    assert(d.seek(1500000) == 1000000);
    assert(d.nextPacket(1, p));
    assert(p.ptsUs == 1000000 && p.keyframe);

    assert(d.seek(0) == 0);
    assert(d.nextPacket(1, p));
    assert(p.ptsUs == 0);

    // ── A Segment whose size was never patched ────────────────────────────
    //
    // Found in the wild on the first real file this player was pointed at: a
    // recording interrupted before finalisation, whose Segment declares a size
    // of ZERO while 908 MB of Clusters follow it. Read literally that is an
    // empty Segment, and the parser reported "no Tracks" about a file whose
    // Info element began four bytes later.
    {
        const std::string zeroPath = "mkv_test_unfinalised.mkv";
        {
            std::ifstream in(path, std::ios::binary);
            std::string all((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
            // The Segment header sits right after the EBML header. Its size is
            // the 8-byte field written by size(); zero it in place.
            const size_t segIdAt = all.find(std::string("\x18\x53\x80\x67", 4));
            assert(segIdAt != std::string::npos);
            for (size_t k = segIdAt + 4; k < segIdAt + 12; ++k) all[k] = 0;
            all[segIdAt + 4] = 0x01;   // keep it a valid 8-byte size varint
            std::ofstream out(zeroPath, std::ios::binary);
            out.write(all.data(), static_cast<std::streamsize>(all.size()));
        }
        Demuxer z;
        assert(z.open(zeroPath));                 // ... and not "no Tracks element"
        assert(z.videoTrack() != nullptr);
        assert(z.videoTrack()->colour.isHdr10());
        Packet zp;
        assert(z.nextPacket(1, zp) && zp.ptsUs == 0);
        std::remove(zeroPath.c_str());
    }

    // ── Not Matroska ──────────────────────────────────────────────────────
    {
        const std::string junk = "mkv_test_junk.bin";
        std::ofstream out(junk, std::ios::binary);
        out << "this is not a matroska file at all";
        out.close();
        Demuxer bad;
        assert(!bad.open(junk));
        assert(!bad.error().empty());
        std::remove(junk.c_str());
    }

    std::remove(path.c_str());
    std::printf("mkv_test: all assertions passed\n");
    return 0;
}
