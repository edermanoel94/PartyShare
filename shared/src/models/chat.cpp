#include <string>

#include <dv/models/chat.hpp>
#include <dv/models/text.hpp>

namespace dv::models {

std::string trim_chat_text(const std::string& text) {
  return trim_text(text);
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
