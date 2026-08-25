package ui

import (
	"strings"
	"testing"
)

func TestTheRoomsScreenShowsWhatOutlivesTheServer(t *testing.T) {
	t.Parallel()
	screen := start(t, roomsAndAccounts())
	screen.awaits(t, "Ana Souza")

	screen.press("2")
	screen.awaits(t, "8F42A1")
	screen.awaits(t, "standup")
	// The owner is a user id in the document and a username on the screen.
	screen.awaits(t, "ana")

	final := screen.finish(t)
	if !strings.Contains(final, "9FF0D8") {
		t.Fatalf("the second room was never drawn:\n%s", final)
	}
	// An owner whose account is gone shows as the identifier rather than as a
	// blank, which would read like a bug in the column.
	if !strings.Contains(final, "id-nobod") {
		t.Fatalf("a room whose owner was deleted should still name it:\n%s", final)
	}
}

func TestTheRoomsTabCountsWhatIsInTheDatabase(t *testing.T) {
	t.Parallel()
	screen := start(t, roomsAndAccounts())
	screen.press("2")
	screen.awaits(t, "Rooms (2)")
	screen.awaits(t, "2 rooms")
	screen.finish(t)
}

func TestDeletingARoomAsksFirstAndSaysWhatItDoesNotDo(t *testing.T) {
	t.Parallel()
	database := roomsAndAccounts()
	screen := start(t, database)
	screen.press("2")
	screen.awaits(t, "8F42A1")

	screen.press("d")
	screen.awaits(t, "Delete room 8F42A1?")
	// The warning is the reason the card exists: a document disappearing is
	// not the server closing a room.
	screen.awaits(t, "nobody is evicted")

	screen.press("y")
	screen.awaits(t, "Room 8F42A1 was deleted")
	screen.finish(t)

	if len(database.deletedRooms) != 1 || database.deletedRooms[0] != "8F42A1" {
		t.Fatalf("the screen deleted %v", database.deletedRooms)
	}
	if len(database.deleted) != 0 {
		t.Fatalf("it deleted accounts as well: %v", database.deleted)
	}
}

func TestAnsweringNoLeavesTheRoomAlone(t *testing.T) {
	t.Parallel()
	database := roomsAndAccounts()
	screen := start(t, database)
	screen.press("2")
	screen.awaits(t, "8F42A1")

	screen.press("d")
	screen.awaits(t, "Delete room 8F42A1?")
	screen.press("n")
	screen.awaits(t, "8F42A1")
	screen.finish(t)

	if len(database.deletedRooms) != 0 {
		t.Fatalf("a refused confirmation still deleted %v", database.deletedRooms)
	}
}

func TestADatabaseThatRefusesTheDeletionSaysSo(t *testing.T) {
	t.Parallel()
	database := roomsAndAccounts()
	database.failure = errDatabaseUnreachable
	screen := start(t, database)
	screen.press("2")
	screen.press("d")
	screen.press("y")
	screen.awaits(t, "could not reach the database")
	screen.finish(t)
}

func TestTheRoomFilterNarrowsByNameAndByOwner(t *testing.T) {
	t.Parallel()
	screen := start(t, roomsAndAccounts())
	screen.press("2")
	screen.awaits(t, "8F42A1")

	screen.press("/")
	for _, letter := range "retro" {
		screen.press(string(letter))
	}
	screen.press("enter")
	screen.awaits(t, "9FF0D8")

	final := screen.finish(t)
	if strings.Contains(final, "8F42A1") {
		t.Fatalf("the filter left a room that does not match:\n%s", final)
	}
}

// The single letter shortcuts belong to the list. Inside the filter they are
// text, which is the same rule the users screen keeps.
func TestDIsTextWhileTheRoomFilterIsOpen(t *testing.T) {
	t.Parallel()
	database := roomsAndAccounts()
	screen := start(t, database)
	screen.press("2")
	screen.awaits(t, "8F42A1")

	screen.press("/")
	screen.press("d")
	screen.finish(t)

	if len(database.deletedRooms) != 0 {
		t.Fatalf("typing in the filter deleted %v", database.deletedRooms)
	}
}
