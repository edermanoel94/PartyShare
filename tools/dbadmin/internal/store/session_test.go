package store

import (
	"testing"
	"time"
)

// now is the instant every case below is measured against. Fixed rather than
// time.Now(), because a test whose answer depends on when it ran is a test
// that fails once a month for reasons nobody can reproduce.
var now = time.Unix(1_800_000_000, 0)

// at is a stored timestamp `ago` before `now`, which is how every one of these
// cases is written: the question is never what o'clock it is, it is how long
// ago the last heartbeat was.
func at(ago time.Duration) int64 { return now.Add(-ago).Unix() }

func TestAnOpenSessionSeenRecentlyIsOnline(t *testing.T) {
	t.Parallel()
	session := Session{UserID: "id-ana", IP: "203.0.113.7", ConnectedAt: at(time.Hour), LastSeenAt: at(2 * time.Second)}

	if !session.Open() {
		t.Fatal("a session with no ended_at should be open")
	}
	if !session.Online(now) {
		t.Error("a session heard from two seconds ago should be online")
	}
}

func TestAnOpenSessionNobodyHasHeardFromIsNotOnline(t *testing.T) {
	t.Parallel()
	// The row a server that was killed rather than stopped leaves behind.
	// Nothing will ever close it, so `Open` goes on saying yes forever, and
	// this is the check that stops the screen repeating that as "online".
	session := Session{UserID: "id-ana", ConnectedAt: at(time.Hour), LastSeenAt: at(time.Hour)}

	if !session.Open() {
		t.Fatal("a session with no ended_at should still be open")
	}
	if session.Online(now) {
		t.Error("a session last heard from an hour ago should not be online")
	}
}

func TestAClosedSessionIsNeverOnline(t *testing.T) {
	t.Parallel()
	// Even one closed a second ago, and even though its heartbeat is fresh:
	// the server said this session is over, and that is the more specific
	// statement of the two.
	session := Session{
		UserID:      "id-ana",
		ConnectedAt: at(time.Hour),
		LastSeenAt:  at(time.Second),
		EndedAt:     at(time.Second),
	}

	if session.Open() {
		t.Fatal("a session with an ended_at should not be open")
	}
	if session.Online(now) {
		t.Error("a session the server closed should not be online")
	}
}

func TestTheStaleWindowIsWhereItSaysItIs(t *testing.T) {
	t.Parallel()
	// The boundary itself, from both sides. A window nobody pins is a window
	// that drifts by a heartbeat the next time somebody edits the expression.
	fresh := Session{LastSeenAt: at(SessionStaleAfter - time.Second)}
	stale := Session{LastSeenAt: at(SessionStaleAfter + time.Second)}

	if !fresh.Online(now) {
		t.Errorf("a heartbeat %v ago should still count as online", SessionStaleAfter-time.Second)
	}
	if stale.Online(now) {
		t.Errorf("a heartbeat %v ago should not count as online", SessionStaleAfter+time.Second)
	}
}

func TestASessionWithNoHeartbeatIsNotOnline(t *testing.T) {
	t.Parallel()
	// A zero last_seen_at is a document written by hand, or by a version of
	// the server that did not have the field. The safe reading of a field
	// nobody wrote is the one that does not claim somebody is here.
	session := Session{UserID: "id-ana", ConnectedAt: at(time.Second)}

	if session.Online(now) {
		t.Error("a session with no heartbeat at all should not be online")
	}
}
