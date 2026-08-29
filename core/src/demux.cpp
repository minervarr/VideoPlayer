#include "core/demux.h"

#include <algorithm>
#include <deque>
#include <fstream>
#include <map>

#include "ebml.h"
#include "mkv_parser.h"

namespace vp {
namespace {

using namespace ebml;

// Matroska packs several small frames into one Block to save headers. FLAC
// files are laced constantly; HEVC essentially never is. Reading a laced Block
// as one frame yields audio that plays at the wrong speed and video that
// stalls, so all three schemes are handled rather than only the one the video
// track happens to use.
enum class Lacing { None, Xiph, Fixed, EBML };

Lacing lacingOf(uint8_t flags) {
    switch ((flags >> 1) & 0x03) {
        case 0: return Lacing::None;
        case 1: return Lacing::Xiph;
        case 2: return Lacing::Fixed;
        default: return Lacing::EBML;
    }
}

// A signed EBML varint, as used for the size DELTAS in EBML lacing: the value
// is read like a size, then biased down by half its own range.
int64_t readSignedLaceSize(std::istream& in) {
    const std::streampos start = in.tellg();
    uint64_t v = readSize(in);
    const uint64_t len = static_cast<uint64_t>(in.tellg() - start);
    if (len == 0 || len > 8) return 0;
    return static_cast<int64_t>(v) - ((1LL << (7 * len - 1)) - 1);
}

// Seek, unless the stream is already there.
//
// readOneBlock() advances nextElementPos to the end of the element it just
// read, which is exactly where the stream is already standing — so the seek at
// the top of the next iteration was almost always a no-op that nevertheless
// threw away the istream's read buffer, forcing a fresh read for the next
// element header. Every other caller genuinely jumps, and pays nothing here.
void seekTo(std::istream& in, uint64_t pos) {
    if (in.good()) {
        const std::streampos cur = in.tellg();
        if (cur >= 0 && static_cast<uint64_t>(cur) == pos) return;
    }
    in.clear();
    in.seekg(static_cast<std::streamoff>(pos), std::ios::beg);
}

}  // namespace

struct Demuxer::Impl {
    // The stream everything reads through. Owned, because it may be a file
    // this class opened OR one the caller handed over (an fd on Android) —
    // and the parser cannot tell the difference, which is the point.
    std::unique_ptr<std::istream> stream;
    std::istream& in() { return *stream; }
    MkvHeaders    hdr;
    std::string   err;
    bool          open = false;

    // Where the next Block will be read from, and the Cluster we are inside.
    uint64_t clusterEnd     = 0;
    uint64_t nextElementPos = 0;
    int64_t  clusterTimeUs  = 0;

    // Everything read out of Blocks and not yet handed to a caller, in the
    // order the file stores it. ONE queue, not one per track: a per-track map
    // has no natural bound, and a caller draining one track slower than the
    // other silently accumulates the difference — which, with 1.5 MB intra
    // frames, is tens of megabytes a second.
    std::deque<Packet> pending;

    // Buffers to fill again, instead of asking the allocator for another one.
    //
    // Every packet used to be a fresh std::vector sized from the file and
    // freed by the consumer. At ~1.5 MB per all-intra frame that is well past
    // the allocator's mmap threshold, so each frame cost an mmap, some 375
    // page faults as it was written, and a munmap — thirty times a second,
    // for the whole length of a film. Nothing in the profile pointed at it,
    // because it is spread evenly across every page touched.
    //
    // The buffers are all nearly the same size, so after the first few frames
    // reuse never reallocates at all. Bounded because the point is to recycle
    // the working set, not to hold the file.
    std::vector<std::vector<uint8_t>> spare;
    static constexpr size_t kSpareBuffers = 8;

    // Takes a buffer able to hold `size` bytes, reusing one when possible.
    std::vector<uint8_t> takeBuffer(size_t size) {
        std::vector<uint8_t> buf;
        if (!spare.empty()) {
            buf = std::move(spare.back());
            spare.pop_back();
        }
        buf.resize(size);
        return buf;
    }

    // Hands one back. Keeps the capacity and drops the contents.
    void recycle(std::vector<uint8_t>&& buf) {
        if (buf.capacity() == 0 || spare.size() >= kSpareBuffers) return;
        buf.clear();
        spare.push_back(std::move(buf));
    }

