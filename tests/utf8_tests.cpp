#include <cstdio>
#include <cstdlib>
#define assert(c) do { if(!(c)) { std::fprintf(stderr,"UTF8 check failed at %d\n",__LINE__);std::exit(2); } } while(0)
#include <string>
#include "printsphere/utf8.hpp"
int main() {
 using namespace printsphere;
 for (const std::string value : {u8"Geh\u00e4use",u8"Stra\u00dfe",u8"\u20ac\u2013\U0001f600",u8"\u00d8\u00b5\u00d7\u010c"}) {
  for (size_t n=0;n<=value.size()+1;++n) {
   auto part=utf8_truncate_bytes(value,n);
   assert(part.size()<=n && value.compare(0,part.size(),part)==0);
   if(part.size()<value.size()) assert((static_cast<unsigned char>(value[part.size()])&0xC0)!=0x80);
   char buffer[64]; utf8_copy_c_str(buffer,n+1,value);
   assert(std::string(buffer)==part);
  }
 }
 assert(utf8_truncate_chars(u8"Gr\u00f6\u00dfe\U0001f600",5)==u8"Gr\u00f6\u00dfe");
 assert(utf8_truncate_chars(u8"Gr\u00f6\u00dfe\U0001f600",6)==u8"Gr\u00f6\u00dfe\U0001f600");
 char sentinel='x';utf8_copy_c_str(&sentinel,0,u8"?");assert(sentinel=='x');
 utf8_copy_c_str(nullptr,10,"ignored");
}
