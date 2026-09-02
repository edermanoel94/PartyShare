package store

import (
	"context"
	"testing"
	"time"

	"go.mongodb.org/mongo-driver/v2/bson"
)

// writeSession puts one row into the collection the way the server writes it:
// the field names of dv::server::store::SessionRecord, and nothing else.
//
// By hand rather than through this package, because this package has no write
// for this collection and is not getting one. The server owns these documents;
// what is under test here is that they are read back the way they were meant.
func writeSession(t *testing.T, store *Store, session Session) {
	t.Helper()
	_, err := store.sessions.InsertOne(context.Background(), bson.D{
		{Key: "user_id", Value: session.UserID},
		{Key: "ip", Value: session.IP},
		{Key: "connected_at", Value: session.ConnectedAt},
		{Key: "last_seen_at", Value: session.LastSeenAt},
		{Key: "ended_at", Value: session.EndedAt},
	})
	if err != nil {
		t.Fatalf("could not write the session: %v", err)
	}
}

func TestSessionsAreReadOpenFirst(t *testing.T) {
	t.Parallel()
	store := testStore(t)
	now := time.Now().Unix()

	// The order is the whole design of the query. A session open since this
	// morning is older than everything that came and went since, and it is the
	// row somebody opened this screen to see: without the ended_at key it
	// would sit below two hundred finished ones.
	writeSession(t, store, Session{
		UserID: "id-recent-and-gone", IP: "203.0.113.9",
		ConnectedAt: now - 60, LastSeenAt: now - 30, EndedAt: now - 30,
	})
	writeSession(t, store, Session{
		UserID: "id-here-since-morning", IP: "203.0.113.7",
		ConnectedAt: now - 28800, LastSeenAt: now - 2,
	})
	writeSession(t, store, Session{
		UserID: "id-older-and-gone", IP: "203.0.113.10",
		ConnectedAt: now - 7200, LastSeenAt: now - 7000, EndedAt: now - 7000,
	})

	sessions, err := store.Sessions(context.Background(), 0)
	if err != nil {
		t.Fatalf("Sessions: %v", err)
	}
	if len(sessions) != 3 {
		t.Fatalf("expected three sessions, got %d", len(sessions))
	}

	if sessions[0].UserID != "id-here-since-morning" {
		t.Errorf("the open session should come first, got %q", sessions[0].UserID)
	}
	// And what follows is ordered by how recently anybody was heard from, so
	// the closed rows read as a history rather than as whatever order the
	// collection happened to be in.
	if sessions[1].UserID != "id-recent-and-gone" {
		t.Errorf("the newer closed session should come second, got %q", sessions[1].UserID)
	}
	if sessions[2].UserID != "id-older-and-gone" {
		t.Errorf("the older closed session should come last, got %q", sessions[2].UserID)
	}
}

func TestASessionIsReadFieldForField(t *testing.T) {
	t.Parallel()
	store := testStore(t)

	written := Session{
		UserID:      "id-ana",
		IP:          "203.0.113.7",
		ConnectedAt: 1_800_000_000,
		LastSeenAt:  1_800_000_030,
		EndedAt:     1_800_000_600,
	}
	writeSession(t, store, written)

	sessions, err := store.Sessions(context.Background(), 0)
	if err != nil {
		t.Fatalf("Sessions: %v", err)
	}
	if len(sessions) != 1 {
		t.Fatalf("expected one session, got %d", len(sessions))
	}
	if sessions[0] != written {
		t.Fatalf("the session did not survive the round trip:\n got %+v\nwant %+v",
			sessions[0], written)
	}
}

// A document written before a field existed, or by hand. The reader has to
// answer with a session rather than an error, the same way the room reader
// does and for the same reason: this program is what somebody reaches for when
// the database is in a state nothing else can cope with.
func TestASessionWithFieldsMissingStillReads(t *testing.T) {
	t.Parallel()
	store := testStore(t)

	_, err := store.sessions.InsertOne(context.Background(), bson.D{
		{Key: "user_id", Value: "id-ana"},
	})
	if err != nil {
		t.Fatalf("could not write the session: %v", err)
	}

	sessions, listErr := store.Sessions(context.Background(), 0)
	if listErr != nil {
		t.Fatalf("Sessions: %v", listErr)
	}
	if len(sessions) != 1 {
		t.Fatalf("expected one session, got %d", len(sessions))
	}
	session := sessions[0]
	if session.UserID != "id-ana" || session.IP != "" ||
		session.ConnectedAt != 0 || session.LastSeenAt != 0 || session.EndedAt != 0 {
		t.Fatalf("the missing fields did not read as zero: %+v", session)
	}
	// And it is open, because ended_at is absent - which is exactly the row
	// the staleness rule has to catch, since its heartbeat is zero too.
	if !session.Open() {
		t.Error("a document with no ended_at should read as open")
	}
	if session.Online(time.Now()) {
		t.Error("a document with no heartbeat should not read as online")
	}
}

func TestTheSessionLimitIsClamped(t *testing.T) {
	t.Parallel()
	store := testStore(t)
	now := time.Now().Unix()

	for i := range 5 {
		writeSession(t, store, Session{
			UserID:      "id-ana",
			IP:          "203.0.113.7",
			ConnectedAt: now - int64(i) - 1,
			LastSeenAt:  now - int64(i),
			EndedAt:     now - int64(i),
		})
	}

	sessions, err := store.Sessions(context.Background(), 2)
	if err != nil {
		t.Fatalf("Sessions: %v", err)
	}
	if len(sessions) != 2 {
		t.Fatalf("expected the limit to be honoured, got %d rows", len(sessions))
	}

	// Zero asks for the default rather than for nothing, which is what every
	// other limit in this program means by it.
	all, err := store.Sessions(context.Background(), 0)
	if err != nil {
		t.Fatalf("Sessions: %v", err)
	}
	if len(all) != 5 {
		t.Fatalf("zero should have asked for the default, got %d rows", len(all))
	}
}

func TestTheSummaryCountsSessions(t *testing.T) {
	t.Parallel()
	store := testStore(t)

	writeSession(t, store, Session{UserID: "id-ana", LastSeenAt: time.Now().Unix()})
	writeSession(t, store, Session{UserID: "id-bruno", EndedAt: time.Now().Unix()})

	summary, err := store.Summary(context.Background())
	if err != nil {
		t.Fatalf("Summary: %v", err)
	}
	// Every document, open and closed alike, because that is what the tab bar
	// counts for the other three collections as well. How many are somebody
	// who is here is a question about time, and the screen answers it.
	if summary.Sessions != 2 {
		t.Fatalf("expected two sessions counted, got %d", summary.Sessions)
	}
}
