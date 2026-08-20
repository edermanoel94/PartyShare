// Package store is the database half of dbadmin: the documents the PartyShare
// server keeps in MongoDB, and the operations an administrator performs on
// them.
//
// The shapes below are the Go transcription of what server/src/store writes.
// They are deliberately literal, field name for field name, because a document
// this tool writes has to be one the C++ server reads back without noticing
// which program wrote it.
package store

import "time"

// Role is what an account is allowed to do, section 18 of SPEC.md.
type Role string

const (
	// RoleUser joins rooms, creates rooms, shares a screen, mutes themselves.
	RoleUser Role = "user"
	// RoleAdmin is everything above, plus removing and muting other
	// participants and managing the accounts and the rooms.
	RoleAdmin Role = "admin"
)

// Roles is every role, in the order a form offers them.
var Roles = []Role{RoleUser, RoleAdmin}

// RoleFromString is the inverse of the constants above.
//
// Anything unrecognised is RoleUser, which is the same answer
// dv::models::role_from_string gives and for the same reason: the safe reading
// of a value nobody understands is the role that can do less.
func RoleFromString(name string) Role {
	if Role(name) == RoleAdmin {
		return RoleAdmin
	}
	return RoleUser
}

// Account is one document of the users collection.
//
// The salt and the hash are carried because this program is the one place
// allowed to write them. Nothing renders them: see the fields the users table
// shows in the UI package.
type Account struct {
	UserID          string `bson:"user_id"`
	Username        string `bson:"username"`
	DisplayName     string `bson:"display_name"`
	Avatar          string `bson:"avatar"`
	Role            Role   `bson:"role"`
	SaltHex         string `bson:"salt_hex"`
	PasswordHashHex string `bson:"password_hash_hex"`
	// Seconds since the Unix epoch, UTC, stamped when the account was created.
	CreatedAt int64 `bson:"created_at"`
}

// IsAdmin reports whether the account holds the administrator role.
func (a Account) IsAdmin() bool { return a.Role == RoleAdmin }

// Created is CreatedAt as a local time. The zero second, which is what an
// account written by hand without the field decodes to, comes back as the zero
// time so that a caller can tell it apart from a real date.
func (a Account) Created() time.Time {
	if a.CreatedAt == 0 {
		return time.Time{}
	}
	return time.Unix(a.CreatedAt, 0).Local()
}

// AuditEntry is one document of the audit collection: one administrative
// action, as dv::models::AuditEntry defines it.
type AuditEntry struct {
	// Assigned by MongoDB. Rendered short, because its only use here is to
	// tell two entries of the same second apart.
	ID            string `bson:"-"`
	ActorID       string `bson:"actor_id"`
	ActorUsername string `bson:"actor_username"`
	// What was done: "kick", "force_mute", "force_unmute", "create_user",
	// "update_user", "delete_user", "delete_room", "create_room".
	Action string `bson:"action"`
	// Who or what it was done to: a user id, or a room id for room actions.
	TargetID string `bson:"target_id"`
	// Where it happened, when the action belongs to a room.
	RoomID string `bson:"room_id"`
	// Free text: the reason given for a kick, the role a user was moved to.
	Detail string `bson:"detail"`
	// Seconds since the Unix epoch, UTC.
	TimestampSeconds int64 `bson:"timestamp_seconds"`
}

// When is TimestampSeconds as a local time, zero for an entry that has none.
func (e AuditEntry) When() time.Time {
	if e.TimestampSeconds == 0 {
		return time.Time{}
	}
	return time.Unix(e.TimestampSeconds, 0).Local()
}

// Actor is who this program acts as when it writes to the audit log.
//
// Not an account: whoever runs a database tool has a shell, not a session, and
// inventing an account for them would put a login in the users collection that
// nobody can use. The identifier is a constant prefix plus the operating
// system user, so that entries written here are one filter away from being
// separated from the ones the server wrote.
type Actor struct {
	ID       string
	Username string
}

// The three actions this program is allowed to record. They are the server's
// own names on purpose: a reader of the audit log should not have to learn a
// second vocabulary to understand an entry written from a terminal.
const (
	ActionCreateUser = "create_user"
	ActionUpdateUser = "update_user"
	ActionDeleteUser = "delete_user"
)

// Summary is the count line at the top of the screen.
type Summary struct {
	Users        int64
	Admins       int64
	AuditEntries int64
}
