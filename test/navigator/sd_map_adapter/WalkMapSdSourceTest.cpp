#ifdef NDEBUG
#undef NDEBUG
#endif
// Fake SD transport verifies adapter handle/seek ownership, not physical SD timing.
#include <cassert>
#include "WalkMapSdSource.h"
static bool cancelled=false;
static bool cancel(){return cancelled;}
int main(){
 navigator::WalkMapSdSource source;uint8_t b[256];
 assert(source.read(0,b,16)==0);
 assert(source.beginRead(cancel));assert(source.size()==2048);
 for(int i=0;i<8;++i)assert(source.read(i*256,b,256)==256);
 assert(fake.opens==1);assert(fake.seeks==0);
 assert(source.read(24,b,12)==12);assert(fake.seeks==1);
 cancelled=true;assert(source.read(36,b,12)==0);
 assert(source.endRead());assert(fake.closes==1);assert(source.size()==0);
 cancelled=false;assert(source.beginRead());assert(source.read(0,b,256)==256);
 assert(source.endRead());assert(fake.opens==2&&fake.closes==2);
 fake.ready=false;assert(!source.beginRead());
}
