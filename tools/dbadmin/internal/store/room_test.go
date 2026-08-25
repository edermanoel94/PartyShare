package store

import (
	"context"
	"errors"
	"testing"

	"go.mongodb.org/mongo-driver/v2/bson"
)

// writeRoom puts a document in the shape the server writes, field for field.
//
// By hand rather than through a method of this package, because there is no
// such method on purpose: nothing here creates a room. What the tests below
// are about is reading and removing what the server left, so the document has
// to come from somewhere that is not the code under test.
func writeRoom(t *testing.T, store *Store, room Room) {
	t.Helper()
	_, err := store.rooms.InsertOne(context.Background(), bson.D{
		{Key: "id", Value: room.ID},
		{Key: "name", Value: room.Name},
		{Key: "owner_id", Value: room.OwnerID},
		{Key: "persistent", Value: room.Persistent},
		{Key: "created_at", Value: room.CreatedAt},
	})
	if err != nil {
		t.Fatalf("could not write the room: %v", err)
	}
}

func TestRoomsComeBackOldestFirst(t *testing.T) {
	t.Parallel()
	store := testStore(t)

	writeRoom(t, store, Room{ID: "9FF0D8", Name: "retro", OwnerID: "id-bruno",
		Persistent: true, CreatedAt: 2000})
	writeRoom(t, store, Room{ID: "8F42A1", Name: "standup", OwnerID: "id-ana",
		Persistent: true, CreatedAt: 1000})

	rooms, err := store.Rooms(context.Background())
	if err != nil {
		t.Fatalf("Rooms: %v", err)
	}
	if len(rooms) != 2 {
		t.Fatalf("expected two rooms, got %d", len(rooms))
	}
	// The server's own list uses this order, and the screen shows it unsorted.
	if rooms[0].ID != "8F42A1" || rooms[1].ID != "9FF0D8" {
		t.Fatalf("expected the oldest first, got %s then %s", rooms[0].ID, rooms[1].ID)
	}
	if rooms[0].Name != "standup" || rooms[0].OwnerID != "id-ana" {
		t.Fatalf("the document did not decode into the struct: %+v", rooms[0])
	}
	if !rooms[0].Persistent {
		t.Fatalf("persistent did not survive the round trip: %+v", rooms[0])
	}
}

func TestAnEmptyRoomsCollectionIsNotAnError(t *testing.T) {
	t.Parallel()
	store := testStore(t)

	rooms, err := store.Rooms(context.Background())
	if err != nil {
		t.Fatalf("Rooms: %v", err)
	}
	if len(rooms) != 0 {
		t.Fatalf("expected nothing, got %d rooms", len(rooms))
	}
}

func TestDeletingARoomRemovesItAndIsRecorded(t *testing.T) {
	t.Parallel()
	store := testStore(t)

	writeRoom(t, store, Room{ID: "8F42A1", Name: "standup", OwnerID: "id-ana",
		Persistent: true, CreatedAt: 1000})
	writeRoom(t, store, Room{ID: "9FF0D8", Name: "retro", OwnerID: "id-bruno",
		Persistent: true, CreatedAt: 2000})

	if err := store.DeleteRoom(context.Background(), "8F42A1"); err != nil {
		t.Fatalf("DeleteRoom: %v", err)
	}

	rooms, err := store.Rooms(context.Background())
	if err != nil {
		t.Fatalf("Rooms: %v", err)
	}
	if len(rooms) != 1 || rooms[0].ID != "9FF0D8" {
		t.Fatalf("the wrong room went: %+v", rooms)
	}

	entries, err := store.Audit(context.Background(), AuditQuery{})
	if err != nil {
		t.Fatalf("Audit: %v", err)
	}
	if len(entries) != 1 {
		t.Fatalf("expected one entry, got %d", len(entries))
	}
	entry := entries[0]
	if entry.Action != ActionDeleteRoom {
		t.Fatalf("action was %q", entry.Action)
	}
	// Both, which is what the server writes for its own delete_room. An entry
	// the two programs disagree on is one somebody has to know the origin of
	// before they can read it.
	if entry.TargetID != "8F42A1" || entry.RoomID != "8F42A1" {
		t.Fatalf("target=%q room=%q", entry.TargetID, entry.RoomID)
	}
	if entry.Detail != "name=standup" {
		t.Fatalf("detail was %q", entry.Detail)
	}
	if entry.ActorUsername != "test (dbadmin)" {
		t.Fatalf("the entry was attributed to %q", entry.ActorUsername)
	}
}

func TestDeletingARoomThatIsNotThereIsAnError(t *testing.T) {
	t.Parallel()
	store := testStore(t)

	err := store.DeleteRoom(context.Background(), "ABCDEF")
	if !errors.Is(err, ErrRoomNotFound) {
		t.Fatalf("expected ErrRoomNotFound, got %v", err)
	}

	// And nothing was written about a deletion that did not happen.
	entries, auditErr := store.Audit(context.Background(), AuditQuery{})
	if auditErr != nil {
		t.Fatalf("Audit: %v", auditErr)
	}
	if len(entries) != 0 {
		t.Fatalf("a refused deletion still wrote %d entries", len(entries))
	}
}

func TestTheSummaryCountsTheRooms(t *testing.T) {
	t.Parallel()
	store := testStore(t)

	writeRoom(t, store, Room{ID: "8F42A1", Name: "standup", CreatedAt: 1000})
	writeRoom(t, store, Room{ID: "9FF0D8", Name: "retro", CreatedAt: 2000})

	summary, err := store.Summary(context.Background())
	if err != nil {
		t.Fatalf("Summary: %v", err)
	}
	if summary.Rooms != 2 {
		t.Fatalf("expected two rooms in the summary, got %d", summary.Rooms)
	}
}

// A room the server wrote before `persistent` or `created_at` existed, or one
// somebody put there by hand. The reader has to answer with a room rather than
// with an error, the same way restrictions_from does on the C++ side.
func TestARoomWithFieldsMissingStillReads(t *testing.T) {
	t.Parallel()
	store := testStore(t)

	_, err := store.rooms.InsertOne(context.Background(), bson.D{
		{Key: "id", Value: "8F42A1"},
		{Key: "name", Value: "standup"},
	})
	if err != nil {
		t.Fatalf("could not write the room: %v", err)
	}

	rooms, listErr := store.Rooms(context.Background())
	if listErr != nil {
		t.Fatalf("Rooms: %v", listErr)
	}
	if len(rooms) != 1 {
		t.Fatalf("expected one room, got %d", len(rooms))
	}
	if rooms[0].ID != "8F42A1" || rooms[0].CreatedAt != 0 || rooms[0].Persistent {
		t.Fatalf("the missing fields did not read as zero: %+v", rooms[0])
	}
}
