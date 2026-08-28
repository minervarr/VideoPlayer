#include "mkv_parser.h"

#include "ebml.h"

#include <cmath>
#include <type_traits>
#include <utility>

namespace vp {
namespace {

using namespace ebml;

void skipTo(std::istream& in, uint64_t pos) {
    in.clear();
    in.seekg(static_cast<std::streamoff>(pos), std::ios::beg);
}

// Walks the children of an element whose payload spans [dataPos, endPos),
// calling `fn(child)` for each. `fn` returns having consumed whatever it
// wanted; this always repositions to the child's end afterwards, which is what
// makes "skip anything unrecognized" the automatic behaviour rather than a
// thing every caller has to remember.
// `fn` may set `stop` to end the walk early — which the Segment-level walk
// does at the first Cluster, so that opening a file never reads media data.
template <typename F>
void forEachChild(std::istream& in, uint64_t dataPos, uint64_t endPos, F&& fn) {
    skipTo(in, dataPos);
    bool stop = false;
    while (in && !stop && static_cast<uint64_t>(in.tellg()) < endPos) {
        Element e = readElement(in);
        if (!e.ok) break;
        // An unknown-size child inside a sized parent: the only sane reading is
        // "runs to the parent's end". Bail rather than seek to nonsense.
        const uint64_t childEnd = e.unknownSize() ? endPos : e.endPos();
        if (childEnd > endPos) break;
        if constexpr (std::is_invocable_v<F, const Element&, bool&>) {
            fn(e, stop);
        } else {
            fn(e);
        }
        if (!stop) skipTo(in, childEnd);
    }
}

void parseMastering(std::istream& in, const Element& parent, MasteringMetadata& m) {
    m.present = true;
    forEachChild(in, parent.dataPos, parent.endPos(), [&](const Element& e) {
        switch (e.id) {
            case kPrimaryRChromX:   m.redX   = readFloat(in, e.size); break;
            case kPrimaryRChromY:   m.redY   = readFloat(in, e.size); break;
            case kPrimaryGChromX:   m.greenX = readFloat(in, e.size); break;
            case kPrimaryGChromY:   m.greenY = readFloat(in, e.size); break;
            case kPrimaryBChromX:   m.blueX  = readFloat(in, e.size); break;
            case kPrimaryBChromY:   m.blueY  = readFloat(in, e.size); break;
            case kWhitePointChromX: m.whiteX = readFloat(in, e.size); break;
            case kWhitePointChromY: m.whiteY = readFloat(in, e.size); break;
            case kLuminanceMax:     m.maxLuminance = readFloat(in, e.size); break;
            case kLuminanceMin:     m.minLuminance = readFloat(in, e.size); break;
            default: break;
        }
    });
}

// Colour's enumerations are stored as-is, NOT validated against the enums in
// mkv.h. A transfer characteristic we have no name for must reach the decoder
// unchanged; collapsing it to Unspecified here would silently reinterpret
// somebody's file. See the note at the top of core/mkv.h.
void parseColour(std::istream& in, const Element& parent, ColourInfo& c) {
    forEachChild(in, parent.dataPos, parent.endPos(), [&](const Element& e) {
        switch (e.id) {
            case kMatrixCoeffs:
                c.matrix = static_cast<MatrixCoeffs>(readUInt(in, e.size)); break;
            case kBitsPerChannel:
                c.bitDepth = static_cast<int>(readUInt(in, e.size)); break;
            case kRange:
                c.range = static_cast<Range>(readUInt(in, e.size)); break;
            case kTransferChar:
                c.transfer = static_cast<Transfer>(readUInt(in, e.size)); break;
            case kPrimaries:
                c.primaries = static_cast<Primaries>(readUInt(in, e.size)); break;
            case kMaxCLL:
                c.maxCLL = static_cast<int>(readUInt(in, e.size)); break;
            case kMaxFALL:
                c.maxFALL = static_cast<int>(readUInt(in, e.size)); break;
            case kMasteringMeta:
                parseMastering(in, e, c.mastering); break;
            default: break;
        }
    });
}

void parseTrackEntry(std::istream& in, const Element& parent, TrackEntry& t) {
    forEachChild(in, parent.dataPos, parent.endPos(), [&](const Element& e) {
        switch (e.id) {
            case kTrackNumber: t.number = readUInt(in, e.size); break;
            case kTrackType:   t.kind = static_cast<TrackKind>(readUInt(in, e.size)); break;
            case kCodecID:     t.codecId = readString(in, e.size); break;
            case kLanguage:    t.language = readString(in, e.size); break;
            case kFlagDefault: t.isDefault = readUInt(in, e.size) != 0; break;
            case kCodecPrivate:
                t.codecPrivate.resize(static_cast<size_t>(e.size));
                if (e.size) in.read(reinterpret_cast<char*>(t.codecPrivate.data()),
                                    static_cast<std::streamsize>(e.size));
                break;
            case kVideo:
                forEachChild(in, e.dataPos, e.endPos(), [&](const Element& v) {
                    switch (v.id) {
                        case kPixelWidth:    t.width  = static_cast<uint32_t>(readUInt(in, v.size)); break;
                        case kPixelHeight:   t.height = static_cast<uint32_t>(readUInt(in, v.size)); break;
                        case kDisplayWidth:  t.displayWidth  = static_cast<uint32_t>(readUInt(in, v.size)); break;
                        case kDisplayHeight: t.displayHeight = static_cast<uint32_t>(readUInt(in, v.size)); break;
                        case kColour:        parseColour(in, v, t.colour); break;
                        case kProjection:
                            forEachChild(in, v.dataPos, v.endPos(), [&](const Element& pj) {
                                if (pj.id != kProjectionRoll) return;
                                // Matroska states the roll as a COUNTER-clockwise
                                // angle in degrees; a player needs the clockwise
                                // turn that undoes it, hence the negation. Snapped
                                // to a quadrant because that is all a sampler can
                                // do without resampling the picture, and because
                                // every real file uses one.
                                const double roll = readFloat(in, pj.size);
                                int q = static_cast<int>(std::lround(-roll / 90.0)) % 4;
                                if (q < 0) q += 4;
                                t.rotationDegrees = q * 90;
                            });
                            break;
                        default: break;
                    }
                });
                break;
            case kAudio:
                forEachChild(in, e.dataPos, e.endPos(), [&](const Element& a) {
                    switch (a.id) {
                        case kSamplingFreq: t.sampleRate = readFloat(in, a.size); break;
                        case kChannels:     t.channels = static_cast<uint32_t>(readUInt(in, a.size)); break;
                        case kBitDepth:     t.bitsPerSample = static_cast<uint32_t>(readUInt(in, a.size)); break;
                        default: break;
                    }
                });
                break;
            default: break;
        }
    });
    t.codec = codecFromId(t.codecId);
    if (t.language.empty()) t.language = "und";
}

void parseCues(std::istream& in, const Element& parent, uint64_t segmentDataPos,
               uint64_t timecodeScaleNs, std::vector<CuePoint>& out) {
    forEachChild(in, parent.dataPos, parent.endPos(), [&](const Element& cp) {
        if (cp.id != kCuePoint) return;
        uint64_t timeTicks = 0;
        std::vector<CuePoint> positions;
        forEachChild(in, cp.dataPos, cp.endPos(), [&](const Element& e) {
            if (e.id == kCueTime) {
                timeTicks = readUInt(in, e.size);
            } else if (e.id == kCueTrackPosition) {
                CuePoint c;
                forEachChild(in, e.dataPos, e.endPos(), [&](const Element& p) {
                    switch (p.id) {
                        case kCueTrack:       c.trackNumber = readUInt(in, p.size); break;
                        // Relative to the Segment's payload, not to the file.
                        case kCueClusterPos:  c.clusterOffset = segmentDataPos + readUInt(in, p.size); break;
                        case kCueRelativePos: c.relativePosition = readUInt(in, p.size); break;
                        default: break;
                    }
                });
                positions.push_back(c);
            }
        });
        for (CuePoint& c : positions) {
            c.timeUs = timeTicks * timecodeScaleNs / 1000;
            out.push_back(c);
        }
    });
}

}  // namespace

Codec codecFromId(const std::string& codecId) {
    // Matroska writes HEVC as V_MPEGH/ISO/HEVC. Nothing else is accepted:
    // this player decodes one video codec on purpose.
    if (codecId == "V_MPEGH/ISO/HEVC") return Codec::HEVC;
    if (codecId == "A_FLAC")           return Codec::FLAC;
    return Codec::Unknown;
}

bool parseHeaders(std::istream& in, MkvHeaders& out, std::string& err) {
    err.clear();
    skipTo(in, 0);

    Element hdr = readElement(in);
    if (!hdr.ok || hdr.id != kEBMLHeader) {
        err = "not a Matroska file (no EBML header)";
        return false;
    }
    skipTo(in, hdr.endPos());

    Element seg = readElement(in);
    if (!seg.ok || seg.id != kSegment) {
        err = "no Segment element after the EBML header";
        return false;
    }
    out.segmentDataPos = seg.dataPos;

    // How far the Segment's payload runs.
    //
    // Three cases collapse to "the end of the file", and only the first is the
    // one the spec describes:
    //
    //   * an all-ones size — the documented "unknown", written by live muxers;
    //   * a size of ZERO — a muxer that reserved the field and never went back
    //     to patch it. Read literally that is an empty Segment, which is what
    //     this parser did: it walked no children and reported "no Tracks",
    //     about a 908 MB recording whose Info element began four bytes later.
    //     A recording interrupted before finalisation looks exactly like this,
    //     and it is a file a person still wants to watch;
    //   * a size that runs past the end of the file — the same situation, with
    //     a partially written value or a truncated download.
    //
    // Being lenient here costs nothing: a genuinely empty Segment has no
    // Tracks either way and still fails below, with the same message.
    in.clear();
    in.seekg(0, std::ios::end);
    const uint64_t fileEnd = static_cast<uint64_t>(in.tellg());

    uint64_t segEnd = seg.endPos();
    if (seg.unknownSize() || seg.size == 0 || segEnd > fileEnd)
        segEnd = fileEnd;

    // Where SeekHead says Cues live, if it says. Absolute, already rebased
    // onto segmentDataPos.
    uint64_t cuesSeekPos = 0;

    Element cuesElement;  // parsed last: it needs TimecodeScale, which Info carries
    forEachChild(in, seg.dataPos, segEnd, [&](const Element& e, bool& stop) {
        switch (e.id) {
            case kSeekHead:
                // The index of the index. Read only for where Cues are: in a
                // file muxed with Cues at the END (mkvmerge's default for
                // streaming-unfriendly output), this is the ONLY way to reach
                // them without walking every Cluster in between.
                forEachChild(in, e.dataPos, e.endPos(), [&](const Element& s) {
                    if (s.id != kSeek) return;
                    uint64_t seekId = 0, seekPos = 0;
                    forEachChild(in, s.dataPos, s.endPos(), [&](const Element& f) {
                        if (f.id == kSeekID)       seekId = readUInt(in, f.size);
                        else if (f.id == kSeekPos) seekPos = readUInt(in, f.size);
                    });
                    if (seekId == kCues) cuesSeekPos = out.segmentDataPos + seekPos;
                });
                break;
            case kInfo:
                forEachChild(in, e.dataPos, e.endPos(), [&](const Element& i) {
                    switch (i.id) {
                        case kTimecodeScale: out.info.timecodeScaleNs = readUInt(in, i.size); break;
                        case kDuration:      out.info.durationTicks = readFloat(in, i.size); break;
                        case kTitle:         out.info.title = readString(in, i.size); break;
                        case kMuxingApp:     out.info.muxingApp = readString(in, i.size); break;
                        case kWritingApp:    out.info.writingApp = readString(in, i.size); break;
                        default: break;
                    }
                });
                break;
            case kTracks:
                forEachChild(in, e.dataPos, e.endPos(), [&](const Element& t) {
                    if (t.id != kTrackEntry) return;
                    TrackEntry entry;
                    parseTrackEntry(in, t, entry);
                    out.tracks.push_back(std::move(entry));
                });
                break;
            case kCues:
                cuesElement = e;
                break;
            case kCluster:
                // STOP here. Everything above a Cluster is metadata; below it
                // is media data, and walking element headers through a 40 GB
                // remux to reach a Cues element at the end would make opening
                // a file take minutes. SeekHead above is how we get there
                // instead.
                out.firstClusterPos = e.startPos;
                stop = true;
                break;
            default: break;
        }
    });

    // Cues before the first Cluster (streaming-friendly muxes) were captured
    // in the walk; Cues after it are reached through SeekHead. Either way they
    // are parsed here, once TimecodeScale is known.
    if (!cuesElement.ok && cuesSeekPos) {
        skipTo(in, cuesSeekPos);
        Element e = readElement(in);
        if (e.ok && e.id == kCues) cuesElement = e;
    }
    if (cuesElement.ok)
        parseCues(in, cuesElement, out.segmentDataPos, out.info.timecodeScaleNs, out.cues);

    if (out.tracks.empty()) {
        err = "Matroska file has no Tracks element";
        return false;
    }
    return true;
}

}  // namespace vp