    bool enterNextCluster();
    bool readOneBlock();     // appends to `pending`; false at end of stream
    void parseBlock(uint64_t bodyEnd, bool simple, bool blockGroupKeyframe);
};

Demuxer::Demuxer() : impl_(std::make_unique<Impl>()) {}
Demuxer::~Demuxer() = default;

bool Demuxer::open(const std::string& path) {
    close();
    auto f = std::make_unique<std::ifstream>(path, std::ios::binary);
    if (!*f) {
        impl_->err = "cannot open " + path;
        return false;
    }
    return open(std::move(f));
}

bool Demuxer::open(std::unique_ptr<std::istream> stream) {
    // Not close(): a path-based open() has already called it, and calling it
    // again here would drop the stream that was just handed to us.
    impl_->stream = std::move(stream);
    if (!impl_->stream || !*impl_->stream) {
        impl_->err = "the stream handed to Demuxer is not readable";
        return false;
    }
    if (!parseHeaders(impl_->in(), impl_->hdr, impl_->err)) return false;
    if (impl_->hdr.firstClusterPos == 0) {
        impl_->err = "Matroska file has no Cluster — nothing to play";
        return false;
    }
    // Both point at the first Cluster's ID byte, so the first pull's
    // "this cluster is exhausted" branch enters that cluster rather than
    // rewinding to the top of the file.
    impl_->nextElementPos = impl_->hdr.firstClusterPos;
    impl_->clusterEnd     = impl_->hdr.firstClusterPos;
    impl_->open = true;
    return true;
}

void Demuxer::close() {
    impl_ = std::make_unique<Impl>();
}

const SegmentInfo& Demuxer::info() const { return impl_->hdr.info; }
const std::vector<TrackEntry>& Demuxer::tracks() const { return impl_->hdr.tracks; }
bool Demuxer::seekable() const { return !impl_->hdr.cues.empty(); }
const std::string& Demuxer::error() const { return impl_->err; }

namespace {
const TrackEntry* pickTrack(const std::vector<TrackEntry>& tracks, TrackKind kind) {
    const TrackEntry* first = nullptr;
    for (const TrackEntry& t : tracks) {
        if (t.kind != kind || t.codec == Codec::Unknown) continue;
        if (t.isDefault) return &t;      // the muxer's own answer wins
        if (!first) first = &t;
    }
    return first;
}
}  // namespace

const TrackEntry* Demuxer::videoTrack() const {
    return pickTrack(impl_->hdr.tracks, TrackKind::Video);
}
const TrackEntry* Demuxer::audioTrack() const {
    return pickTrack(impl_->hdr.tracks, TrackKind::Audio);
}

bool Demuxer::Impl::enterNextCluster() {
    while (true) {
        seekTo(in(), nextElementPos);
        Element e = readElement(in());
        if (!e.ok) return false;

        if (e.id != kCluster) {
            // Cues, Tags, Chapters and friends can sit between Clusters. Skip
            // by size and keep going; only a truly unreadable element ends
            // playback.
            if (e.unknownSize()) return false;
            nextElementPos = e.endPos();
            continue;
        }

        // An unknown-size Cluster (live mux) simply has no end we can compute;
        // reading runs until an element fails, which is the correct behaviour
        // for a file still being written.
        clusterEnd = e.unknownSize() ? ~0ULL : e.endPos();
        nextElementPos = e.dataPos;

        // Find the Cluster's Timecode. Every Block in the Cluster is relative
        // to it, so getting this wrong does not fail — it silently places the
        // whole cluster at the wrong moment.
        //
        // SCANNED, not assumed to be first. It usually is, and this code read
        // exactly one child and gave up if that child was something else. Real
        // recordings put a CRC-32 (0xBF) ahead of it, so the Timecode was
        // never found, every cluster was treated as starting at zero, and a
        // 42-second file reported timestamps that never passed one second.
        // Playback stalled a few seconds in, because everything after the
        // first cluster claimed to belong to a moment that had already gone.
        clusterTimeUs = 0;
        uint64_t scan = e.dataPos;
        while (scan < clusterEnd) {
            seekTo(in(), scan);
            Element c = readElement(in());
            if (!c.ok || c.unknownSize()) break;
            if (c.id == kTimecode) {
                clusterTimeUs = static_cast<int64_t>(readUInt(in(), c.size) *
                                                     hdr.info.timecodeScaleNs / 1000);
                break;
            }
            // Stop at the first payload element: Timecode is required to
            // precede the blocks, so not having found it by now means the
            // cluster has none, and scanning a 30 MB cluster to prove it would
            // read the whole file at open.
            if (c.id == kSimpleBlock || c.id == kBlockGroup) break;
            scan = c.endPos();
        }

        // Blocks are read from the start of the children either way: the
        // Timecode and any CRC-32 ahead of it are simply skipped as elements
        // this parser does not act on.
        nextElementPos = e.dataPos;
        return true;
    }
}

void Demuxer::Impl::parseBlock(uint64_t bodyEnd, bool simple, bool blockGroupKeyframe) {
    const uint64_t track = readSize(in());       // track number: a size-style varint
    const int64_t rel = readInt(in(), 2);        // signed, relative to the Cluster
    const int flagsByte = in().get();
    if (flagsByte == std::istream::traits_type::eof()) return;
    const uint8_t flags = static_cast<uint8_t>(flagsByte);

    // SimpleBlock states keyframe-ness in its own flags. A Block inside a
    // BlockGroup does not: there, "no ReferenceBlock" is what means keyframe,
    // which the caller has already worked out.
    const bool keyframe = simple ? (flags & 0x80) != 0 : blockGroupKeyframe;
    // `rel` is in TimecodeScale TICKS, exactly like the Cluster's own
    // Timecode — not in microseconds. Adding it raw to an already-scaled
    // clusterTimeUs is off by the scale factor (1000x on a normal 1 ms file),
    // which looks like correct playback for the first cluster and then falls
    // apart. mkv_test asserts the 42-tick block lands at 42000 us.
    const int64_t ptsUs =
        clusterTimeUs + static_cast<int64_t>(rel * static_cast<int64_t>(hdr.info.timecodeScaleNs) / 1000);

    std::vector<uint64_t> sizes;
    const Lacing lacing = lacingOf(flags);
    if (lacing != Lacing::None) {
        const int extra = in().get();
        if (extra == std::istream::traits_type::eof()) return;
        const size_t count = static_cast<size_t>(extra) + 1;

        if (lacing == Lacing::Fixed) {
            const uint64_t remaining = bodyEnd - static_cast<uint64_t>(in().tellg());
            sizes.assign(count, count ? remaining / count : 0);
        } else if (lacing == Lacing::Xiph) {
            // Each size is a run of 0xFF bytes plus a terminator.
            for (size_t i = 0; i + 1 < count; ++i) {
                uint64_t s = 0;
                int c;
                do {
                    c = in().get();
                    if (c == std::istream::traits_type::eof()) return;
                    s += static_cast<uint64_t>(c);
                } while (c == 0xFF);
                sizes.push_back(s);
            }
            sizes.push_back(0);  // the last one is "whatever is left"
        } else {  // EBML lacing: first size absolute, the rest signed deltas
            uint64_t s = readSize(in());
            sizes.push_back(s);
            for (size_t i = 1; i + 1 < count; ++i) {
                s = static_cast<uint64_t>(static_cast<int64_t>(s) + readSignedLaceSize(in()));
                sizes.push_back(s);
            }
            if (count > 1) sizes.push_back(0);
        }
        if (!sizes.empty()) {
            uint64_t used = 0;
            for (size_t i = 0; i + 1 < sizes.size(); ++i) used += sizes[i];
            const uint64_t here = static_cast<uint64_t>(in().tellg());
            sizes.back() = bodyEnd > here + used ? bodyEnd - here - used : 0;
        }
    } else {
        sizes.push_back(bodyEnd - static_cast<uint64_t>(in().tellg()));
    }

    // Every frame in a laced Block shares one timestamp in the container. That
    // is what the container says, and inventing sub-timestamps here would mean
    // guessing a frame duration — FLAC's decoder knows the real one, and the
    // audio clock is what drives presentation anyway (see core/clock.h).
    for (uint64_t s : sizes) {
        if (s == 0) continue;
        // Sized from the file, so bounded by the file. Lacing sizes in
        // particular are sums and signed deltas of values read out of the
        // stream, so one corrupt byte can produce an enormous one — and
        // std::vector will faithfully try to allocate it.
        if (hdr.fileEnd > 0 && s > hdr.fileEnd) return;
        Packet p;
        p.trackNumber = track;
        p.ptsUs = p.dtsUs = ptsUs;
        p.keyframe = keyframe;
        p.bytes = takeBuffer(static_cast<size_t>(s));
        in().read(reinterpret_cast<char*>(p.bytes.data()), static_cast<std::streamsize>(s));
        if (!in()) return;
        pending.push_back(std::move(p));
    }
}

bool Demuxer::Impl::readOneBlock() {
    while (true) {
        if (nextElementPos >= clusterEnd) {
            const uint64_t resumeAt = clusterEnd == ~0ULL ? nextElementPos : clusterEnd;
            nextElementPos = resumeAt;
            if (!enterNextCluster()) return false;
        }

        seekTo(in(), nextElementPos);
        Element e = readElement(in());
        if (!e.ok || e.unknownSize()) return false;
        nextElementPos = e.endPos();

        if (e.id == kSimpleBlock) {
            parseBlock(e.endPos(), /*simple=*/true, false);
            return true;
        }
        if (e.id == kBlockGroup) {
            // Keyframe-ness here is the ABSENCE of ReferenceBlock, so the
            // group's children are scanned before its Block is read.
            bool hasReference = false;
            Element block;
            uint64_t pos = e.dataPos;
            while (pos < e.endPos()) {
                seekTo(in(), pos);
                Element c = readElement(in());
                if (!c.ok || c.unknownSize()) break;
                if (c.id == kReferenceBlock) hasReference = true;
                if (c.id == kBlock) block = c;
                pos = c.endPos();
            }
            if (block.ok) {
                seekTo(in(), block.dataPos);
                parseBlock(block.endPos(), /*simple=*/false, !hasReference);
                return true;
            }
            continue;
        }
        // Anything else inside a Cluster (CRC-32, Void, EncryptedBlock we do
        // not support) is skipped; nextElementPos already moved past it.
    }
}

bool Demuxer::nextPacket(Packet& out) {
    if (!impl_->open) return false;
    while (impl_->pending.empty()) {
        if (!impl_->readOneBlock()) return false;
    }
    // The caller is done with whatever it held; keep the allocation rather
    // than let the assignment below free it.
    impl_->recycle(std::move(out.bytes));
    out = std::move(impl_->pending.front());
    impl_->pending.pop_front();
    return true;
}

bool Demuxer::nextPacket(uint64_t trackNumber, Packet& out) {
    if (!impl_->open) return false;
    while (true) {
        for (auto it = impl_->pending.begin(); it != impl_->pending.end(); ++it) {
            if (it->trackNumber != trackNumber) continue;
            impl_->recycle(std::move(out.bytes));
            out = std::move(*it);
            impl_->pending.erase(it);
            return true;
        }
        if (!impl_->readOneBlock()) return false;
    }
}

int64_t Demuxer::seek(int64_t timeUs) {
    if (!impl_->open || impl_->hdr.cues.empty()) {
        impl_->err = "this file has no Cues — seeking would mean scanning it";
        return -1;
    }

    // The nearest Cue at or BEFORE the target. Never after: landing past the
    // requested point means the user asked for a moment and got a later one,
    // and there is no keyframe behind them to decode from.
    const CuePoint* best = nullptr;
    for (const CuePoint& c : impl_->hdr.cues) {
        if (static_cast<int64_t>(c.timeUs) > timeUs) continue;
        if (!best || c.timeUs > best->timeUs) best = &c;
    }
    if (!best) best = &*std::min_element(
        impl_->hdr.cues.begin(), impl_->hdr.cues.end(),
        [](const CuePoint& a, const CuePoint& b) { return a.timeUs < b.timeUs; });

    for (Packet& p : impl_->pending) impl_->recycle(std::move(p.bytes));
    impl_->pending.clear();
    impl_->clusterEnd = 0;
    impl_->nextElementPos = best->clusterOffset;
    if (!impl_->enterNextCluster()) {
        impl_->err = "Cue pointed at a position that is not a Cluster";
        return -1;
    }
    return static_cast<int64_t>(best->timeUs);
}

}  // namespace vp
