#include "blackspan/textparse.h"

#include <cerrno>
#include <cstdlib>
#include <sstream>

namespace blackspan {

bool parse_uint_token(const std::string& s, uint64_t& out) {
  if (s.empty()) return false;
  errno = 0;
  char* end = nullptr;
  unsigned long long v = std::strtoull(s.c_str(), &end, 0);
  if (end == s.c_str() || *end != '\0') return false;
  out = v;
  return true;
}

bool parse_double_token(const std::string& s, double& out) {
  if (s.empty()) return false;
  errno = 0;
  char* end = nullptr;
  double v = std::strtod(s.c_str(), &end);
  if (end == s.c_str()) return false;
  while (*end == ' ' || *end == '\t') ++end;
  if (*end != '\0') return false;
  out = v;
  return true;
}

std::vector<std::string> tokenize(const std::string& line) {
  std::vector<std::string> tok;
  std::istringstream iss(line);
  std::string t;
  while (iss >> t) tok.push_back(t);
  return tok;
}

std::vector<SourceLine> source_lines(const std::string& text) {
  std::vector<SourceLine> out;
  size_t line_no = 0;
  size_t start = 0;
  while (start <= text.size()) {
    ++line_no;
    const size_t nl = text.find('\n', start);
    const size_t end = (nl == std::string::npos) ? text.size() : nl;
    std::string line = text.substr(start, end - start);
    start = (nl == std::string::npos) ? text.size() + 1 : nl + 1;
    const size_t hash = line.find('#');
    if (hash != std::string::npos) line = line.substr(0, hash);
    out.push_back(SourceLine{line_no, std::move(line)});
  }
  return out;
}

}
