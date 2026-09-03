package store

import (
	"context"
	"errors"
	"strings"
	"testing"

	"go.mongodb.org/mongo-driver/v2/bson"
)

// The notice document is what the C++ server reads back through
// notice_from in mongo_store.cpp, and hands over on its heartbeat or at the
// next sign-in. A field renamed here is not a broken test: it is a notice
// with no text, or one addressed to nobody, sitting pending forever.
func TestSendNoticeWritesTheDocumentTheServerReads(t *testing.T) {
	t.Parallel()
	database := testStore(t)
	created := mustCreate(t, database, NewAccount{Username: "bruno", Password: "one"})

	notice, err := database.SendNotice(context.Background(), created.UserID, "please use a headset")
	if err != nil {
		t.Fatalf("SendNotice: %v", err)
	}
	if len(notice.ID) != 24 {
		t.Errorf("the identifier is %q, want the hexadecimal object id the server acknowledges by",
			notice.ID)
	}

	var document bson.M
	if err := database.notices.FindOne(context.Background(), bson.D{}).Decode(&document); err != nil {
		t.Fatalf("could not read the document back: %v", err)
	}
	for field, want := range map[string]any{
		"user_id":           created.UserID,
		"from_user_id":      "",
		"from_display_name": "",
		"text":              "please use a headset",
		"acknowledged_at":   int64(0),
	} {
		if document[field] != want {
			t.Errorf("%s is %#v, want %#v", field, document[field], want)
		}
	}
	// int64 and not int32, which is what the server's int_field reads and
	// what the Go driver would otherwise pick for a small number.
	if created, ok := document["created_at"].(int64); !ok || created <= 0 {
		t.Errorf("created_at is %#v, want a positive int64", document["created_at"])
	}
	if id, ok := document["_id"].(bson.ObjectID); !ok || id.Hex() != notice.ID {
		t.Errorf("_id is %#v, want the identifier that was handed back", document["_id"])
	}

	entries, err := database.Audit(context.Background(), AuditQuery{})
	if err != nil {
		t.Fatalf("Audit: %v", err)
	}
	if len(entries) != 2 {
		t.Fatalf("the log holds %d entries, want the create and the notice", len(entries))
	}
	// By action and not by position: the two were written in the same second,
	// and the log orders by the second.
	entry := entryFor(t, entries, ActionSendNotice)
	if entry.TargetID != created.UserID {
		t.Errorf("the entry is %+v, want a send_notice against the account", entry)
	}
	// The same detail the server writes for its own, so the two read alike.
	if entry.Detail != "notice="+notice.ID+" please use a headset" {
		t.Errorf("the detail is %q", entry.Detail)
	}
	if entry.ActorID != "dbadmin:test" {
		t.Errorf("the entry is attributed to %q", entry.ActorID)
	}
}

func TestSendNoticeTrimsAndRefusesWhatTheServerWould(t *testing.T) {
	t.Parallel()
	database := testStore(t)
	created := mustCreate(t, database, NewAccount{Username: "bruno", Password: "one"})

	notice, err := database.SendNotice(context.Background(), created.UserID, "  read this \n")
	if err != nil {
		t.Fatalf("SendNotice: %v", err)
	}
	if notice.Text != "read this" {
		t.Errorf("the text was stored as %q, want it trimmed", notice.Text)
	}

	if _, err := database.SendNotice(context.Background(), created.UserID, " \t\n"); !errors.Is(err, ErrEmptyNotice) {
		t.Errorf("a blank notice was answered with %v, want ErrEmptyNotice", err)
	}
	// Bytes and not characters: five hundred of these is a thousand bytes.
	long := strings.Repeat("é", MaxNoticeTextBytes)
	if _, err := database.SendNotice(context.Background(), created.UserID, long); !errors.Is(err, ErrNoticeTooLong) {
		t.Errorf("an oversized notice was answered with %v, want ErrNoticeTooLong", err)
	}
	// And exactly the limit is not over it.
	if _, err := database.SendNotice(context.Background(), created.UserID,
		strings.Repeat("a", MaxNoticeTextBytes)); err != nil {
		t.Errorf("a notice of exactly the limit was refused: %v", err)
	}

	count, err := database.notices.CountDocuments(context.Background(), bson.D{})
	if err != nil {
		t.Fatalf("CountDocuments: %v", err)
	}
	if count != 2 {
		t.Errorf("the collection holds %d notices, want the two that were accepted", count)
	}
}

func TestSendNoticeReportsAMissingAccountAndWritesNothing(t *testing.T) {
	t.Parallel()
	database := testStore(t)

	_, err := database.SendNotice(context.Background(), "nobody", "hello?")
	if !errors.Is(err, ErrUserNotFound) {
		t.Fatalf("got %v, want ErrUserNotFound", err)
	}
	count, err := database.notices.CountDocuments(context.Background(), bson.D{})
	if err != nil {
		t.Fatalf("CountDocuments: %v", err)
	}
	if count != 0 {
		t.Errorf("a notice to nobody was written")
	}
	entries, err := database.Audit(context.Background(), AuditQuery{})
	if err != nil {
		t.Fatalf("Audit: %v", err)
	}
	if len(entries) != 0 {
		t.Errorf("a refused notice was recorded: %+v", entries)
	}
}

// entryFor is the one entry of the log with this action, and fails when there
// is none or more than one.
func entryFor(t *testing.T, entries []AuditEntry, action string) AuditEntry {
	t.Helper()
	var found []AuditEntry
	for _, entry := range entries {
		if entry.Action == action {
			found = append(found, entry)
		}
	}
	if len(found) != 1 {
		t.Fatalf("the log holds %d %s entries, want 1: %+v", len(found), action, entries)
	}
	return found[0]
}
