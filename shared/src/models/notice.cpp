#include <string>

#include <dv/models/notice.hpp>
#include <dv/models/text.hpp>

namespace dv::models {

std::string trim_notice_text(const std::string& text) {
  return trim_text(text);
}

bool is_valid_notice_text(const std::string& text) {
  // Checked against the trimmed text, because that is what gets stored. See
  // is_valid_chat_text, which makes the same choice for the same reason.
  const std::string trimmed = trim_notice_text(text);
  return !trimmed.empty() && trimmed.size() <= kMaxNoticeTextBytes;
}

}  // namespace dv::models
