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
constexpr uint64_t Projection = 0x7670, ProjectionRoll = 0x7675;
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

// A minimal but complete file whose single Cluster carries one SimpleBlock per
// payload, on track 1, one millisecond apart.
std::string writePayloadFixture(const std::vector<Bytes>& payloads) {
    using namespace ids;
    Bytes info;
    put(info, uintElem(TimecodeScale, 1000000));

    Bytes video;
    put(video, uintElem(PixelWidth, 16));
    put(video, uintElem(PixelHeight, 16));
    Bytes track;
    put(track, uintElem(TrackNumber, 1));
    put(track, uintElem(TrackType, 1));
    put(track, strElem(CodecID, "V_MPEGH/ISO/HEVC"));
    put(track, elem(Video, video));
    Bytes tracks;
    put(tracks, elem(TrackEntry, track));

    Bytes cluster;
    put(cluster, uintElem(Timecode, 0));
    for (size_t i = 0; i < payloads.size(); ++i)
        put(cluster, simpleBlock(1, static_cast<int16_t>(i), true, payloads[i]));

    Bytes segment;
    put(segment, elem(Info, info));
    put(segment, elem(Tracks, tracks));
    put(segment, elem(Cluster, cluster));

    Bytes file;
    put(file, elem(EBMLHeader, Bytes{0x42, 0x82, 0x88, 'm', 'a', 't', 'r', 'o', 's', 'k', 'a'}));
    put(file, elem(Segment, segment));

    const std::string path = "mkv_test_payloads.mkv";
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(file.data()),
              static_cast<std::streamsize>(file.size()));
    out.close();
    return path;
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

