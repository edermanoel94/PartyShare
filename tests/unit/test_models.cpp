#include <string>

#include <gtest/gtest.h>

#include <dv/models/chat.hpp>
#include <dv/models/room.hpp>
#include <dv/models/user.hpp>

namespace {

using dv::models::Participant;
using dv::models::Room;
using dv::models::User;

Room make_room() {
  Room room;
  room.id = "8F42A1";
  room.name = "dev-room";
  // Named rather than positional. Both types gained a field in the middle
  // while roles were added, and positional initialisers quietly shifted their
  // meaning: what had been "sharing" became "muted by an administrator", and
  // nothing but a failing assertion said so.
  room.participants = {
      Participant{.user = User{.id = "user1", .display_name = "Ana"}},
      Participant{.user = User{.id = "user2", .display_name = "Bruno"}, .muted = true},
      Participant{.user = User{.id = "user3", .display_name = "Carla"}, .sharing_screen = true},
  };
  return room;
}

TEST(Room, FindsAParticipantById) {
  const Room room = make_room();
  const Participant* found = room.find("user2");
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->user.display_name, "Bruno");
  EXPECT_TRUE(found->muted);
}

TEST(Room, ReturnsNullForAnUnknownParticipant) {
  const Room room = make_room();
  EXPECT_EQ(room.find("user9"), nullptr);
  EXPECT_FALSE(room.contains("user9"));
  EXPECT_TRUE(room.contains("user1"));
}

TEST(Room, MutableFindAllowsUpdatingState) {
  Room room = make_room();
  Participant* found = room.find("user1");
  ASSERT_NE(found, nullptr);
  found->muted = true;
  EXPECT_TRUE(room.find("user1")->muted);
}

TEST(Room, ReportsTheSingleScreenSharer) {
  const Room room = make_room();
  const Participant* sharer = room.screen_sharer();
  ASSERT_NE(sharer, nullptr);
  EXPECT_EQ(sharer->user.id, "user3");
}

TEST(Room, ReportsNoSharerWhenNobodyIsSharing) {
  Room room = make_room();
  room.find("user3")->sharing_screen = false;
  EXPECT_EQ(room.screen_sharer(), nullptr);
}

TEST(Room, SizeCountsParticipants) {
  EXPECT_EQ(make_room().size(), 3u);
}

TEST(RoomId, AcceptsTheSpecExample) {
  EXPECT_TRUE(dv::models::is_valid_room_id("8F42A1"));
}

TEST(RoomId, RejectsTheWrongLength) {
  EXPECT_FALSE(dv::models::is_valid_room_id("8F42A"));
  EXPECT_FALSE(dv::models::is_valid_room_id("8F42A11"));
  EXPECT_FALSE(dv::models::is_valid_room_id(""));
}

TEST(RoomId, RejectsLowercaseAndNonHexCharacters) {
  EXPECT_FALSE(dv::models::is_valid_room_id("8f42a1"));
  EXPECT_FALSE(dv::models::is_valid_room_id("8F42AG"));
  EXPECT_FALSE(dv::models::is_valid_room_id("8F42A-"));
}

TEST(User, ComparesByValue) {
  EXPECT_EQ((User{"1", "Ana", ""}), (User{"1", "Ana", ""}));
  EXPECT_NE((User{"1", "Ana", ""}), (User{"2", "Ana", ""}));
}

TEST(ChatText, TrimsTheWhitespaceAround) {
  EXPECT_EQ(dv::models::trim_chat_text("  hello  "), "hello");
  EXPECT_EQ(dv::models::trim_chat_text("\n\thello\r\n"), "hello");
  // Only around. What somebody typed in the middle of a sentence is theirs.
  EXPECT_EQ(dv::models::trim_chat_text("  a  b  "), "a  b");
}

TEST(ChatText, RejectsWhatIsOnlyWhitespace) {
  EXPECT_FALSE(dv::models::is_valid_chat_text(""));
  EXPECT_FALSE(dv::models::is_valid_chat_text("   "));
  EXPECT_FALSE(dv::models::is_valid_chat_text("\n\t\r"));
  EXPECT_TRUE(dv::models::is_valid_chat_text(" x "));
}

TEST(ChatText, MeasuresTheLimitAfterTrimming) {
  const std::string at_the_limit(dv::models::kMaxChatTextBytes, 'x');
  EXPECT_TRUE(dv::models::is_valid_chat_text(at_the_limit));
  EXPECT_FALSE(dv::models::is_valid_chat_text(at_the_limit + "x"));

  // Padded past the limit with spaces that will be thrown away. Checking the
  // length before trimming would refuse a message that is going to fit.
  EXPECT_TRUE(dv::models::is_valid_chat_text("   " + at_the_limit + "   "));
}

