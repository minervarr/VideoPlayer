#include "fd_stream.hh"

#include <sys/types.h>

#include <algorithm>

namespace vp {

FdStreambuf::FdStreambuf(int fd, size_t bufferBytes)
    : fd_(fd), buf_(bufferBytes) {
    // Empty get area: the first read calls underflow().
    setg(buf_.data(), buf_.data(), buf_.data());
    if (fd_ >= 0) bufFilePos_ = ::lseek(fd_, 0, SEEK_CUR);
}

FdStreambuf::~FdStreambuf() {
    if (fd_ >= 0) ::close(fd_);
}

std::streambuf::int_type FdStreambuf::underflow() {
    if (fd_ < 0) return traits_type::eof();
    if (gptr() < egptr()) return traits_type::to_int_type(*gptr());

    // read() may return short for reasons that are not end-of-file — a pipe,
    // a signal, a provider streaming from somewhere slow. Only 0 means EOF.
    ssize_t n;
    do {
        n = ::read(fd_, buf_.data(), buf_.size());
    } while (n < 0 && errno == EINTR);

    if (n <= 0) return traits_type::eof();

    bufFilePos_ += n;
    setg(buf_.data(), buf_.data(), buf_.data() + n);
    return traits_type::to_int_type(*gptr());
}

std::streambuf::pos_type FdStreambuf::seekoff(off_type off,
                                              std::ios_base::seekdir dir,
                                              std::ios_base::openmode which) {
    if (fd_ < 0 || !(which & std::ios_base::in)) return pos_type(off_type(-1));

    // Where the reader logically is: the end of the buffer, minus whatever of
    // it has not been consumed yet.
    const off_t logical = bufFilePos_ - static_cast<off_t>(egptr() - gptr());

    // tellg() is seekoff(0, cur), and it is called once per element by the
    // parser. Answering it from bookkeeping rather than an lseek is the
    // difference between a syscall per element and none.
    if (off == 0 && dir == std::ios_base::cur) return pos_type(logical);

    off_t target;
    switch (dir) {
        case std::ios_base::beg: target = static_cast<off_t>(off); break;
        case std::ios_base::cur: target = logical + static_cast<off_t>(off); break;
        case std::ios_base::end: {
            const off_t end = ::lseek(fd_, 0, SEEK_END);
            if (end < 0) return pos_type(off_type(-1));
            target = end + static_cast<off_t>(off);
            break;
        }
        default: return pos_type(off_type(-1));
    }
    return seekpos(pos_type(target), which);
}

std::streambuf::pos_type FdStreambuf::seekpos(pos_type pos,
                                              std::ios_base::openmode which) {
    if (fd_ < 0 || !(which & std::ios_base::in)) return pos_type(off_type(-1));
    const off_t target = static_cast<off_t>(pos);
    if (target < 0) return pos_type(off_type(-1));

    // Inside the buffer already: move the get pointer and issue no syscall.
    // The parser seeks backwards by a few bytes constantly (read an element
    // header, decide, reposition), and almost all of those land in here.
    const off_t bufStart = bufFilePos_ - static_cast<off_t>(egptr() - eback());
    if (egptr() > eback() && target >= bufStart && target < bufFilePos_) {
        setg(eback(), eback() + (target - bufStart), egptr());
        return pos;
    }

    const off_t landed = ::lseek(fd_, target, SEEK_SET);
    if (landed < 0) return pos_type(off_type(-1));
    bufFilePos_ = landed;
    setg(buf_.data(), buf_.data(), buf_.data());   // buffer is now stale
    return pos_type(landed);
}

namespace {
// An istream that owns its streambuf, so the two have one lifetime instead of
// two that a caller has to keep in step.
class FdIStream : public std::istream {
public:
    explicit FdIStream(int fd) : std::istream(nullptr), buf_(fd) { rdbuf(&buf_); }
private:
    FdStreambuf buf_;
};
}  // namespace

std::unique_ptr<std::istream> streamFromFd(int fd) {
    if (fd < 0) return nullptr;
    // A descriptor that cannot seek cannot carry Matroska: Cues sit after the
    // clusters in most files and are reached by seeking to them. Find out here,
    // where it can still be reported, rather than as a parse failure later.
    if (::lseek(fd, 0, SEEK_SET) < 0) {
        ::close(fd);
        return nullptr;
    }
    return std::make_unique<FdIStream>(fd);
}

}  // namespace vp
