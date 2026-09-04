#pragma once
#include <cstdint>
#include <cstring>
#define O_RDONLY 0
struct FakeSDState { int opens=0, closes=0, seeks=0; bool ready=true; uint8_t data[2048]{}; };
inline FakeSDState fake;
class FsFile {
 bool open_=false; uint32_t pos=0;
 public:
 FsFile()=default;
 explicit FsFile(bool v):open_(v) {}
 explicit operator bool()const{return open_;}
 bool isFile()const{return open_;}
 uint64_t fileSize()const{return sizeof(fake.data);}
 bool close(){if(open_)++fake.closes;open_=false;return true;}
 bool seekSet(uint32_t p){++fake.seeks;pos=p;return p<sizeof(fake.data);}
 int read(uint8_t* p,uint32_t n){if(!open_||pos+n>sizeof(fake.data))return -1;std::memcpy(p,fake.data+pos,n);pos+=n;return n;}
};
struct SD {bool ready()const{return fake.ready;} FsFile open(const char*,int){++fake.opens;return FsFile(true);}};
inline SD SdMan;
