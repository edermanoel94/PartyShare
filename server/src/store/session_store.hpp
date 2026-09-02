#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <dv/core/result.hpp>

namespace dv::server::store {

/// One account's stay on this server: where they came from, when they arrived,
/// when they were last heard from, and when they left.
///
/// It is written down for one reason, and the reason is not the server. The
/// server already knows who is connected - it is holding the sockets - and
/// nothing in `hub.cpp` reads this back. What cannot know is anything that is
/// not the server process: tools/dbadmin talks to MongoDB and to nothing else,
/// by a contract its README states in the first paragraph, so "who is online"
/// is a question only the database can answer, and only if somebody writes the
/// answer into it.
struct SessionRecord {
  /// Assigned by the store.
  std::string id;
  /// The account. An identifier and not a username, like every other reference
  /// between collections here: the username can be changed, and a row that
  /// then names nobody is worse than one carrying a code that still resolves.
  std::string user_id;
  /// Where the connection came from, as the transport reported it.
  ///
  /// Text and not a parsed address, because what is useful about it is that it
  /// can be compared with the one in a firewall log or read out over the
  /// phone. A server behind a proxy sees the proxy, which is a true statement
  /// about this hop and is why the field is named for the connection rather
  /// than for the person.
  std::string ip;
  /// Seconds since the Unix epoch, UTC, all three. Wall clocks rather than
  /// steady ones: they are read by a person, in another program, after this
  /// process is gone.
  std::int64_t connected_at = 0;
  /// Refreshed on the server's heartbeat, which is what tells a reader the
  /// difference between somebody who is here and somebody whose server died
  /// mid-session. A row that is open and stale is the second one.
  std::int64_t last_seen_at = 0;
  /// When the session ended, and zero while it has not.
  ///
  /// Zero is the safe reading of a field nobody wrote, and it is safe in the
  /// direction that matters: an open row that should be closed is caught by
  /// `last_seen_at` going stale, where a closed row that should be open would
  /// be somebody shown as offline while they are talking.
  std::int64_t ended_at = 0;

  [[nodiscard]] bool open() const noexcept { return ended_at == 0; }

  friend bool operator==(const SessionRecord&, const SessionRecord&) = default;
};

/// Where the sessions above live.
///
/// Every write here is best effort from the Hub's point of view: a session
/// that could not be recorded is logged and the person is let in anyway. The
/// trade is the audit log's, one step further along. Refusing a login because
/// the presence collection is unreachable would take the product down to
/// protect a report about the product, and unlike an audit entry this is not
/// even a record of a decision somebody made - it is a record of somebody
/// being present, which the room they are in is already evidence of.
///
/// Not thread safe. See UserStore.
class SessionStore {
 public:
  SessionStore() = default;
  virtual ~SessionStore() = default;

  SessionStore(const SessionStore&) = delete;
  SessionStore& operator=(const SessionStore&) = delete;
  SessionStore(SessionStore&&) = delete;
  SessionStore& operator=(SessionStore&&) = delete;

  /// Records a session that has just started, and hands back what was written.
  ///
  /// The store assigns `id` and stamps `connected_at` and `last_seen_at` when
  /// they are zero, which is what keeps the wall clock out of the Hub - the
  /// Hub's own clock is a steady one and means nothing to a reader of the
  /// database. The identifier comes back because it is what `touch` and
  /// `close` name, and it is held on the connection for exactly as long as the
  /// connection is.
  [[nodiscard]] virtual Result<SessionRecord> open(SessionRecord record) = 0;

  /// Says that every one of these sessions was still there just now.
  ///
  /// A list and not one identifier, because it is called once per heartbeat
  /// for every connection at once. One write for the whole set rather than one
  /// per session is what keeps a server holding fifty connections from making
  /// fifty round trips to the database every five seconds with its own lock
  /// held.
  ///
  /// Identifiers that no longer exist are skipped rather than reported: a
  /// session removed from the collection by hand is not a reason to fail the
  /// heartbeat of everybody else on the server.
  [[nodiscard]] virtual std::optional<Error> touch(const std::vector<std::string>& ids) = 0;

  /// Marks one session as over, stamping the moment it ended.
  ///
  /// A session that is already closed is left exactly as it is, and that is
  /// not a failure. The three ways a session ends - the socket closing, the
  /// account being banned, and a second login taking the identity - can
  /// overlap, and the first of them is the one that told the truth about when
  /// the person left.
  [[nodiscard]] virtual std::optional<Error> close(const std::string& id) = 0;

  /// Ends every session still open, and answers how many there were.
  ///
  /// Called once at startup, and it is the recovery for the one thing this
  /// collection cannot otherwise survive: a server that was killed rather than
  /// stopped leaves its rows open, and nothing else will ever close them.
  /// Each is stamped with its own `last_seen_at` rather than with the moment
  /// of recovery, because that is when the session was last known to be real -
  /// writing "now" would claim everybody stayed connected through however long
  /// the server was down.
  ///
  /// It assumes one server per database, which is what this project deploys:
  /// two servers sharing rooms held in one process's memory would be broken in
  /// louder ways than this long before anybody noticed the sessions.
  [[nodiscard]] virtual std::size_t close_open() = 0;

  /// The sessions that have not ended, newest first.
  ///
  /// The read half of the contract. Nothing in the server calls it - the
  /// server has the sockets - and it is here because a store that can only be
  /// written to is a store whose two implementations cannot be shown to agree
  /// about what they wrote.
  [[nodiscard]] virtual std::vector<SessionRecord> list_open() const = 0;
};

}  // namespace dv::server::store