TEST(ChatText, LeavesMultibyteCharactersAlone) {
  // Trimming walks bytes, so this is the case that would break if a
  // continuation byte were ever mistaken for whitespace.
  const std::string text = "  Ana Ferrão diz olá  ";
  EXPECT_EQ(dv::models::trim_chat_text(text), "Ana Ferrão diz olá");
}

TEST(ChatText, AcceptsEmojiAndDoesNotCutThemInHalf) {
  // Four bytes each, and the last one sits right where the trim starts
  // walking backwards. A trim that mistook a continuation byte for whitespace
  // would leave half a character behind, which is not a string any more.
  const std::string text = "  deploy feito 🚀🎉  ";
  EXPECT_TRUE(dv::models::is_valid_chat_text(text));
  EXPECT_EQ(dv::models::trim_chat_text(text), "deploy feito 🚀🎉");

  // A message that is nothing but an emoji is a message. Nobody has to write
  // words to say something.
  EXPECT_TRUE(dv::models::is_valid_chat_text("👍"));
  EXPECT_EQ(dv::models::trim_chat_text(" 👍 "), "👍");
}

TEST(ChatText, TheLimitIsBytesSoEmojiCostFourOfThem) {
  // Worth pinning rather than discovering: the limit is bytes, so the number
  // of characters that fit depends on what they are. 2000 emoji do not fit,
  // 500 do, and both of those are the rule working rather than a bug.
  const std::string one = "🚀";
  ASSERT_EQ(one.size(), 4U);

  std::string exactly_full;
  for (std::size_t i = 0; i < dv::models::kMaxChatTextBytes / one.size(); ++i) {
    exactly_full += one;
  }
  EXPECT_EQ(exactly_full.size(), dv::models::kMaxChatTextBytes);
  EXPECT_TRUE(dv::models::is_valid_chat_text(exactly_full));
  EXPECT_FALSE(dv::models::is_valid_chat_text(exactly_full + one));
}

TEST(RoomName, TrimsTheWhitespaceAround) {
  EXPECT_EQ(dv::models::trim_room_name("  Retro de sexta  "), "Retro de sexta");
  EXPECT_EQ(dv::models::trim_room_name("\r\n Daily \t"), "Daily");
}

TEST(RoomName, AcceptsAnEmptyName) {
  // Not an omission to be refused. An empty name is how a client asks for the
  // room to be called by its own identifier, and the RoomManager fills it in.
  EXPECT_TRUE(dv::models::is_valid_room_name(""));
  EXPECT_TRUE(dv::models::is_valid_room_name("     "));
  EXPECT_EQ(dv::models::trim_room_name("     "), "");
}

TEST(RoomName, MeasuresTheLimitAfterTrimming) {
  const std::string full(dv::models::kMaxRoomNameBytes, 'a');
  EXPECT_TRUE(dv::models::is_valid_room_name(full));
  EXPECT_TRUE(dv::models::is_valid_room_name("   " + full + "   "));
  EXPECT_FALSE(dv::models::is_valid_room_name(full + "a"));
}

TEST(RoomName, LeavesMultibyteCharactersAlone) {
  // The limit is bytes and a room name is text people type in their own
  // language, so an accent costs two of them. Pinned rather than discovered.
  EXPECT_EQ(dv::models::trim_room_name(" Reunião de sexta "), "Reunião de sexta");
  EXPECT_TRUE(dv::models::is_valid_room_name("Reunião de sexta"));
}

TEST(RoomNameKey, FoldsAsciiCaseAndTrims) {
  EXPECT_EQ(dv::models::room_name_key("  Retro De Sexta  "), "retro de sexta");
  EXPECT_EQ(dv::models::room_name_key("DAILY"), dv::models::room_name_key("daily"));
}

TEST(RoomNameKey, FoldsTheAsciiLettersOfAWordThatIsNotAscii) {
  // The half of the job that is worth having: names differ in case at the
  // first letter far more often than anywhere else, and the first letter is
  // usually ASCII even when the word is not.
  EXPECT_EQ(dv::models::room_name_key("Reunião"), dv::models::room_name_key("reunião"));
}

TEST(RoomNameKey, LeavesAccentedLettersAsTheyAre) {
  // Pinned rather than discovered. Folding Ã to ã needs a Unicode table this
  // project does not carry, so these two are different names, and somebody
  // surprised by that should find this test rather than guess.
  EXPECT_NE(dv::models::room_name_key("REUNIÃO"), dv::models::room_name_key("reunião"));
}

TEST(RoomNameKey, DoesNotCorruptMultibyteCharacters) {
  // The safety property behind folding bytes instead of characters: every byte
  // of a multibyte UTF-8 character is at least 0x80, so nothing in 'A' to 'Z'
  // can land inside one.
  EXPECT_EQ(dv::models::room_name_key("ção 🚀"), "ção 🚀");
}

