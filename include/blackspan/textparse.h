#ifndef BLACKSPAN_TEXTPARSE_H
#define BLACKSPAN_TEXTPARSE_H

#include <cstdint>
#include <string>
#include <vector>

namespace blackspan {

bool parse_uint_token(const std::string& s, uint64_t& out);

bool parse_double_token(const std::string& s, double& out);

std::vector<std::string> tokenize(const std::string& line);

struct SourceLine {
  size_t number = 0;
  std::string text;
};

std::vector<SourceLine> source_lines(const std::string& text);

}

#endif
