#pragma once

#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>

// The scene's compact number blobs ("x y z yaw ..." as one JSON string: painted
// grass/trees/flowers, terrain sculpt + paint). Same text as the iostreams that
// used to write them -- %.Ng floats, plain ints, one space after each -- so old
// and new files read either way. What changes is the speed: MSVC's iostream
// number conversion took 1.5 s to read and 2.7 s to write scaper's 7.9 million
// grass floats; std::from_chars / std::to_chars do it in 0.3 s / 0.2 s.

class NumberBlobWriter {
public:
    explicit NumberBlobWriter(int precision, std::size_t expectedValues = 0)
        : precision_(precision) {
        if (expectedValues) out_.reserve(expectedValues * 10);
    }
    void put(float v) {
        char buf[48];
        const auto r = std::to_chars(buf, buf + sizeof(buf), v,
                                     std::chars_format::general, precision_);
        out_.append(buf, r.ptr);
        out_ += ' ';
    }
    void put(int v) {
        char buf[16];
        const auto r = std::to_chars(buf, buf + sizeof(buf), v);
        out_.append(buf, r.ptr);
        out_ += ' ';
    }
    std::string take() { return std::move(out_); }

private:
    int         precision_;
    std::string out_;
};

// Reads values back in order. next() fails at the end of the text or at the first
// token that is not a number, like `stream >> v` did.
class NumberBlobReader {
public:
    explicit NumberBlobReader(std::string_view s) : p_(s.data()), end_(s.data() + s.size()) {}
    template <class T>
    bool next(T& v) {
        while (p_ < end_ && (*p_ == ' ' || *p_ == '\n' || *p_ == '\r' || *p_ == '\t')) ++p_;
        if (p_ >= end_) return false;
        // from_chars takes no leading '+', which the streams did accept.
        if (*p_ == '+') ++p_;
        const auto r = std::from_chars(p_, end_, v);
        if (r.ec != std::errc()) { p_ = end_; return false; }
        p_ = r.ptr;
        return true;
    }

private:
    const char* p_;
    const char* end_;
};
