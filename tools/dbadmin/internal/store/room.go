package store

// Room is a room as the database keeps it.
//
// No participant list, because there is none to keep: who is in a room right
// now is state of a running server process, and this program is looking at
// what outlives that process. The server's own store/room_store.hpp says the
// same thing from the other side.
type Room struct {
	// The six characters people read out to each other.
	ID string `bson:"id"`
	// What it was called when it was made. Not unique, and not an identifier.
	Name string `bson:"name"`
	// The account that created it. An identifier, not a username: resolving it
	// to a name is the screen's job, and an account that has been deleted
	// leaves a room whose owner cannot be resolved at all.
	OwnerID string `bson:"owner_id"`
	// True for every room the current server writes. Kept because the field is
	// in the documents and dropping it here would silently rewrite it away on
	// any future update, not because anything reads it.
	Persistent bool `bson:"persistent"`
	// Seconds since the Unix epoch, UTC.
	CreatedAt int64 `bson:"created_at"`
}
