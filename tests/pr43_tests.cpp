#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include <string>
#include "cJSON.h"
#include "printsphere/web_escape.hpp"
#include "printsphere/wav_parser.hpp"
#include "printsphere/task_start.hpp"
#include "printsphere/bounded_io.hpp"
#define CHECK(c) do { if(!(c)) { std::fprintf(stderr,"PR43 check failed at %d: %s\n",__LINE__,#c);std::exit(2); } } while(0)
void put32(std::vector<uint8_t>& b,size_t n,uint32_t v){for(unsigned i=0;i<4;++i)b[n+i]=uint8_t(v>>(8*i));}
void put16(std::vector<uint8_t>& b,size_t n,uint16_t v){b[n]=uint8_t(v);b[n+1]=uint8_t(v>>8);}
std::vector<uint8_t> wave(){
 std::vector<uint8_t> b(44+640);
 std::memcpy(b.data(),"RIFF",4);put32(b,4,uint32_t(b.size()-8));std::memcpy(b.data()+8,"WAVEfmt ",8);
 put32(b,16,16);put16(b,20,1);put16(b,22,1);put32(b,24,16000);put32(b,28,32000);put16(b,32,2);put16(b,34,16);
 std::memcpy(b.data()+36,"data",4);put32(b,40,640);for(size_t i=44;i<b.size();i+=2)put16(b,i,1000);
 return b;
}
int main(){
 using namespace printsphere;
 CHECK(html_escape("&<>\"'")=="&amp;&lt;&gt;&quot;&#39;");
 const std::string attack="\"><img src=x onerror=alert(1)></script><script>alert(2)</script>";
 CHECK(html_escape(attack).find('<')==std::string::npos);
 CHECK(script_string_escape(attack).find('<')==std::string::npos);
 const std::string json="\""+json_escape(attack)+"\"";
 auto parsed=cJSON_Parse(json.c_str());CHECK(cJSON_IsString(parsed));CHECK(attack==parsed->valuestring);cJSON_Delete(parsed);
 for(char c=1;c<32;++c){const std::string raw(1,c),encoded="\""+json_escape(raw)+"\"";auto v=cJSON_Parse(encoded.c_str());CHECK(cJSON_IsString(v)&&std::string(v->valuestring)==raw);cJSON_Delete(v);}
 CHECK(json_escape(std::string(1,char(0)))=="\\u0000");
 CHECK(script_string_escape(u8"\u2028\u2029")=="\\u2028\\u2029");
 std::vector<int16_t> pcm;std::string error;auto good=wave();
 CHECK(parse_wav_pcm(good.data(),good.size(),pcm,error,320,96)&&pcm.size()==320&&pcm[0]==1000);
 for(size_t n=0;n<good.size();++n){auto b=good;if(n>=12)put32(b,4,uint32_t(n-8));CHECK(!parse_wav_pcm(b.data(),n,pcm,error,320,96));CHECK(pcm.empty());}
 for(size_t where : {size_t(16),size_t(40)}){auto b=good;put32(b,where,0xffffffff);CHECK(!parse_wav_pcm(b.data(),b.size(),pcm,error,320,96));}
 auto b=good;b.resize(b.size()+8);std::memcpy(b.data()+good.size(),"fmt ",4);put32(b,good.size()+4,16);put32(b,4,uint32_t(b.size()-8));CHECK(!parse_wav_pcm(b.data(),b.size(),pcm,error,320,96));
 b=good;put16(b,22,2);CHECK(!parse_wav_pcm(b.data(),b.size(),pcm,error,320,96));
 b=good;std::fill(b.begin()+44,b.end(),0);CHECK(!parse_wav_pcm(b.data(),b.size(),pcm,error,320,96));
 // Unknown padded chunks are skipped without shifting the PCM sample boundary.
 b=good;b.insert(b.begin()+12,10,0);std::memcpy(b.data()+12,"JUNK",4);put32(b,16,1);b[20]=7;put32(b,4,uint32_t(b.size()-8));CHECK(parse_wav_pcm(b.data(),b.size(),pcm,error,320,96)&&pcm[0]==1000);
 std::atomic<bool> started{false};std::mutex lock;int creates=0;
 CHECK(!start_task_once(started,lock,[&]{++creates;return false;}));CHECK(!started);
 std::vector<std::thread> workers;for(int i=0;i<8;++i)workers.emplace_back([&]{CHECK(start_task_once(started,lock,[&]{++creates;return true;}));});for(auto& t:workers)t.join();CHECK(creates==2&&started);
 int64_t clock=0;auto now=[&]{return clock;};auto pause=[&]{++clock;};auto retry=[](int v){return v==-2||v==-3;};
 int calls=0;CHECK(read_until([&]{++calls;return -2;},retry,now,pause,3)==-1&&calls==3);
 clock=0;calls=0;CHECK(read_until([&]{return ++calls==1?-3:4;},retry,now,pause,3)==4&&calls==2);
 clock=0;calls=0;CHECK(read_until([&]{++calls;return 0;},retry,now,pause,3)==0&&calls==1);
 clock=0;calls=0;std::string sent;std::vector<int> results{-2,2,-3,1,2};const std::string message="abcde";
 CHECK(write_all_until(reinterpret_cast<const uint8_t*>(message.data()),message.size(),[&](const uint8_t* bytes,size_t size){int n=results.at(calls++);if(n>0){CHECK(size>=size_t(n));sent.append(reinterpret_cast<const char*>(bytes),n);}return n;},retry,now,pause,10));CHECK(sent==message);
 clock=0;CHECK(!write_all_until(reinterpret_cast<const uint8_t*>(message.data()),message.size(),[](const uint8_t*,size_t){return 0;},retry,now,pause,10));
 clock=int64_t(UINT32_MAX)+100;calls=0;
 CHECK(read_until([&]{++calls;return -2;},retry,now,pause,clock+3)==-1&&calls==3);
 std::puts("PR43: HTML/inline-script escaping, malformed WAV boundaries, task retry/concurrency and bounded partial TLS I/O passed");
}
