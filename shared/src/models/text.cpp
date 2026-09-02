#include <cstddef>
#include <string>

#include <dv/models/text.hpp>

namespace dv::models {
namespace {

[[nodiscard]] bool is_space(char character) noexcept {
  return character == ' ' || character == '\t' || character == '\n' || character == '\r' ||
         character == '\v' || character == '\f';
}

}  // namespace

std::string trim_text(const std::string& text) {
  std::size_t begin = 0;
  while (begin < text.size() && is_space(text[begin])) {
    ++begin;
  }
  std::size_t end = text.size();
  while (end > begin && is_space(text[end - 1])) {
    --end;
  }
  return text.substr(begin, end - begin);
}

}  // namespace dv::models