TEST(RoomName, RejectsControlCharacters) {
  // The client flattens every room into one tab separated row on its way to
  // the table. A name carrying a tab would arrive there as extra columns in
  // everybody else's list, so it is refused at the door.
  EXPECT_FALSE(dv::models::is_valid_room_name("Retro\tde sexta"));
  EXPECT_FALSE(dv::models::is_valid_room_name("Retro\nde sexta"));
  EXPECT_FALSE(dv::models::is_valid_room_name(std::string("Retro\0de sexta", 14)));
}

TEST(DisplayName, RejectsControlCharacters) {
  // The same rule as a room name, for a sharper reason. The client flattens
  // each participant into one tab separated row whose first field is the user
  // id the moderation menu acts on, so a tab in a name used to move which
  // field was read as that id.
  EXPECT_FALSE(dv::models::is_valid_display_name("Ana\tSilva"));
  EXPECT_FALSE(dv::models::is_valid_display_name("Ana\nSilva"));
  EXPECT_FALSE(dv::models::is_valid_display_name(std::string("Ana\0Silva", 9)));
  EXPECT_FALSE(dv::models::is_valid_display_name("Ana\x7F"));
}

TEST(DisplayName, RejectsTheShapeOfTheForgery) {
  // The attack written out: a name that ends the row early, inserts somebody
  // else's identifier, and starts a new name, so that the list shows "Alice"
  // over an identifier that is not hers.
  EXPECT_FALSE(dv::models::is_valid_display_name("Alice\t9f2c1ab4d0e75613\tAlice"));
}

TEST(DisplayName, AcceptsAnyOrdinaryName) {
  // Accented text, spaces, punctuation and emoji are all names somebody
  // actually has. Only the C0 range and DEL are refused, so nothing above
  // 0x7F is ever inspected and no multibyte character can be caught by it.
  EXPECT_TRUE(dv::models::is_valid_display_name("Ana Silva"));
  EXPECT_TRUE(dv::models::is_valid_display_name("Éder"));
  EXPECT_TRUE(dv::models::is_valid_display_name("O'Brien-Souza"));
  EXPECT_TRUE(dv::models::is_valid_display_name("ção 🚀"));
}

TEST(DisplayName, AcceptsTheEmptyString) {
  // "No display name", which every reader already answers by falling back to
  // the username. Refusing it here would turn a normal account into an error.
  EXPECT_TRUE(dv::models::is_valid_display_name(""));
}

TEST(UserLabel, PutsTheAccountBehindTheName) {
  // The whole point of the pair. The display name is what an operator
  // recognises and it is not unique; the username is unique and is what they
  // type to find the account again.
  EXPECT_EQ(dv::models::user_label("u_7f3a", "Ana", "ana.silva"), "Ana (ana.silva)");
}

TEST(UserLabel, TellsTwoPeopleOfTheSameNameApart) {
  // The case the username is carried for. Two accounts may call themselves
  // "Ana", and a log line naming one of them has to say which.
  EXPECT_NE(dv::models::user_label("u_1", "Ana", "ana"),
            dv::models::user_label("u_2", "Ana", "ana.silva"));
}

TEST(UserLabel, SaysOneNameOnceWhenBothAreTheSame) {
  // Not a nicety: an account created without a display name is given its
  // username as one, which makes this the common case rather than the odd one.
  // "ana (ana)" says nothing twice.
  EXPECT_EQ(dv::models::user_label("u_7f3a", "ana", "ana"), "ana");
}

TEST(UserLabel, FallsBackToTheUsernameWithoutADisplayName) {
  // A row that came back from the store with the field empty. The username is
  // a name a person can read, so there is no reason to reach past it.
  EXPECT_EQ(dv::models::user_label("u_7f3a", "", "ana"), "ana");
}

TEST(UserLabel, FallsBackToTheIdentifierWhenThereIsNothingElse) {
  // What a caller holding an identifier no account answers to gets: somebody
  // deleted while they were still connected. The identifier is not a name, but
  // a line naming nobody at all is worse.
  EXPECT_EQ(dv::models::user_label("u_7f3a", "", ""), "u_7f3a");
}

TEST(RoomLabel, PrefersTheNamePeopleCallIt) {
  EXPECT_EQ(dv::models::room_label("8F42A1", "Daily"), "Daily");
}

TEST(RoomLabel, FallsBackToTheIdentifier) {
  // Very nearly dead code, because create_room writes the identifier into the
  // name of a room nobody named. It is reached by a room stored before that
  // field existed, and by an identifier no room answers to any more.
  EXPECT_EQ(dv::models::room_label("8F42A1", ""), "8F42A1");
}

TEST(RoomLabel, PrintsOneThingAndNotTwo) {
  // Unlike a user label. A room name is unique, because create_room refuses
  // one that is taken, so the name alone already says which room.
  EXPECT_EQ(dv::models::room_label("8F42A1", "Daily").find("8F42A1"), std::string::npos);
}

}  // namespace
