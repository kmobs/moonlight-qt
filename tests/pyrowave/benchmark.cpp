// Optional benchmark of independent LZ4 detail groups. Real GPU encoder input;
// timed CPU framing/compression/validation and modeled gigabit wire time are
// reported separately. No capture, GPU decode, rendering or live-link claims.
#include <vulkan/vulkan.h>
#include <pyrowave.h>
#include "pyrowavecompression.h"
#include "../../app/streaming/video/pyrowave/pyrowaveframing.h"
#include "pyrowave_policy.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <numeric>
#include <string>
#include <vector>

using Bytes = std::vector<uint8_t>;
using Clock = std::chrono::steady_clock;
struct Frame { std::map<uint32_t, Bytes> records; Bytes all; Bytes header; };
static uint32_t rng(uint32_t &s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
static double us(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::micro>(b-a).count(); }
static double percentile(std::vector<double> v, double q) { std::sort(v.begin(),v.end()); return v[size_t((v.size()-1)*q)]; }

static void setSequence(Bytes& records, unsigned sequence) {
    for (size_t offset=0; offset<records.size();) {
        uint32_t word=0;std::memcpy(&word,records.data()+offset,4);
        const size_t size=offset==0?8:((word>>16)&0xfffu)*4;
        word=(word&~(7u<<28))|((sequence&7u)<<28);
        std::memcpy(records.data()+offset,&word,4);
        offset+=size;
    }
}

static Frame encode(pyrowave_encoder encoder, int w, int h, int pattern, int frame, size_t budget) {
    std::vector<Bytes> planes(3,Bytes(size_t(w)*h));
    uint32_t state = pattern == 3 ? 0x917ac993u + frame * 317u : 0x917ac993u;
    for (int p = 0; p < 3; ++p) for(int y = 0; y < h; ++y) for(int x = 0; x < w; ++x) {
        const auto i=size_t(y)*w+x;
        if (pattern == 0 || pattern == 1) {
            int shift=pattern==1?frame:0;
            int value=p==0?((x*3+y*2+shift*17)&255):96+((p==1?x+shift*5:y*2+shift*3)%64);
            if(p==0&&((x/64+y/64+shift)%7)==0)value=235;
            planes[p][i]=uint8_t(std::clamp(value,16,235));
        } else {
            planes[p][i]=uint8_t(16+rng(state)%220);
            // Pattern 2 is a static detailed image with a small moving object.
            if(pattern==2 && p==0 && x>=w/2+frame*8 && x<w/2+frame*8+64 && y>=h/2 && y<h/2+64)
                planes[p][i]=235;
        }
    }
    pyrowave_cpu_buffer input = {};
    input.width=w; input.height=h; input.format=PYROWAVE_CPU_BUFFER_FORMAT_YUV444P;
    for(int p=0;p<3;++p) {input.data[p]=planes[p].data();input.row_stride_in_bytes[p]=w;input.plane_size_in_bytes[p]=planes[p].size();}
    pyrowave_rate_control rate={budget};
    if(pyrowave_encoder_encode_cpu_synchronous(encoder,&input,&rate)!=PYROWAVE_SUCCESS)std::exit(5);
    size_t count=0;
    if(pyrowave_encoder_compute_num_packets(encoder,1376,&count)!=PYROWAVE_SUCCESS)std::exit(6);
    std::vector<pyrowave_packet> packets(count);
    Bytes stream(budget+1024*1024);
    size_t written=0;
    if(pyrowave_encoder_packetize(encoder,packets.data(),1376,&written,stream.data(),stream.size())!=PYROWAVE_SUCCESS)std::exit(7);
    Frame out;
    for(size_t pi=0;pi<written;++pi)for(size_t pos=packets[pi].offset;pos<packets[pi].offset+packets[pi].size;) {
        uint32_t w0=0,w1=0;std::memcpy(&w0,stream.data()+pos,4);std::memcpy(&w1,stream.data()+pos+4,4);
        const bool extended=(w0&0x80000000u)!=0;
        size_t n=extended?8:((w0>>16)&0xfffu)*4;
        if(n<8||pos+n>stream.size())std::exit(8);
        if(!extended) {
            Bytes rec(stream.begin()+pos,stream.begin()+pos+n);
            w0&=~(7u<<28);std::memcpy(rec.data(),&w0,4);
            out.records[w1>>8]=rec;
            out.all.insert(out.all.end(),rec.begin(),rec.end());
        } else if(out.header.empty()) {
            out.header.assign(stream.begin()+pos,stream.begin()+pos+8);
            w0&=~(7u<<28);std::memcpy(out.header.data(),&w0,4);
        }
        pos+=n;
    }
    return out;
}

struct WireInfo {size_t shards=0,parity=0,ethernetBytes=0;double ms=0;};
static WireInfo wireInfo(size_t bytes,size_t critical) {
    constexpr size_t shard=1376,wirePacket=1392+16+38+20+8;
    WireInfo result;
    result.shards=(bytes+8+shard-1)/shard;
    size_t criticalShards=std::min(result.shards,(critical+8+shard-1)/shard);
    auto plan=pyrowave::policy::plan_fec_blocks(result.shards,criticalShards,20,2);
    for(auto block:plan)result.parity+=pyrowave::policy::parity_shards(block.data_shards,block.fec_percentage,2);
    result.ethernetBytes=(result.shards+result.parity)*wirePacket;
    result.ms=double(result.ethernetBytes)*8/1e6;
    return result;
}

int main(int argc,char**argv) {
    if(argc>1 && std::strcmp(argv[1],"--help")==0) {
        std::puts("Usage: pyrowave_compression_benchmark [width height bits_per_pixel timed_frames]\nDefaults: 3840 2160 1.6 300. Requires a supported Vulkan GPU.");
        return 0;
    }
    int w=argc>1?std::atoi(argv[1]):3840,h=argc>2?std::atoi(argv[2]):2160;
    double bpp=argc>3?std::atof(argv[3]):1.6;
    int frames=argc>4?std::atoi(argv[4]):300;
    if(argc>5 || w<1 || w>16384 || h<1 || h>16384 || !std::isfinite(bpp) || bpp<=0 || frames<1 || frames>100000) {
        std::fputs("Invalid dimensions, bitrate, frame count or extra arguments. Use --help.\n",stderr);
        return 2;
    }
    const double imageBudget=double(w)*h*bpp/8;
    if(!std::isfinite(imageBudget) || imageBudget<8 || imageBudget>16*1024*1024-72) {
        std::fputs("Encoded image budget must fit the 16 MiB transport cap.\n",stderr);
        return 2;
    }
    const size_t budget=size_t(imageBudget);
    pyrowave_device device=nullptr;if(pyrowave_create_default_device(&device)!=PYROWAVE_SUCCESS)return 3;
    pyrowave_encoder_create_info info={};info.device=device;info.width=w;info.height=h;info.chroma=PYROWAVE_CHROMA_SUBSAMPLING_444;
    pyrowave_encoder encoder=nullptr;if(pyrowave_encoder_create(&info,&encoder)!=PYROWAVE_SUCCESS)return 4;
    const char *names[]={"static-gradient","moving-gradient","static-noise-small-object","changing-noise"};
    for(int pattern=0;pattern<4;++pattern) {
        std::vector<Bytes> inputs, expectedRecords;
        std::vector<size_t> nativeWire,nativeCritical;
        std::vector<double> nativeFrameUs;
        for(int i=0;i<8;++i) {
            Frame frame=encode(encoder,w,h,pattern,i,budget);
            Bytes input=frame.header;input.insert(input.end(),frame.all.begin(),frame.all.end());setSequence(input,unsigned(i));inputs.push_back(input);
            Bytes framed;
            auto t0=Clock::now();
            auto stats=pyrowave::policy::write_record_frame(input,1376,framed,16*1024*1024);
            auto t1=Clock::now();
            if(!stats)return 5;
            PyroWaveFraming::Frame nativeParsed;
            std::string nativeError;
            if(!PyroWaveFraming::parse(framed.data(),framed.size(),{},0,{w,h,true},nativeParsed,nativeError)) {
                std::fprintf(stderr,"native framing failed: %s\n",nativeError.c_str());return 5;
            }
            Bytes expected;
            for(const auto& span:nativeParsed.spans)
                expected.insert(expected.end(),framed.begin()+span.offset,framed.begin()+span.offset+span.size);
            expectedRecords.push_back(std::move(expected));
            nativeWire.push_back(framed.size());nativeCritical.push_back(stats->critical_bytes);nativeFrameUs.push_back(us(t0,t1));
        }
        PyroWaveCompression::Encoder compressor;
        PyroWaveCompression::EncodedFrame encoded;
        PyroWaveFraming::Frame decoded;
        std::string error;
        std::vector<double> sendUs,receiveUs,serialUs,modeledReadyMs,hostFrameUs,shardMapUs,hostUs;
        std::vector<size_t> nativeBytes,wireBytes,criticalBytes,netBytes;
        size_t compressedGroups=0;
        double nativeMs=0;
        Bytes nativeFramed;
        for(int i=-32;i<frames;++i) {
            size_t index=size_t(i+32)%inputs.size();const auto &input=inputs[index];
            auto t0=Clock::now();
            nativeFramed.clear();
            auto frameStats=pyrowave::policy::write_record_frame(input,1376,nativeFramed,16*1024*1024);
            if(!frameStats)return 5;
            auto ta=Clock::now();
            bool sent=compressor.encode(nativeFramed.data(),nativeFramed.size(),frameStats->critical_bytes,1376,encoded,error);
            auto tb=Clock::now();
            const auto& shardMap=encoded.recordStartShards;
            if(shardMap.empty()) return 6;
            auto t1=Clock::now();
            if(!sent){std::fprintf(stderr,"send failed: %s\n",error.c_str());return 6;}
            bool received=PyroWaveFraming::parse(encoded.bytes.data(),encoded.bytes.size(),{},0,{w,h,true},decoded,error,true);
            auto t2=Clock::now();
            if(!received){std::fprintf(stderr,"receive failed: %s\n",error.c_str());return 7;}
            Bytes reconstructed;
            for(const auto& span:decoded.spans) {
                const auto& src=span.expanded?decoded.expanded:encoded.bytes;
                reconstructed.insert(reconstructed.end(),src.begin()+span.offset,src.begin()+span.offset+span.size);
            }
            if(reconstructed!=expectedRecords[index]){std::fprintf(stderr,"reconstruction differs for %s\n",names[pattern]);return 8;}
            if(i<0)continue;
            auto wi=wireInfo(encoded.bytes.size(),encoded.criticalBytes);
            auto normal=wireInfo(nativeWire[index],nativeCritical[index]);
            sendUs.push_back(us(ta,t1));receiveUs.push_back(us(t1,t2));serialUs.push_back(us(t0,t2));hostFrameUs.push_back(us(t0,ta));shardMapUs.push_back(us(tb,t1));hostUs.push_back(us(t0,t1));
            modeledReadyMs.push_back(us(t0,t2)/1000+wi.ms);
            nativeMs+=normal.ms;
            nativeBytes.push_back(nativeWire[index]);
            wireBytes.push_back(encoded.bytes.size());criticalBytes.push_back(encoded.criticalBytes);netBytes.push_back(wi.ethernetBytes);
            compressedGroups+=encoded.compressedGroups;
        }
        auto mean=[](const auto &v){return std::accumulate(v.begin(),v.end(),0.0)/v.size();};
        std::printf("PIPELINE %s %dx%d %.2fbpp frames=%d native_bytes_mean=%.1f compressed_bytes_mean=%.1f native_wire_ms=%.3f compressed_wire_ms=%.3f compression_us_mean=%.1f p95=%.1f expand_validate_us_mean=%.1f p95=%.1f compressed_groups=%zu\n",names[pattern],w,h,bpp,frames,mean(nativeBytes),mean(wireBytes),nativeMs/frames,mean(netBytes)*8/1e6,mean(sendUs),percentile(sendUs,.95),mean(receiveUs),percentile(receiveUs,.95),compressedGroups);
        std::printf("HOST_STAGE %s cpu_us_mean=%.1f p50=%.1f p95=%.1f max=%.1f\n",names[pattern],mean(hostUs),percentile(hostUs,.5),percentile(hostUs,.95),percentile(hostUs,1));
    }
    pyrowave_encoder_destroy(encoder);pyrowave_device_destroy(device);
}
