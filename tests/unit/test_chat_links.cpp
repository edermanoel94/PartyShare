// Finding the URLs in a chat line.
//
// The interesting half of these is what is *not* found. What comes out of
// find_links is handed to the system shell by ui::ChatView, and the text going
// in was typed by another participant, so every scheme that is not http or
// https staying plain text is a rule and not an omission.

#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "app/chat_links.hpp"

namespace {

using dv::client::app::find_links;
using dv::client::app::LinkSpan;

/// The links in `text`, as the substrings they cover.
///
/// Offsets are what the renderer needs and are unreadable in an expectation.
[[nodiscard]] std::vector<std::string> found(std::string_view text) {
  std::vector<std::string> links;
  for (const LinkSpan& span : find_links(text)) {
    links.emplace_back(text.substr(span.begin, span.end - span.begin));
  }
  return links;
}

TEST(ChatLinksTest, ALineWithNoUrlInItHasNoLinks) {
  EXPECT_TRUE(found("nothing to click here").empty());
  EXPECT_TRUE(found("").empty());
}

TEST(ChatLinksTest, AUrlOnItsOwnIsTheWholeLine) {
  EXPECT_EQ(found("https://example.com"), std::vector<std::string>{"https://example.com"});
}

TEST(ChatLinksTest, AUrlIsFoundInsideASentence) {
  EXPECT_EQ(found("look at https://example.com/a/b?c=d for the answer"),
            std::vector<std::string>{"https://example.com/a/b?c=d"});
}

TEST(ChatLinksTest, PlainHttpIsAlsoALink) {
  EXPECT_EQ(found("http://10.0.0.4:8080/health"),
            std::vector<std::string>{"http://10.0.0.4:8080/health"});
}

TEST(ChatLinksTest, TheSchemeIsMatchedWhateverItsCase) {
  EXPECT_EQ(found("HTTPS://Example.COM/x"), std::vector<std::string>{"HTTPS://Example.COM/x"});
}

TEST(ChatLinksTest, EveryUrlOnALineIsFound) {
  const std::vector<std::string> expected = {"http://one.example", "https://two.example/x"};
  EXPECT_EQ(found("http://one.example and then https://two.example/x"), expected);
}

TEST(ChatLinksTest, TheFullStopThatEndedTheSentenceIsNotPartOfTheUrl) {
  EXPECT_EQ(found("it is at https://example.com/page."),
            std::vector<std::string>{"https://example.com/page"});
  EXPECT_EQ(found("https://example.com/a, https://example.com/b!"),
            (std::vector<std::string>{"https://example.com/a", "https://example.com/b"}));
}

TEST(ChatLinksTest, ABracketTheUrlOpenedItselfIsKept) {
  // The Wikipedia case, and the reason the rule counts brackets rather than
  // trimming every closing one.
  EXPECT_EQ(found("https://example.com/a_(b)"),
            std::vector<std::string>{"https://example.com/a_(b)"});
}

TEST(ChatLinksTest, ABracketTheSentenceOpenedIsNotKept) {
  EXPECT_EQ(found("(see https://example.com/a)"),
            std::vector<std::string>{"https://example.com/a"});
}

TEST(ChatLinksTest, TheTailOfAWordIsNotAScheme) {
  // Without the boundary check, all three of these would produce a link
  // starting in the middle of a word somebody wrote.
  EXPECT_TRUE(found("xhttp://example.com").empty());
  EXPECT_TRUE(found("nothttps://example.com").empty());
  EXPECT_TRUE(found("a1http://example.com").empty());
}

TEST(ChatLinksTest, ASchemeWithNoHostIsNotALink) {
  // There would be nothing to open.
  EXPECT_TRUE(found("http://").empty());
  EXPECT_TRUE(found("https:// example.com").empty());
}

TEST(ChatLinksTest, NoOtherSchemeIsEverALink) {
  // The decision this file exists to pin down. Each of these is a way to turn
  // a line somebody typed into an action on the machine of whoever reads it,
  // and each of them stays text.
  EXPECT_TRUE(found("file:///C:/Windows/System32/cmd.exe").empty());
  EXPECT_TRUE(found("javascript:alert(1)").empty());
  EXPECT_TRUE(found("ftp://example.com/payload.exe").empty());
  EXPECT_TRUE(found("mailto:someone@example.com").empty());
  EXPECT_TRUE(found("smb://server/share").empty());
  EXPECT_TRUE(found("ms-msdt:/id PCWDiagnostic").empty());
}

TEST(ChatLinksTest, AccentedTextAroundAUrlSurvivesWhole) {
  // The offsets are bytes and the text is UTF-8. Nothing here looks at a byte
  // above 0x7F, so a multi-byte character next to a URL cannot be cut in half
  // by the trimming.
  const std::vector<LinkSpan> spans = find_links("veja isso: https://exemplo.com/ação é bom");
  ASSERT_EQ(spans.size(), 1U);
  const std::string_view text = "veja isso: https://exemplo.com/ação é bom";
  EXPECT_EQ(text.substr(spans[0].begin, spans[0].end - spans[0].begin), "https://exemplo.com/ação");
}

TEST(ChatLinksTest, TheSpansNeverOverlapAndAreInOrder) {
  const std::vector<LinkSpan> spans =
      find_links("a http://x.example b https://y.example c http://z.example");
  ASSERT_EQ(spans.size(), 3U);
  for (std::size_t i = 1; i < spans.size(); ++i) {
    EXPECT_LE(spans[i - 1].end, spans[i].begin);
  }
}

}  // namespace
