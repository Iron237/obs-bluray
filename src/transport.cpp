#include "transport.h"
namespace sd {
static constexpr std::uint64_t mask33=(std::uint64_t(1)<<33)-1;
static void shiftPts(std::uint8_t *p,std::int64_t offset) {
    const auto value=(std::uint64_t((p[0]>>1)&7)<<30)|(std::uint64_t(p[1])<<22)|(std::uint64_t(p[2]>>1)<<15)|(std::uint64_t(p[3])<<7)|(p[4]>>1);
    const auto v=std::uint64_t(std::int64_t(value)+offset)&mask33;
    p[0]=std::uint8_t((p[0]&0xf1)|((v>>29)&0x0e));p[1]=std::uint8_t(v>>22);p[2]=std::uint8_t(((v>>14)&0xfe)|1);p[3]=std::uint8_t(v>>7);p[4]=std::uint8_t(((v<<1)&0xfe)|1);
}
void rebaseM2ts(std::uint8_t *data,std::size_t size,std::int64_t offset) {
    for(std::size_t pos=0;pos+192<=size;pos+=192) {
        auto *ts=data+pos+4;if(ts[0]!=0x47||(ts[1]&0x80))continue;
        const int control=(ts[3]>>4)&3;int payload=4;
        if(control&2) {
            const int length=ts[4];if(length>183)continue;
            if(length>=7&&(ts[5]&0x10)) {
                auto *p=ts+6;const auto base=(std::uint64_t(p[0])<<25)|(std::uint64_t(p[1])<<17)|(std::uint64_t(p[2])<<9)|(std::uint64_t(p[3])<<1)|(p[4]>>7);
                const auto v=std::uint64_t(std::int64_t(base)+offset)&mask33;
                p[0]=std::uint8_t(v>>25);p[1]=std::uint8_t(v>>17);p[2]=std::uint8_t(v>>9);p[3]=std::uint8_t(v>>1);p[4]=std::uint8_t((p[4]&0x7f)|((v&1)<<7));
            }
            payload+=length+1;
        }
        if(!(control&1)||!(ts[1]&0x40)||payload+14>188)continue;
        auto *pes=ts+payload;if(pes[0]||pes[1]||pes[2]!=1)continue;
        // MPEG-2 PES optional header; exclude program maps, padding and PSI.
        if((pes[6]&0xc0)!=0x80)continue;
        const int flags=pes[7]>>6;
        if((flags==2||flags==3)&&pes[8]>=5)shiftPts(pes+9,offset);
        if(flags==3&&pes[8]>=10&&payload+19<=188)shiftPts(pes+14,offset);
    }
}
}
