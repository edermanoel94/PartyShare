#include <cstddef>
#include <string>

#include <dv/models/chat.hpp>

namespace dv::models {
namespace {

/// Whitespace, checked without <cctype> so that a byte above 127 cannot be
/// misread as one. The continuation bytes of a UTF-8 character are never any
/// of these, which is what makes trimming byte by byte safe on text that is
/// not ASCII.
[[nodiscard]] bool is_space(char character) noexcept {
  return character == ' ' || character == '\t' || character == '\n' || character == '\r' ||
         character == '\v' || character == '\f';
}

}  // namespace

std::string trim_chat_text(const std::string& text) {
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

bool is_valid_chat_text(const std::string& text) {
  // Both checks are made against the trimmed text, because that is what gets
  // stored. A message accepted at its full length and then stored trimmed
  // would be one whose limit depends on how much whitespace it was padded
  // with.
  const std::string trimmed = trim_chat_text(text);
  return !trimmed.empty() && trimmed.size() <= kMaxChatTextBytes;
}

}  // namespace dv::models
