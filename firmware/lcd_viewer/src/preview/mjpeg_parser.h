#pragma once
#include <cstddef>
#include <cstdint>
namespace lcd {
constexpr size_t kMaxJpegBytes = 128 * 1024;
class MjpegParser {
public:
    using Callback = void(*)(void*,const uint8_t*,size_t,uint64_t);
    MjpegParser(uint8_t* buffer, size_t capacity, Callback callback, void* context);
    void reset();
    bool feed(const uint8_t*, size_t);
    bool finish() const;
    const char* error() const { return error_; }
    bool inFrame() const { return body_ == Body::Jpeg; }
private:
    enum class Http { Status, Headers, Body };
    enum class Body { Boundary, Headers, Jpeg, Cr, Lf, Done };
    enum class Chunk { Size, Data, Cr, Lf, Trailers, Done };
    bool lineByte(uint8_t c, bool& complete);
    bool httpLine();
    bool multipartByte(uint8_t c);
    bool chunkByte(uint8_t c);
    bool fail(const char* reason);
    uint8_t* buffer_; size_t capacity_; Callback callback_; void* context_;
    Http http_; Body body_; Chunk chunk_;
    char line_[513]; size_t lineLength_, headerBytes_;
    char chunkLine_[257]; size_t chunkLineLength_, chunkMetaBytes_;
    char boundary_[73]; size_t jpegLength_, jpegUsed_, chunkRemaining_;
    uint64_t sequence_;
    bool chunked_, contentType_, haveLength_, haveSequence_, partType_, haveTransfer_, haveHttpLength_;
    const char* error_;
};
}
