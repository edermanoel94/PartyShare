#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace dv::models {

/// What a user is allowed to do, section 18 of SPEC.md.
///
/// Two values and no more, because a third one costs a policy nobody has
/// written yet. `User` is deliberately the zero value: a record that arrives
/// without a role, from an older database or a hand written document, is a
/// plain user and never an administrator.
enum class Role : std::uint8_t {
  /// Joins rooms, creates rooms, shares a screen, mutes themselves.
  User,
  /// Everything above, plus removing and muting other participants and
  /// managing the accounts and the rooms.
  Admin,
};

/// The wire and database name of a role, "user" or "admin".
[[nodiscard]] std::string_view to_string(Role role) noexcept;

/// Inverse of to_string. Anything unrecognised is `Role::User`, for the same
/// reason the enum starts there: the safe answer to "I do not know what this
/// is" is the role that can do less.
[[nodiscard]] Role role_from_string(std::string_view name) noexcept;

/// What an administrator has taken away from an account until they give it
/// back, section 18 of SPEC.md.
///
/// Account wide and written down, which is what separates these from the two
/// things that already existed. A participant's own mute is theirs to undo, and
/// a kick ends when the room does. A restriction outlives the room, the session
/// and the process, because what it is about outlives all three: the person is
/// the same person the next time they log in, and an administrator who had to
/// re-apply it after every reconnection would not be moderating anything.
///
/// Four flags and no expiry. A restriction that lifts itself needs a clock
/// every reader agrees on and a rule for what happens to a room while it runs
/// out. Until somebody wants that, "until an administrator says otherwise" is a
/// policy that cannot be half applied.
///
/// Every flag is false by default, so an account read out of a database written
/// before this existed is an account with nothing taken away. Same reasoning as
/// `Role::User` being the zero value: the safe reading of a missing field is
/// the one that takes nothing away.
struct Restrictions {
  /// Cannot log in. The lasting form of a kick: a kick ends one visit, and this
  /// ends the account's access until somebody lifts it.
  bool banned = false;
  /// Cannot transmit audio. They arrive in a room already muted, by the
  /// administrator rather than by themselves, so the mute holds exactly as
  /// `Participant::muted_by_admin` holds.
  bool muted = false;
  /// Cannot say anything in a room's chat. Reading it is untouched: a
  /// conversation somebody may not speak in is still one they are sitting in,
  /// and taking that away as well would only mean they cannot follow what is
  /// being said about them.
  bool silenced = false;
  /// Cannot start a screen share. One already running stops the moment the
  /// restriction is applied, because a rule that waits for the next attempt is
  /// not a rule about what is on everybody's screen right now.
  bool screen_share_blocked = false;

  /// Whether anything at all is taken away, which is what decides between
  /// drawing a row of badges and drawing nothing.
  [[nodiscard]] bool any() const noexcept {
    return banned || muted || silenced || screen_share_blocked;
  }

  friend bool operator==(const Restrictions&, const Restrictions&) = default;
};

/// The restrictions as a line of their wire names, "banned muted", and an empty
/// string when there are none.
///
/// The display form, for wherever a set of them has to be shown to a person:
/// the panel's table, a detail card, a log line. What an audit entry records is
/// not this but which flags moved, "banned=true muted=false", because a log
/// that only ever states the resulting set leaves the reader to diff it against
/// an entry they have to go and find.
[[nodiscard]] std::string describe(const Restrictions& restrictions);

/// Section 18 of SPEC.md.
struct User {
  std::string id;
  std::string display_name;
  /// URL or local path. Empty means the client falls back to initials.
  std::string avatar;
  Role role = Role::User;
  /// What an administrator has taken away. It travels with the user everywhere
  /// the user does, which is what lets every participant's client explain why
  /// somebody's microphone is off, and lets a session know at login what it may
  /// not do rather than finding out by being refused.
  Restrictions restrictions;

  [[nodiscard]] bool is_admin() const noexcept { return role == Role::Admin; }

  friend bool operator==(const User&, const User&) = default;
};

/// How a person is named in a log line: the name they answer to, followed by
/// the account it belongs to.
///
/// Both halves earn their place. `display_name` is what an operator reading the
/// log recognises, and printing it instead of an identifier is the whole point.
/// It is also not unique: nothing stops two accounts from calling themselves
/// "Ana", and a line naming one of them would say nothing about which. The
/// username is unique and is what an administrator types to find the account
/// again, so it rides along in parentheses.
///
/// The parentheses are dropped when the two are the same string, which is the
/// common case and not the exceptional one: an account created without a
/// display name is given its username as one. "ana (ana)" says nothing twice.
///
/// `id` is the last resort, for a caller holding an identifier no account
/// answers to any more. A line about somebody deleted mid-session should still
/// say which identifier it meant.
[[nodiscard]] std::string user_label(const std::string& id, const std::string& display_name,
                                     const std::string& username);

/// A user as seen inside a room, together with the state the other
/// participants need to render them.
struct Participant {
  User user;
  bool muted = false;
  /// Whether it was an administrator who muted them, rather than themselves.
  ///
  /// Its own flag and not a shade of `muted`, because the two behave
  /// differently: a participant releases their own mute whenever they like,
  /// and only an administrator releases this one. Without the distinction a
  /// forced mute is advice, undone by one click on the button that turned
  /// itself off.
  bool muted_by_admin = false;
  bool sharing_screen = false;
  /// Whether that share is carrying the sound of the sharer's machine.
  ///
  /// Remembered here only so that somebody joining mid-share can be told. The
  /// audio itself rides in the sharer's own track and needs nothing from the
  /// server. Meaningless while `sharing_screen` is false, and cleared with it.
  bool sharing_audio = false;

  friend bool operator==(const Participant&, const Participant&) = default;
};

}  // namespace dv::models
