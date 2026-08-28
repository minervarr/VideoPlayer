#pragma once

// A seekable std::istream over a POSIX file descriptor.
//
// The bridge between what Android hands a viewer and what core/ can read. A
// file manager opening a video sends ACTION_VIEW with a content:// URI, which
// names a row in another app's ContentProvider — there is no path behind it,
// the read grant belongs to the Intent rather than to us, and the only handle
// that comes back is a descriptor.
//
// core/ must not know that. Demuxer reads a std::istream and nothing else
// (rule 1), so the descriptor is wrapped here, in platform/android/, where
// POSIX headers are allowed.
//
// ── Why not fdopen + a stdio stream ───────────────────────────────────────
//
// libc++ has no stdio_filebuf (that is a libstdc++ extension), so there is no
// standard way to hand an existing FILE* or fd to an ifstream. Writing the
// streambuf is about forty lines and is the portable answer.
//
// Owns the descriptor: close() happens in the destructor. The Java side
// DETACHES the fd from its ParcelFileDescriptor precisely so ownership can
// land somewhere with a defined lifetime, and this is that somewhere.

#include <unistd.h>

#include <istream>
#include <memory>
#include <streambuf>
#include <vector>

namespace vp {

class FdStreambuf : public std::streambuf {
public:
    explicit FdStreambuf(int fd, size_t bufferBytes = 1 << 16);
    ~FdStreambuf() override;

    FdStreambuf(const FdStreambuf&) = delete;
    FdStreambuf& operator=(const FdStreambuf&) = delete;

    bool valid() const { return fd_ >= 0; }

protected:
    int_type underflow() override;
    // Both seeks are implemented, and both must be: Matroska is not a format
    // that can be parsed forwards. seekoff covers relative moves, seekpos the
    // absolute ones the parser makes constantly.
    pos_type seekoff(off_type off, std::ios_base::seekdir dir,
                     std::ios_base::openmode which) override;
    pos_type seekpos(pos_type pos, std::ios_base::openmode which) override;

private:
    int fd_;
    std::vector<char> buf_;
    // Where the descriptor's own offset sits, in the FILE's coordinates, for
    // the byte after everything currently in buf_. Tracking it here rather
    // than calling lseek to ask is what makes tellg cheap — the parser calls
    // it once per element.
    off_t bufFilePos_ = 0;
};

// An istream over `fd`, or nullptr when the descriptor is unusable. Takes
// ownership of `fd` either way: a caller that gets nullptr back has not leaked
// anything.
std::unique_ptr<std::istream> streamFromFd(int fd);

}  // namespace vp
