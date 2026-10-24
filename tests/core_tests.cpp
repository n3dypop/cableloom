#include "../driver/core.hpp"
#include "../include/protocol.h"
#include <cstdio>
#include <cstdlib>
#include <memory>
using namespace cableloom;
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)
int main() {
    auto q = std::make_unique<DelayLine>();
    q->reset(96);
    for (int32_t i = 0; i < 200000; ++i) {
        Frame input{static_cast<int32_t>(static_cast<uint32_t>(i) * 2654435761u),
                    static_cast<int32_t>(~(static_cast<uint32_t>(i) * 2654435761u))};
        REQUIRE(!q->push(input));
        Frame output{}; REQUIRE(q->pull(output));
        if (i < 96) REQUIRE(output.left == 0 && output.right == 0);
        else {
            uint32_t old = static_cast<uint32_t>(i - 96) * 2654435761u;
            REQUIRE(output.left == static_cast<int32_t>(old));
            REQUIRE(output.right == static_cast<int32_t>(~old));
        }
    }
    q->reset(0);
    for (uint32_t i = 0; i < DelayLine::capacity + 37; ++i)
        REQUIRE(q->push({static_cast<int32_t>(i), 0}) == (i >= DelayLine::capacity));
    for (uint32_t i = 37; i < DelayLine::capacity + 37; ++i) {
        Frame f{}; REQUIRE(q->pull(f)); REQUIRE(f.left == static_cast<int32_t>(i));
    }
    Frame missing{123,456}; REQUIRE(!q->pull(missing)); REQUIRE(missing.left == 0 && missing.right == 0);
    q->push({123,456}); q->reset(48);
    for (int i=0;i<48;++i) { Frame f{}; REQUIRE(q->pull(f)); REQUIRE(f.left==0&&f.right==0); }
    REQUIRE(!q->pull(missing));
    std::unique_ptr<DelayLine> cables[8];
    for (int i=0;i<8;++i) { cables[i]=std::make_unique<DelayLine>(); cables[i]->reset(48+i); }
    for (int32_t t=0;t<40000;++t) for (int k=0;k<8;++k) {
        int i=(t+k)%8;
        REQUIRE(!cables[i]->push({t+1, i+100}));
        Frame f{}; REQUIRE(cables[i]->pull(f));
        if(t<48+i) REQUIRE(f.left==0&&f.right==0);
        else REQUIRE(f.left==t+1-(48+i)&&f.right==i+100);
    }
    // Packed PCM sign extension and byte-perfect roundtrips, including extrema.
    for (uint32_t bits : {16u,24u,32u}) {
        const uint32_t width=bits/8;
        uint8_t bytes[8]{};
        for (uint32_t i=0;i<100000;++i) {
            Frame source{static_cast<int32_t>(i*2654435761u),static_cast<int32_t>(~(i*2246822519u))};
            encode(bytes,bits,source);
            uint8_t original[8]{};for(uint32_t b=0;b<width*2;++b)original[b]=bytes[b];
            auto decoded=decode(bytes,bits);encode(bytes,bits,decoded);
            for(uint32_t b=0;b<width*2;++b)REQUIRE(bytes[b]==original[b]);
            uint32_t mask=bits==32?0xffffffffu:((1u<<bits)-1);
            REQUIRE((static_cast<uint32_t>(decoded.left)&mask)==(static_cast<uint32_t>(source.left)&mask));
            if(bits<32)REQUIRE((decoded.left<0)==((original[width-1]&0x80)!=0));
        }
        Frame edges[]={{0,0},{-1,-1},{INT32_MIN,INT32_MAX}};
        for(auto edge:edges){encode(bytes,bits,edge);auto decoded=decode(bytes,bits);encode(bytes,bits,decoded);}
    }
    FrameClock clock; constexpr uint64_t frequency=10000019;
    clock.reset(0,frequency); uint64_t now=0,total=0;
    for (uint64_t i=1;i<2000000;++i) { now+=731+(i%97); total+=clock.advance(now); }
    REQUIRE(total==(now/frequency)*48000+(now%frequency)*48000/frequency);
    clock.reset(0,frequency);
    REQUIRE(clock.advance(frequency*86400)==48000ull*86400);
    clock.reset(100,frequency); REQUIRE(clock.advance(99)==0);
    clock.reset(0,0); REQUIRE(clock.advance(123456)==0);
    std::puts("PASS: PCM16/24/32 packing, PCM32 exact delay, wraps, overflow, underrun, reset, 8-cable isolation, QPC drift and 24-hour accounting.");
}