// A file that differs from the one above in ONE respect: the video track's
// Projection. `roll` is Matroska's counter-clockwise angle in degrees;
// `haveProjection` false omits the element entirely, which is what almost
// every file does and which must mean "upright", not "unknown".
//
// Deliberately minimal — Info, Tracks, and the one Cluster that open() insists
// on (a file with no Cluster has nothing to play and is refused there). A
// rotation is read from the header, so a richer fixture would only be testing
// the cluster parser again.
std::string writeRotationFixture(const std::string& path, bool haveProjection,
                                 double roll) {
    using namespace ids;
    Bytes video;
    put(video, uintElem(PixelWidth, 1920));
    put(video, uintElem(PixelHeight, 1080));
    if (haveProjection)
        put(video, elem(Projection, dblElem(ProjectionRoll, roll)));

    Bytes videoTrack;
    put(videoTrack, uintElem(TrackNumber, 1));
    put(videoTrack, uintElem(TrackType, 1));
    put(videoTrack, strElem(CodecID, "V_MPEGH/ISO/HEVC"));
    put(videoTrack, elem(Video, video));

    Bytes body;
    put(body, elem(Info, uintElem(TimecodeScale, 1000000)));
    put(body, elem(Tracks, elem(TrackEntry, videoTrack)));
    Bytes cluster;
    put(cluster, uintElem(Timecode, 0));
    put(cluster, simpleBlock(1, 0, true, Bytes{0x00}));
    put(body, elem(Cluster, cluster));

    Bytes file;
    put(file, elem(EBMLHeader, strPayload("\x42\x82")));
    put(file, elem(Segment, body));

    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(file.data()),
              static_cast<std::streamsize>(file.size()));
    out.close();   // the reader below opens this path; flush before it does
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

    // ── Rotation ──────────────────────────────────────────────────────────
    //
    // The renderer's external-image path turned every frame a quarter turn
    // unconditionally, because it was written for a camera preview that needs
    // one. A video file needs whatever its container says, and the common
    // answer is nothing at all — so the ABSENT case is the one that matters
    // most here.
    assert(v->rotationDegrees == 0);   // the main fixture has no Projection
    {
        struct Case { bool present; double roll; int expect; };
        // Matroska's roll is counter-clockwise; a player needs the clockwise
        // turn that undoes it. -90 CCW is a picture that must be turned 90
        // clockwise to come upright, which is what a phone held upright while
        // recording writes.
        const Case cases[] = {
            {false,    0.0,   0},
            {true,     0.0,   0},
            {true,   -90.0,  90},
            {true,   180.0, 180},
            {true,   -270.0, 270},
            // Not a right angle, and not exactly one either: a sampler can
            // only swizzle uv, so anything else snaps to the nearest quadrant
            // rather than silently resampling the picture.
            {true,   -89.5,  90},
            // Beyond one turn. 450 CCW is 90 CCW, so 270 clockwise.
            {true,   450.0, 270},
        };
        for (const Case& c2 : cases) {
            const std::string rp = "mkv_test_rotation.mkv";
            writeRotationFixture(rp, c2.present, c2.roll);
            Demuxer rd;
            assert(rd.open(rp) && rd.error().empty());
            const TrackEntry* rv = rd.videoTrack();
            assert(rv);
            assert(rv->rotationDegrees == c2.expect);
        }
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

    // ── Pixel aspect ──────────────────────────────────────────────────────
    //
    // Pure arithmetic over four numbers the container already carried and
    // nothing read, so it is asserted here rather than looked at on a phone.
    {
        TrackEntry t;
        t.width = 1920; t.height = 1080;

        // Nothing stated: square, which is what "the file did not say" means.
        assert(t.pixelAspect() == 1.0);

        // Stated and equal: still square.
        t.displayWidth = 1920; t.displayHeight = 1080;
        assert(t.pixelAspect() == 1.0);

        // Anamorphic DVD-style: 720x480 stored, shown 16:9. The pixels are
        // wider than they are tall, so the ratio is above 1.
        TrackEntry a;
        a.width = 720; a.height = 480;
        a.displayWidth = 16; a.displayHeight = 9;   // DisplayUnit 3, a ratio
        const double par = a.pixelAspect();
        assert(par > 1.18 && par < 1.19);           // (16*480)/(9*720)

        // The same shape stated in pixels rather than as a ratio must give the
        // same answer — which is why DisplayUnit never has to be read.
        TrackEntry b = a;
        b.displayWidth = 854; b.displayHeight = 480;
        assert(b.pixelAspect() > 1.18 && b.pixelAspect() < 1.19);

        // Nonsense is refused, not clamped: a corrupt header must not stretch
        // the picture by a factor of ten.
        TrackEntry c;
        c.width = 100; c.height = 100;
        c.displayWidth = 10000; c.displayHeight = 1;
        assert(c.pixelAspect() == 1.0);
        c.displayWidth = 1; c.displayHeight = 10000;
        assert(c.pixelAspect() == 1.0);

        // A zero anywhere is "not stated".
        TrackEntry d;
        d.width = 0; d.height = 1080;
        d.displayWidth = 1920; d.displayHeight = 1080;
        assert(d.pixelAspect() == 1.0);
    }

    // ── Buffer reuse must not leak one packet into the next ───────────────
    //
    // Demuxer recycles packet buffers rather than allocating one per frame
    // (~1.5 MB thirty times a second, past the allocator's mmap threshold).
    // The hazard that introduces is a shorter packet landing in a buffer that
    // still holds a longer one's bytes, so what is asserted here is not the
    // reuse — it is that reuse is invisible.
    //
    // Descending sizes on purpose: each payload is shorter than the buffer it
    // is handed, which is the only order in which stale tail bytes can survive.
    {
        const std::string reusePath = writePayloadFixture({
            Bytes(64, 0x11), Bytes(32, 0x22), Bytes(8, 0x33), Bytes(48, 0x44),
        });
        Demuxer r;
        assert(r.open(reusePath));

        const size_t expect[] = {64, 32, 8, 48};
        const uint8_t fill[]  = {0x11, 0x22, 0x33, 0x44};

        Packet q;   // ONE packet, reused — which is what the feed thread does
        for (int i = 0; i < 4; ++i) {
            assert(r.nextPacket(q));
            assert(q.bytes.size() == expect[i]);
            for (uint8_t b : q.bytes) assert(b == fill[i]);
        }
        assert(!r.nextPacket(q));

        // reset() keeps the allocation and still reads as empty, which is what
        // the feed thread relies on to know it needs another packet.
        q.bytes.assign(100, 0x55);
        const size_t heldCapacity = q.bytes.capacity();
        q.reset();
        assert(q.empty());
        assert(q.bytes.capacity() == heldCapacity);
        assert(q.trackNumber == 0 && q.ptsUs == 0 && !q.keyframe);
        std::remove(reusePath.c_str());
    }

    // ── A corrupt length must not become an allocation ────────────────────
    //
    // Every size the reader acts on comes out of the file, and std::vector
    // will faithfully attempt whatever it is told. One damaged byte in a
    // Block's size field is a request for gigabytes — an out-of-memory kill
    // where the honest answer is "this file is broken".
    //
    // The fixture below is byte-for-byte a working file with one number
    // changed: a SimpleBlock whose declared size runs far past the end of it.
    {
        const std::string good = writePayloadFixture({Bytes(64, 0x77)});
        std::vector<uint8_t> raw;
        {
            std::ifstream in(good, std::ios::binary);
            raw.assign(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
        }
        std::remove(good.c_str());

        // elem() always writes the 8-byte size encoding, so a SimpleBlock is
        // [A3][01][7 size bytes][track][rel hi][rel lo][flags][payload] — the
        // payload starts 13 bytes in, and the size field can be overwritten
        // where it stands.
        size_t payloadAt = 0;
        for (size_t i = 0; i + 64 <= raw.size(); ++i) {
            bool run = true;
            for (size_t k = 0; k < 64 && run; ++k) run = raw[i + k] == 0x77;
            if (run) { payloadAt = i; break; }
        }
        assert(payloadAt >= 13 && "SimpleBlock payload not found in the fixture");
        const size_t at = payloadAt - 13;
        assert(raw[at] == 0xA3 && raw[at + 1] == 0x01);

        // A body of 281 TB, declared inside a file of a couple of hundred
        // bytes. The seven payload bytes of the size varint follow the 0x01.
        //
        // That size on purpose: it is larger than the address space a process
        // can map, so an unguarded resize() cannot quietly succeed the way a
        // merely implausible one does. Without the bound in parseBlock this
        // test does not fail an assertion, it terminates on bad_alloc — which
        // is precisely the outcome the bound exists to prevent on somebody's
        // truncated download. Two leading zero bytes keep it clear of the
        // all-ones pattern that Matroska reserves for "size unknown".
        const uint8_t huge[7] = {0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        for (int k = 0; k < 7; ++k) raw[at + 2 + k] = huge[k];

        const std::string bad = "mkv_test_corrupt.mkv";
        {
            std::ofstream out(bad, std::ios::binary);
            out.write(reinterpret_cast<const char*>(raw.data()),
                      static_cast<std::streamsize>(raw.size()));
        }

        Demuxer c;
        // Opening still works: the damage is in a Cluster, and the headers
        // above it are intact. Reading refuses rather than allocating.
        assert(c.open(bad));
        Packet cp;
        assert(!c.nextPacket(cp));
        assert(cp.bytes.empty());
        std::remove(bad.c_str());
    }

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
