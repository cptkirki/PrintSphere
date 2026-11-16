#pragma once
#include <string>
#include <string_view>
namespace printsphere {
inline std::string html_escape(std::string_view input) {
  std::string out; out.reserve(input.size());
  for (unsigned char c : input) {
    switch(c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case 39: out += "&#39;"; break;
      case 0: out += "&#xfffd;"; break;
      default: out += static_cast<char>(c);
    }
  }
  return out;
}
inline std::string json_escape(std::string_view input) {
  constexpr char hex[] = "0123456789abcdef";
  std::string out; out.reserve(input.size());
  for (size_t i=0;i<input.size();++i) {
    const auto c=static_cast<unsigned char>(input[i]);
    if(c==0x5c || c=='"') {
      out.push_back(char(0x5c)); out.push_back(char(c));
    } else if(c<0x20 || c=='<' || c=='>' || c=='&' || c==39) {
      out.push_back(char(0x5c)); out += "u00"; out += hex[c>>4]; out += hex[c&15];
    } else if(c==0xe2 && i+2<input.size() && static_cast<unsigned char>(input[i+1])==0x80 &&
              (static_cast<unsigned char>(input[i+2])==0xa8 || static_cast<unsigned char>(input[i+2])==0xa9)) {
      out.push_back(char(0x5c));
      out += static_cast<unsigned char>(input[i+2])==0xa8 ? "u2028" : "u2029"; i+=2;
    } else out += static_cast<char>(c);
  }
  return out;
}
inline std::string script_string_escape(std::string_view input) { return json_escape(input); }
}
