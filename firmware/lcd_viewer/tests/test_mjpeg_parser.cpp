#include "../src/preview/mjpeg_parser.h"
#include "../src/preview/frame_deadline.h"
#include "../src/preview/session_clock.h"
#include "../src/preview/session_state.h"
#include <cstdlib>
#define assert(expr) do { if (!(expr)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); std::exit(1); } } while(false)
#include <cstdio>
#include <string>
#include <vector>

using namespace lcd;
struct Received { std::vector<std::string> frames; std::vector<uint64_t> sequences; };
static void receive(void* p, const uint8_t* bytes, size_t n, uint64_t sequence) {
    auto& r = *static_cast<Received*>(p);
    r.frames.emplace_back(reinterpret_cast<const char*>(bytes), n);
    r.sequences.push_back(sequence);
}
static const std::string jpeg("\xff\xd8\x12\x34\xff\xd9", 6);
static std::string part(const std::string& data = jpeg, const std::string& len = "6", const std::string& seq = "42") {
    return "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " + len + "\r\nX-Sequence: " + seq + "\r\n\r\n" + data + "\r\n";
}
static std::string header(bool chunked = false) {
    return "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=\"frame\"\r\n" +
           std::string(chunked ? "Transfer-Encoding: chunked\r\n" : "") + "\r\n";
}
static std::string chunk(const std::string& body, size_t stride) {
    std::string out;
    for (size_t i=0; i<body.size(); i+=stride) {
        auto block=body.substr(i,stride); char n[40]; std::snprintf(n,sizeof n,"%zx;foo=bar\r\n",block.size());
        out+=n; out+=block; out+="\r\n";
    }
    return out + "0\r\nX-End: yes\r\n\r\n";
}
static void feedFragmented(MjpegParser& p, const std::string& s, size_t n) {
    for (size_t i=0; i<s.size(); i+=n) assert(p.feed(reinterpret_cast<const uint8_t*>(s.data()+i),std::min(n,s.size()-i)));
}
int main() {
    std::vector<uint8_t> storage(kMaxJpegBytes+32,0xa5);
    Received r;
    MjpegParser p(storage.data()+16, kMaxJpegBytes, receive, &r);
    for (size_t stride : {size_t(1),size_t(2),size_t(7),size_t(511),size_t(4096)}) {
        p.reset(); r={}; feedFragmented(p,header()+part()+part(jpeg,"6","43"),stride);
        assert(r.frames.size()==2 && r.frames[0]==jpeg && r.sequences[1]==43);
        assert(p.finish());
        for(size_t chunkSize : {size_t(1),size_t(5),size_t(113)}) {
            p.reset(); r={}; feedFragmented(p,header(true)+chunk(part()+part(jpeg,"6","43")+"--frame--\r\n",chunkSize),stride);
            assert(r.frames.size()==2 && r.sequences[0]==42 && p.finish());
        }
    }
    // A huge advertised length must fail before writing a byte past the provided buffer.
    for (const auto& bad : {std::string("0"),std::string("131073"),std::string("-1"),std::string("6garbage"),std::string("18446744073709551616")}) {
        p.reset(); r={}; auto s=header()+part(jpeg,bad);
        assert(!p.feed(reinterpret_cast<const uint8_t*>(s.data()),s.size()) && r.frames.empty());
    }
    for(const auto& bad : {std::string("-1"),std::string("42x"),std::string("18446744073709551616")}) {
        p.reset(); auto s=header()+part(jpeg,"6",bad); assert(!p.feed(reinterpret_cast<const uint8_t*>(s.data()),s.size()));
    }
    p.reset(); r={}; auto broken=header()+part("broken"); assert(!p.feed(reinterpret_cast<const uint8_t*>(broken.data()),broken.size())); assert(r.frames.empty());
    p.reset(); r={}; auto truncated=header()+part().substr(0,part().size()-4); feedFragmented(p,truncated,1); assert(!p.finish() && r.frames.empty());
    p.reset(); r={}; feedFragmented(p,header()+part(),3); assert(r.frames.size()==1); // reconnect discards partial state
    p.reset(); auto hugeHeader=header()+"--frame\r\nX-Large: "+std::string(600,'a')+"\r\n"; assert(!p.feed(reinterpret_cast<const uint8_t*>(hugeHeader.data()),hugeHeader.size()));
    p.reset(); auto badHttp=std::string("HTTP/1.1 401 Unauthorized\r\n\r\n"); assert(!p.feed(reinterpret_cast<const uint8_t*>(badHttp.data()),badHttp.size()));
    p.reset(); auto badChunk=header(true)+"nope\r\n"; assert(!p.feed(reinterpret_cast<const uint8_t*>(badChunk.data()),badChunk.size()));
    p.reset(); auto duplicate=header()+"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: 6\r\nContent-Length: 6\r\nX-Sequence: 1\r\n\r\n"; assert(!p.feed(reinterpret_cast<const uint8_t*>(duplicate.data()),duplicate.size()));
    p.reset(); auto noSequence=header()+"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: 6\r\n\r\n"; assert(!p.feed(reinterpret_cast<const uint8_t*>(noSequence.data()),noSequence.size()));
    std::string maximum(kMaxJpegBytes,'x'); maximum[0]='\xff';maximum[1]='\xd8';maximum[maximum.size()-2]='\xff';maximum.back()='\xd9';
    p.reset();r={};feedFragmented(p,header()+part(maximum,"131072"),111);assert(r.frames.size()==1 && r.frames[0].size()==131072);
    // 5 FPS, every 2048-byte read crosses a frame boundary but ends inside JPEG data.
    // Losing the completed-frame transition must not accumulate time across frames.
    std::string timedJpeg(1970,'x');timedJpeg[0]='\xff';timedJpeg[1]='\xd8';timedJpeg[1968]='\xff';timedJpeg[1969]='\xd9';
    std::string timedBody;
    for(unsigned sequence=100;sequence<117;++sequence) {
        auto frame=part(timedJpeg,"1970",std::to_string(sequence));assert(frame.size()==2048);timedBody+=frame;
    }
    p.reset();r={};FrameDeadline deadline;
    feedFragmented(p,header()+timedBody.substr(0,1024),1024);
    assert(p.inFrame() && r.frames.empty() && !deadline.expired(p.inFrame(),r.frames.size(),100));
    for(size_t i=0;i<15;++i) {
        feedFragmented(p,timedBody.substr(1024+i*2048,2048),2048);
        assert(p.inFrame() && r.frames.size()==i+1);
        assert(!deadline.expired(p.inFrame(),r.frames.size(),300+static_cast<uint32_t>(i)*200));
    }
    // Current frame began at t=3100. Dribbling bytes cannot postpone its absolute deadline.
    feedFragmented(p,timedBody.substr(1024+15*2048,1),1);
    assert(!deadline.expired(p.inFrame(),r.frames.size(),4100));
    assert(!deadline.expired(p.inFrame(),r.frames.size(),5099));
    assert(deadline.expired(p.inFrame(),r.frames.size(),5100));
    // Wrapping millis and exact 60 s / 10 s / 1 s boundaries must not drift.
    SessionClock clock; clock.start(100); assert(!clock.expired(60099)); assert(clock.expired(60100));
    clock.renewed(100); assert(!clock.renewDue(10099)); assert(clock.renewDue(10100));
    assert(clock.stale(100)); clock.frame(500); assert(!clock.stale(1499)); assert(clock.stale(1500));
    clock.start(0xfffffff0u);assert(!clock.expired(0xfffffff0u+59999u));assert(clock.expired(0xfffffff0u+60000u));
    clock.extend(200); assert(!clock.expired(60199)); assert(clock.expired(60200));
    clock.stop(); assert(!clock.active());
    SessionState state;assert(state.start(100));const uint32_t first=state.generation;
    assert(!state.start(200) && state.generation==first); // duplicate START preserves deadline/request epoch
    assert(state.adopt(first,"session-old",300));state.stop();const uint32_t stopped=state.generation;state.stop();assert(state.generation==stopped);
    assert(std::string(state.retired)=="session-old" && !*state.id);
    assert(state.start(500));const uint32_t second=state.generation;
    assert(!state.adopt(first,"late-old",600) && !*state.id);
    assert(state.adopt(second,"session-new",600));assert(!state.forget(first));
    assert(std::string(state.id)=="session-new" && !state.clock.expired(60499));
    assert(state.forget(second) && !*state.id && state.clock.expired(60500)); // reconnect must not extend viewing
    assert(!state.adopt(second,"session-new",700)); // expired connection cannot revive
    for(size_t i=0;i<16;++i)assert(storage[i]==0xa5 && storage[kMaxJpegBytes+16+i]==0xa5);
    std::puts("PASS: MJPEG arbitrary fragments/chunks, limits, corruption, reconnect, session deadlines, cross-frame watchdog");
}
