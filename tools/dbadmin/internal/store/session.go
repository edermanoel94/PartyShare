package store

import "time"

// Session is one account's stay on a server, as the database keeps it: the Go
// transcription of dv::server::store::SessionRecord.
//
// This is the one collection here that nothing writes and only the server
// does. That is the whole point of it. The server is holding the sockets and
// never has to ask who is connected; this program has no connection to the
// server at all, by the contract stated in the first paragraph of the README,
// so a question about who is online now is a question only the database can
// answer, and only because the server writes the answer down.
type Session struct {
	// The account. An identifier and not a username, like every other
	// reference between collections here: resolving it to a name is the
	// screen's job, and an account that has been deleted leaves sessions
	// whose owner cannot be resolved at all.
	UserID string `bson:"user_id"`
	// Where the connection came from, as the server's transport reported it.
	//
	// Text and not a parsed address, because what is useful about it is that
	// it can be compared against a firewall log or read out over the phone. A
	// server behind a proxy sees the proxy, which is a true statement about
	// that hop and is why the field is named for the connection rather than
	// for the person.
	IP string `bson:"ip"`
	// Seconds since the Unix epoch, UTC, all three.
	ConnectedAt int64 `bson:"connected_at"`
	// LastSeenAt is refreshed on the server's heartbeat, which is what tells a
	// reader the difference between somebody who is here and somebody whose
	// server died mid-session.
	LastSeenAt int64 `bson:"last_seen_at"`
	// EndedAt is when the session finished, and zero while it has not.
	EndedAt int64 `bson:"ended_at"`
}

// SessionStaleAfter is how long a session may go without a heartbeat before
// this program stops calling it online.
//
// Six times the server's default heartbeat interval of five seconds, which is
// in server.heartbeat_interval_ms and in docs/03-configuration.md. Six because
// one missed beat is a slow network and six is a server that is not there: the
// cost of being too eager is reporting somebody as gone while they are talking,
// and the cost of being too patient is reporting a machine that was unplugged
// as being on the platform.
//
// An operator who has raised the server's interval past this should raise this
// with it. There is no way for this program to read that setting: it is in the
// server's configuration file, and this program is pointed at a database.
const SessionStaleAfter = 30 * time.Second

// Open is whether the server thinks this session has not finished.
//
// Not the same question as Online. A session that a killed server left behind
// is open forever - nothing will ever close it - which is exactly why the
// stale check below exists.
func (s Session) Open() bool { return s.EndedAt == 0 }

// Online is whether this session counts as somebody who is on the platform
// right now, measured against `now`.
//
// Two facts, and both have to agree, which is the whole reason this is a
// function rather than a field:
//
//   - EndedAt is zero while the server believes the session is still running.
//     A server that was stopped closes its sessions on the way out, and one
//     that was killed does not - and nothing else ever will, so those rows
//     stay open until the next start of a server against this database runs
//     its recovery pass.
//   - LastSeenAt is stamped on every heartbeat, five seconds apart by default.
//     A row whose last beat was minutes ago is a row whose server is gone,
//     whatever EndedAt says.
//
// Both, and not either, because the two failures are not the same size.
// Believing somebody is here when their machine was unplugged is a screen that
// lies with confidence: an operator reads it, decides nobody needs calling,
// and is wrong. Believing somebody has gone when they are talking is a screen
// that is merely out of date for a few seconds, and the next refresh corrects
// it. So the check that can only remove a row is the one that runs first.
//
// A zero LastSeenAt - a document written by hand, or by a version of the
// server that had no such field - reads as the epoch, which is a very long
// time before `now` and therefore not online. That is the reading this wants
// and it is stated here rather than left to arithmetic, because it is the
// answer that takes nothing away from anybody: a row nobody can date is a row
// nobody should be counted by.
//
// A stamp from the future is treated as fresh. Clocks between a server and
// whoever is reading this drift, and the alternative - refusing to believe a
// heartbeat because it is a second ahead - would report an entire server as
// gone over a clock nobody noticed.
//
// `now` is passed in rather than read from the clock so that a test can say
// what time it is, and so that every row of one screen is measured against the
// same instant.
func (s Session) Online(now time.Time) bool {
	if !s.Open() {
		return false
	}
	return now.Sub(time.Unix(s.LastSeenAt, 0)) <= SessionStaleAfter
}
