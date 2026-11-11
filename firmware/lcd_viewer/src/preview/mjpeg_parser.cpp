#include "mjpeg_parser.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>

namespace lcd {
namespace {
bool equals(const char* a, const char* b) {
    while (*a && *b) if (std::tolower(static_cast<unsigned char>(*a++)) != std::tolower(static_cast<unsigned char>(*b++))) return false;
    return *a == *b;
}
char* trim(char* s) {
    while (*s==' ' || *s=='\t') ++s;
    char* end=s+std::strlen(s);
    while(end>s && (end[-1]==' ' || end[-1]=='\t')) *--end=0;
    return s;
}
bool number(const char* s, uint64_t& out, unsigned base=10) {
    if(!*s) return false;
    out=0;
    for(;*s;++s) {
        unsigned char c=static_cast<unsigned char>(*s); unsigned n;
        if(c>='0' && c<='9') n=c-'0';
        else if(base==16 && std::tolower(c)>='a' && std::tolower(c)<='f') n=std::tolower(c)-'a'+10;
        else return false;
        if(n>=base || out>(std::numeric_limits<uint64_t>::max()-n)/base) return false;
        out=out*base+n;
    }
    return true;
}
}
MjpegParser::MjpegParser(uint8_t* buffer,size_t capacity,Callback callback,void* context)
    :buffer_(buffer),capacity_(std::min(capacity,kMaxJpegBytes)),callback_(callback),context_(context) { reset(); }
void MjpegParser::reset() {
    http_=Http::Status; body_=Body::Boundary; chunk_=Chunk::Size;
    lineLength_=headerBytes_=jpegLength_=jpegUsed_=chunkRemaining_=chunkLineLength_=chunkMetaBytes_=0;
    line_[0]=boundary_[0]=0; sequence_=0; error_=nullptr;
    chunked_=contentType_=haveLength_=haveSequence_=partType_=haveTransfer_=haveHttpLength_=false;
}
bool MjpegParser::fail(const char* s) { error_=s; return false; }
bool MjpegParser::lineByte(uint8_t c,bool& complete) {
    complete=false;
    if(c=='\n') {
        if(!lineLength_ || line_[lineLength_-1]!='\r') return fail("line missing CR");
        line_[--lineLength_]=0; complete=true; return true;
    }
    if(c==0 || lineLength_>=sizeof(line_)-1) return fail("header line too long");
    line_[lineLength_++]=static_cast<char>(c); return true;
}
bool MjpegParser::httpLine() {
    if(http_==Http::Status) {
        if(std::strncmp(line_,"HTTP/1.1 200 ",13)!=0 && std::strncmp(line_,"HTTP/1.0 200 ",13)!=0) return fail("HTTP status is not 200");
        http_=Http::Headers; return true;
    }
    if(!*line_) {
        if(!contentType_ || !*boundary_ || (haveTransfer_ && haveHttpLength_)) return fail("invalid HTTP framing");
        http_=Http::Body; headerBytes_=0; return true;
    }
    char* colon=std::strchr(line_,':'); if(!colon) return fail("invalid HTTP header");
    *colon=0; char* value=trim(colon+1);
    if(equals(line_,"Content-Type")) {
        if(contentType_) return fail("duplicate HTTP content type");
        char* semicolon=std::strchr(value,';'); if(!semicolon) return fail("missing multipart boundary");
        *semicolon=0; if(!equals(trim(value),"multipart/x-mixed-replace")) return fail("not MJPEG");
        char* parameter=trim(semicolon+1);
        if(std::strncmp(parameter,"boundary=",9)!=0) return fail("missing boundary parameter");
        char* boundary=trim(parameter+9); size_t n=std::strlen(boundary);
        if(n>=2 && boundary[0]=='"' && boundary[n-1]=='"') { ++boundary; n-=2; }
        if(n==0 || n>70) return fail("invalid boundary size");
        for(size_t i=0;i<n;++i) if(static_cast<unsigned char>(boundary[i])<=32 || boundary[i]=='"') return fail("invalid boundary");
        boundary_[0]='-';boundary_[1]='-';std::memcpy(boundary_+2,boundary,n);boundary_[n+2]=0;contentType_=true;
    } else if(equals(line_,"Transfer-Encoding")) {
        if(haveTransfer_ || !equals(value,"chunked")) return fail("unsupported transfer encoding");
        haveTransfer_=chunked_=true;
    } else if(equals(line_,"Content-Length")) {
        uint64_t n; if(haveHttpLength_ || !number(value,n)) return fail("invalid HTTP length");
        haveHttpLength_=true;
    } else if(equals(line_,"Content-Encoding") && !equals(value,"identity")) return fail("unsupported content encoding");
    return true;
}
bool MjpegParser::multipartByte(uint8_t c) {
    if(body_==Body::Done) return fail("data after closing boundary");
    if(body_==Body::Jpeg) {
        buffer_[jpegUsed_++]=c;
        if(jpegUsed_==jpegLength_) {
            if(buffer_[0]!=0xff || buffer_[1]!=0xd8 || buffer_[jpegLength_-2]!=0xff || buffer_[jpegLength_-1]!=0xd9) return fail("invalid JPEG markers");
            if(callback_) callback_(context_,buffer_,jpegLength_,sequence_);
            body_=Body::Cr;
        }
        return true;
    }
    if(body_==Body::Cr) { if(c!='\r') return fail("missing JPEG separator CR"); body_=Body::Lf;return true; }
    if(body_==Body::Lf) { if(c!='\n') return fail("missing JPEG separator LF"); body_=Body::Boundary;headerBytes_=0;return true; }
    if(++headerBytes_>4096) return fail("multipart headers too large");
    bool complete; if(!lineByte(c,complete)) return false; if(!complete) return true;
    bool ok=true;
    if(body_==Body::Boundary) {
        if(*line_) {
            const size_t n=std::strlen(boundary_);
            if(std::strcmp(line_,boundary_)==0) {
                body_=Body::Headers;haveLength_=haveSequence_=partType_=false;jpegUsed_=0;
            } else if(std::strncmp(line_,boundary_,n)==0 && std::strcmp(line_+n,"--")==0) body_=Body::Done;
            else ok=fail("multipart boundary mismatch");
        }
    } else if(!*line_) {
        if(!haveLength_ || !haveSequence_ || !partType_) ok=fail("missing part metadata");
        else body_=Body::Jpeg;
    } else {
        char* colon=std::strchr(line_,':');
        if(!colon) ok=fail("invalid part header");
        else {
            *colon=0; char* value=trim(colon+1); uint64_t n;
            if(equals(line_,"Content-Length")) {
                if(haveLength_ || !number(value,n) || n<4 || n>capacity_ || !buffer_) ok=fail("invalid JPEG length");
                else { haveLength_=true;jpegLength_=static_cast<size_t>(n); }
            } else if(equals(line_,"X-Sequence")) {
                if(haveSequence_ || !number(value,sequence_)) ok=fail("invalid sequence"); else haveSequence_=true;
            } else if(equals(line_,"Content-Type")) {
                if(partType_ || !equals(value,"image/jpeg")) ok=fail("invalid part content type"); else partType_=true;
            }
        }
    }
    lineLength_=0;return ok;
}
bool MjpegParser::chunkByte(uint8_t c) {
    if(chunk_==Chunk::Done) return fail("data after final chunk");
    if(chunk_==Chunk::Data) {
        if(!multipartByte(c)) return false;
        if(--chunkRemaining_==0) chunk_=Chunk::Cr;
        return true;
    }
    if(chunk_==Chunk::Cr) { if(c!='\r') return fail("invalid chunk CR");chunk_=Chunk::Lf;return true; }
    if(chunk_==Chunk::Lf) { if(c!='\n') return fail("invalid chunk LF");chunk_=Chunk::Size;return true; }
    return fail("internal chunk metadata state");
}
bool MjpegParser::feed(const uint8_t* data,size_t length) {
    if(error_) return false;
    for(size_t i=0;i<length;++i) {
        const uint8_t c=data[i];
        if(http_!=Http::Body) {
            if(++headerBytes_>4096) return fail("HTTP headers too large");
            bool complete; if(!lineByte(c,complete)) return false;
            if(complete) { if(!httpLine()) return false;lineLength_=0; }
        } else if(chunked_) {
            // Separate storage preserves a multipart header split across HTTP chunks.
            if(chunk_==Chunk::Size || chunk_==Chunk::Trailers) {
                if(++chunkMetaBytes_>4096) return fail("chunk metadata too large");
                if(c=='\n') {
                    if(!chunkLineLength_ || chunkLine_[chunkLineLength_-1]!='\r') return fail("invalid chunk line");
                    chunkLine_[--chunkLineLength_]=0;
                    if(chunk_==Chunk::Size) {
                        char* extension=std::strchr(chunkLine_,';');if(extension)*extension=0;
                        uint64_t n;if(!number(chunkLine_,n,16) || n>1024*1024) return fail("invalid chunk size");
                        chunkRemaining_=static_cast<size_t>(n);chunk_=n?Chunk::Data:Chunk::Trailers;chunkMetaBytes_=0;
                    } else if(chunkLineLength_==0) chunk_=Chunk::Done;
                    chunkLineLength_=0;
                } else {
                    if(c==0 || chunkLineLength_>=sizeof(chunkLine_)-1) return fail("chunk line too long");
                    chunkLine_[chunkLineLength_++]=static_cast<char>(c);
                }
            } else if(!chunkByte(c)) return false;
        } else if(!multipartByte(c)) return false;
    }
    return true;
}
bool MjpegParser::finish() const {
    return !error_ && http_==Http::Body && (body_==Body::Boundary || body_==Body::Done) && lineLength_==0 && (!chunked_ || chunk_==Chunk::Done);
}
}
