#ifdef NDEBUG
#undef NDEBUG
#endif
#include "WalkMap.h"
#include <cassert>
#include <cstring>
#include <vector>
using namespace navigator;
struct Source:WalkMapByteSource{
 std::vector<uint8_t>b; uint32_t size()const override{return b.size();}
 uint32_t read(uint32_t o,uint8_t*p,uint32_t n)override{if(o>b.size()||n>b.size()-o)return 0;std::memcpy(p,b.data()+o,n);return n;}
};
void put(Source&s,int o,uint32_t v,int n=4){for(int i=0;i<n;i++)s.b[o+i]=v>>(8*i);}
uint32_t crc(const uint8_t*p,int n){uint32_t c=~0u;while(n--){c^=*p++;for(int i=0;i<8;i++)c=(c>>1)^((c&1)?0xedb88320u:0);}return ~c;}
void checksums(Source&s){put(s,56,crc(s.b.data()+60,64));put(s,36,crc(s.b.data()+48,12));put(s,44,crc(s.b.data(),44));}
int main(){
 Source s;s.b.resize(124);std::memcpy(s.b.data(),"X3WM",4);put(s,4,2,2);put(s,6,48,2);put(s,8,124);
 put(s,12,520000000);put(s,16,40000000);put(s,20,100000);put(s,24,1,2);put(s,26,1,2);put(s,28,48);put(s,32,60);
 put(s,48,60);put(s,52,1);put(s,60,520001000);put(s,64,40001000);put(s,68,520001000);put(s,72,40001000);
 s.b[76]=7;s.b[78]=6;std::memcpy(s.b.data()+80,"STREET",6);checksums(s);
 WalkMap m;assert(m.open(s)==WalkMapStatus::Ok);
 int count=0;auto capture=[](void*p,const WalkMapEdge&e){++*static_cast<int*>(p);assert(e.kind==7);};
 GeoBounds bounds{520000000,40000000,520100000,40100000};
 assert(m.visit(s,bounds,capture,&count)==WalkMapStatus::Ok);assert(count==1);
 s.b[123]=1;checksums(s);assert(m.open(s)==WalkMapStatus::Ok);
 assert(m.visit(s,bounds,capture,&count)==WalkMapStatus::InvalidData);
 s.b[123]=0;s.b[78]=45;checksums(s);assert(m.open(s)==WalkMapStatus::Ok);
 assert(m.visit(s,bounds,capture,&count)==WalkMapStatus::InvalidData);
 s.b[78]=6;s.b[76]=8;checksums(s);assert(m.open(s)==WalkMapStatus::Ok);
 assert(m.visit(s,bounds,capture,&count)==WalkMapStatus::InvalidData);
}
