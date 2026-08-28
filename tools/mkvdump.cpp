// mkvdump — what does this file actually say?
//
// A desktop tool over the REAL core/ Demuxer: no phone, no GPU, no decoder. It
// answers the question that a black screen never does — did the container
// parse, what tracks are in it, what colour do they claim, is it seekable, and
// do the first packets have sane timestamps.
//
//   ./build/linux_debug/mkvdump some.mkv
//
// It exists because the first real file this player was pointed at failed with
// "no Tracks element", and finding out why through an APK, an install, a
// launch and a logcat is a minute per attempt. Here it is a second.

#include <cstdio>
#include <string>

#include "core/demux.h"

using namespace vp;

namespace {

const char* kindName(TrackKind k) {
    switch (k) {
        case TrackKind::Video:    return "video";
        case TrackKind::Audio:    return "audio";
        case TrackKind::Subtitle: return "subtitle";
        default:                  return "unknown";
    }
}

const char* transferName(Transfer t) {
    switch (t) {
        case Transfer::PQ:          return "PQ (ST 2084)";
        case Transfer::HLG:         return "HLG";
        case Transfer::BT709:       return "BT.709";
        case Transfer::Unspecified: return "unspecified";
    }
    return "?";
}

const char* primariesName(Primaries p) {
    switch (p) {
        case Primaries::BT2020:      return "BT.2020";
        case Primaries::BT709:       return "BT.709";
        case Primaries::Unspecified: return "unspecified";
    }
    return "?";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: mkvdump <file.mkv>\n");
        return 2;
    }

    Demuxer d;
    if (!d.open(argv[1])) {
        std::printf("FAILED to open: %s\n", d.error().c_str());
        return 1;
    }

    const SegmentInfo& i = d.info();
    std::printf("container\n");
    std::printf("  timecodeScale : %llu ns\n", (unsigned long long)i.timecodeScaleNs);
    std::printf("  duration      : %.3f s\n", i.durationUs() / 1e6);
    std::printf("  muxer         : %s / %s\n", i.muxingApp.c_str(), i.writingApp.c_str());
    std::printf("  seekable      : %s\n", d.seekable() ? "yes (has Cues)" : "NO (no Cues)");

    std::printf("\ntracks (%zu)\n", d.tracks().size());
    for (const TrackEntry& t : d.tracks()) {
        std::printf("  #%llu %-8s %-22s%s\n", (unsigned long long)t.number,
                    kindName(t.kind), t.codecId.c_str(),
                    t.codec == Codec::Unknown ? "  [NOT DECODABLE by this player]" : "");
        if (t.kind == TrackKind::Video) {
            std::printf("      %ux%u, CodecPrivate %zu bytes\n", t.width, t.height,
                        t.codecPrivate.size());
            const ColourInfo& c = t.colour;
            std::printf("      transfer=%s primaries=%s bitDepth=%d range=%d\n",
                        transferName(c.transfer), primariesName(c.primaries),
                        c.bitDepth, (int)c.range);
            std::printf("      mastering=%s maxCLL=%d maxFALL=%d\n",
                        c.mastering.present ? "present" : "absent", c.maxCLL, c.maxFALL);
            std::printf("      --> isHdr10() = %s\n", c.isHdr10() ? "YES" : "no");
        } else if (t.kind == TrackKind::Audio) {
            std::printf("      %.0f Hz x %u ch, %u bits, CodecPrivate %zu bytes\n",
                        t.sampleRate, t.channels, t.bitsPerSample, t.codecPrivate.size());
        }
    }

    const TrackEntry* v = d.videoTrack();
    const TrackEntry* a = d.audioTrack();
    std::printf("\nchosen: video=%s audio=%s\n",
                v ? ("#" + std::to_string(v->number)).c_str() : "none",
                a ? ("#" + std::to_string(a->number)).c_str() : "none");

    // The first few packets of each. Timestamps that jump, or a first packet
    // that is not a keyframe, are visible here and nowhere else.
    for (const TrackEntry* t : {v, a}) {
        if (!t) continue;
        std::printf("\nfirst packets of #%llu\n", (unsigned long long)t->number);
        Packet p;
        for (int n = 0; n < 5 && d.nextPacket(t->number, p); ++n)
            std::printf("  pts %10lld us  %-9s %zu bytes\n", (long long)p.ptsUs,
                        p.keyframe ? "keyframe" : "delta", p.bytes.size());
    }
    // ── Read to the end ───────────────────────────────────────────────────
    //
    // Where does the container actually stop? A player that dies a third of
    // the way through a file cannot tell you whether that is the parser, the
    // decoder or the clock; this can, in one pass, with no device involved.
    if (argc > 2 && std::string(argv[2]) == "--all") {
        Demuxer d2;
        if (!d2.open(argv[1])) return 1;
        size_t n = 0;
        int64_t lastPts = 0, maxPts = 0;
        uint64_t bytes = 0;
        Packet p2;
        while (d2.nextPacket(p2)) {
            ++n;
            lastPts = p2.ptsUs;
            if (p2.ptsUs > maxPts) maxPts = p2.ptsUs;
            bytes += p2.bytes.size();
        }
        std::printf("\nread to end: %zu packets, %.3f s reached, %.1f MB\n",
                    n, maxPts / 1e6, bytes / 1048576.0);
        std::printf("last packet pts %.3f s on track %llu\n",
                    lastPts / 1e6, (unsigned long long)p2.trackNumber);
        if (d2.info().durationUs() > 0 &&
            maxPts < static_cast<int64_t>(d2.info().durationUs()) - 1000000)
            std::printf("STOPPED EARLY: header says %.3f s\n",
                        d2.info().durationUs() / 1e6);
    }
    return 0;
}
