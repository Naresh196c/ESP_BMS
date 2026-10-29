#include "../bms_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <vector>
#include <random>
#include <string.h>
static std::vector<uint8_t> frame(int soc=42) {
  std::vector<uint8_t> p(54,0);
  p[0]=0xEA;p[1]=0xD1;p[2]=1;p[3]=50;p[4]=0xFF;p[5]=4;
  p[7]=soc;p[9]=4;p[10]=0xD2;p[47]=0x14;p[48]=0x93;p[53]=0xF5;
  for(size_t i=3;i<p.size()-2;++i)p[52]^=p[i];
  return p;
}
static void expectReading(const FF04Receiver& r) {
  assert(r.ready);assert(r.soc==42);assert(r.cycles==1234);
  assert(r.voltage>52.669f&&r.voltage<52.671f);
}
int main(){
  auto valid=frame();
  for(size_t split=0;split<=valid.size();++split){
    FF04Receiver r;r.feed(valid.data(),split);r.feed(valid.data()+split,valid.size()-split);expectReading(r);
  }
  std::mt19937 rng(20261016);
  for(int run=0;run<500;++run){
    FF04Receiver r;size_t offset=0;
    while(offset<valid.size()){size_t count=1+rng()%20;if(count>valid.size()-offset)count=valid.size()-offset;r.feed(valid.data()+offset,count);offset+=count;}
    expectReading(r);
  }
  FF04Receiver r;
  uint8_t garbage[]={0x00,0xEA,0xEA,0xD1,0x02,0xEA,0xD1,0x01,0xFF};
  r.feed(garbage,sizeof(garbage));r.feed(valid.data(),valid.size());expectReading(r);assert(r.rejected>0);
  auto corrupt=valid;corrupt[10]^=1;r.reset();r.feed(corrupt.data(),corrupt.size());assert(!r.ready);assert(!strcmp(r.lastReject,"Checksum mismatch"));
  r.feed(valid.data(),valid.size());expectReading(r);
  auto badSoc=frame(101);r.reset();r.feed(badSoc.data(),badSoc.size());assert(!r.ready);assert(!strcmp(r.lastReject,"SOC outside 0..100"));
  auto wrong=valid;wrong[5]=5;r.reset();r.feed(wrong.data(),wrong.size());assert(!r.ready);assert(!strcmp(r.lastReject,"Not an FF04 response"));
  auto trailer=valid;trailer.back()=0;r.reset();r.feed(trailer.data(),trailer.size());assert(!r.ready);assert(!strcmp(r.lastReject,"Trailer mismatch"));
  r.reset();r.feed(valid.data(),20);assert(!r.ready&&r.used==20&&r.expected==54);
  r.reset();std::vector<uint8_t> fuzz(100000);for(auto& b:fuzz)b=rng()%256;
  for(uint8_t b:fuzz){r.feed(&b,1);assert(r.used<=sizeof(r.buffer));}
  r.reset();r.feed(valid.data(),valid.size());expectReading(r);
  // A late OPEN must not allow a new peer until physical disconnect.
  BmsLinkLifecycle link;assert(link.begin());assert(!link.begin());link.abort();assert(!link.idle());
  link.onConnected(7);assert(!link.begin());link.onOpen(true,7);assert(!link.idle()&&link.abandoned);
  link.onDisconnected();assert(link.idle());assert(link.begin()&&!link.abandoned);
  // OPEN failure ends a pending operation without tearing down the stack.
  link.onOpen(false,0xFFFF);assert(link.idle());assert(link.begin());
  // DISCONNECT can precede the final OPEN failure event: still no reuse.
  link.onConnected(9);link.abort();link.onDisconnected();assert(!link.idle());
  link.onOpen(false,9);assert(link.idle());
  puts("PASS: all header splits, fragmented notifications, noise/resync, checksum/SOC/command/trailer failures, partial frames, 100KB fuzz, and late-OPEN/disconnect lifecycle.");
}
