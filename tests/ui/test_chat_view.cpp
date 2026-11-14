// How a line of the conversation is drawn. The text of a line was written by
// another participant, so what matters most here is what is *not* done with
// it: none of it is ever markup.

#include <gtest/gtest.h>

#include <QListWidgetItem>
#include <QString>

#include "ui/chat_view.hpp"

using dv::ui::ChatView;

namespace {

/// The rich text the view built for the last line it was given.
[[nodiscard]] QString html_of_last(const ChatView& view) {
  return view.item(view.count() - 1)->data(ChatView::kHtmlRole).toString();
}

}  // namespace

TEST(ChatView, ASpokenLineGetsItsSpeakerAndItsTimeSetApart) {
  ChatView view;
  view.append_line(QStringLiteral("[10:51] Bruno: Can everyone hear me?"));

  const QString html = html_of_last(view);
  EXPECT_TRUE(html.contains(QStringLiteral("<span class=\"who\">Bruno</span>")))
      << qPrintable(html);
  EXPECT_TRUE(html.contains(QStringLiteral("<span class=\"when\">10:51</span>")));
  EXPECT_TRUE(html.endsWith(QStringLiteral("Can everyone hear me?")));
  // The prefix is drawn as a head, not said twice.
  EXPECT_FALSE(html.contains(QStringLiteral("[10:51]")));
}

TEST(ChatView, WhatIsKeptSelectedAndCopiedIsTheLineAsItArrived) {
  // Copying an exchange out of the room is half of what the chat is for, and
  // what lands on the clipboard has to say who said it and when.
  ChatView view;
  const QString line = QStringLiteral("[10:51] Bruno: Can everyone hear me?");
  view.append_line(line);
  EXPECT_EQ(view.item(0)->text(), line);
}

TEST(ChatView, ALineWithNoTimeStillHasASpeaker) {
  ChatView view;
  view.append_line(QStringLiteral("Ana: no clock on this one"));
  const QString html = html_of_last(view);
  EXPECT_TRUE(html.contains(QStringLiteral("<span class=\"who\">Ana</span>")));
  EXPECT_FALSE(html.contains(QStringLiteral("class=\"when\"")));
}

TEST(ChatView, ALineThatIsNotSomebodySpeakingIsDrawnWhole) {
  // A nudge, a notice from the room: no colon, no head.
  ChatView view;
  view.append_line(QStringLiteral("Carla nudged Bruno"));
  EXPECT_EQ(html_of_last(view), QStringLiteral("Carla nudged Bruno"));
}

TEST(ChatView, AColonFarIntoASentenceIsPunctuation) {
  ChatView view;
  const QString line = QStringLiteral(
      "The administrator restricted this account for the rest of the afternoon and wrote: no");
  view.append_line(line);
  EXPECT_FALSE(html_of_last(view).contains(QStringLiteral("class=\"who\"")));
}

TEST(ChatView, NothingAParticipantTypesIsEverMarkup) {
  ChatView view;
  view.append_line(QStringLiteral("[10:51] Eve: <b>bold</b> <img src=x> &amp;"));
  const QString html = html_of_last(view);
  EXPECT_FALSE(html.contains(QStringLiteral("<b>")));
  EXPECT_FALSE(html.contains(QStringLiteral("<img")));
  EXPECT_TRUE(html.contains(QStringLiteral("&lt;b&gt;bold&lt;/b&gt;")));
  EXPECT_TRUE(html.contains(QStringLiteral("&amp;amp;")));
}

TEST(ChatView, NorIsAName) {
  // The speaker's name goes into a span of its own. It is as much somebody
  // else's typing as the message is.
  ChatView view;
  view.append_line(QStringLiteral("[10:51] <i>Eve</i>: hello"));
  const QString html = html_of_last(view);
  EXPECT_FALSE(html.contains(QStringLiteral("<i>")));
  EXPECT_TRUE(html.contains(QStringLiteral("&lt;i&gt;Eve&lt;/i&gt;")));
}

TEST(ChatView, ALinkIsItsOwnLabel) {
  // A chat where the text of a link and where it goes can differ is a chat
  // that can be used to lie about where a click lands.
  ChatView view;
  view.append_line(QStringLiteral("[10:51] Carla: see https://example.com/releases now"));
  const QString html = html_of_last(view);
  EXPECT_TRUE(html.contains(
      QStringLiteral("<a href=\"https://example.com/releases\">https://example.com/releases</a>")))
      << qPrintable(html);
  EXPECT_TRUE(html.contains(QStringLiteral("<span class=\"who\">Carla</span>")));
  EXPECT_TRUE(html.endsWith(QStringLiteral(" now")));
}

TEST(ChatView, ALinkWhereTheNameWouldBeKeepsTheLineWhole) {
  // The link's place in the line is counted from the start of the line. A
  // head cut out from under it would put the anchor round the wrong words,
  // so a line like this is drawn the way lines always were.
  ChatView view;
  view.append_line(QStringLiteral("https://example.com/a: hello"));
  const QString html = html_of_last(view);
  EXPECT_FALSE(html.contains(QStringLiteral("class=\"who\"")));
  EXPECT_TRUE(html.contains(QStringLiteral("<a href=\"https://example.com/a")));
}

TEST(ChatView, ReplacingTheLinesReplacesThem) {
  // The history arrives once per join, and a reconnection that rejoins the
  // same room would otherwise show every message twice.
  ChatView view;
  view.append_line(QStringLiteral("Ana: one"));
  view.set_lines({QStringLiteral("Ana: two"), QStringLiteral("Ana: three")});
  ASSERT_EQ(view.count(), 2);
  EXPECT_EQ(view.item(0)->text(), QStringLiteral("Ana: two"));
  EXPECT_TRUE(html_of_last(view).endsWith(QStringLiteral("three")));
}
