#ifndef BLACKSPAN_JSON_WRITER_H
#define BLACKSPAN_JSON_WRITER_H

#include <cstddef>
#include <string>
#include <vector>

namespace blackspan {

std::string escape_json(const std::string& s);

class JsonWriter {
 public:
  void begin_object();
  void end_object();
  void begin_array();
  void end_array();

  void key(const std::string& k);

  void string_value(const std::string& v);
  void int_value(long long v);
  void uint_value(unsigned long long v);
  void double_value(double v);
  void bool_value(bool v);
  void null_value();

  const std::string& str() const { return out_; }

 private:

  struct Level {
    char kind;
    size_t count;
  };

  void before_value();

  std::string out_;
  std::vector<Level> stack_;
  bool pending_key_ = false;
};

}

#endif
