// Package store is the database half of dbadmin: the documents the PartyShare
// server keeps in MongoDB, and the operations an administrator performs on
// them.
//
// The shapes below are the Go transcription of what server/src/store writes.
// They are deliberately literal, field name for field name, because a document
// this tool writes has to be one the C++ server reads back without noticing
// which program wrote it.
package store

import (
	"strings"
	"time"
)

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

// Restrictions is what an administrator has taken away from an account until
// they give it back: the Go transcription of dv::models::Restrictions.
//
// A subdocument of the account and not four top level fields, so that one $set
// writes the lot. Writing them separately would let a failure land halfway and
// leave an account that is banned and not silenced when the operator asked for
// both, and it is what makes this program's write and the server's the same
// operation on the same shape.
//
// Every flag is false by default, so an account written before this existed is
// an account with nothing taken away. The same reading the server gives a
// missing role: the safe answer to a field nobody wrote is the one that takes
// nothing away.
type Restrictions struct {
	// Banned cannot sign in. The lasting form of a kick, and the one this
	// program can apply: a kick is about a room a running server holds in
	// memory, and this is about the account.
	Banned bool `bson:"banned"`
	// Muted cannot transmit audio. They arrive in a room already muted, by the
	// administrator rather than by themselves, so the mute holds.
	Muted bool `bson:"muted"`
	// Silenced cannot write in a room's chat. Reading it is untouched.
	Silenced bool `bson:"silenced"`
	// ScreenShareBlocked cannot start a screen share, and one already running
	// is stopped by the server the moment it reads this.
	ScreenShareBlocked bool `bson:"screen_share_blocked"`
}

// Any reports whether anything at all is taken away, which is what decides
// between drawing a row of names and drawing nothing.
func (r Restrictions) Any() bool {
	return r.Banned || r.Muted || r.Silenced || r.ScreenShareBlocked
}

// Describe is the restrictions as a line of their wire names, "banned muted",
// and an empty string when there are none.
//
// The same order and the same words as dv::models::describe, so that a line
// read here and a line read in the client's panel are the same line.
func (r Restrictions) Describe() string {
	var parts []string
	if r.Banned {
		parts = append(parts, "banned")
	}
	if r.Muted {
		parts = append(parts, "muted")
	}
	if r.Silenced {
		parts = append(parts, "silenced")
	}
	if r.ScreenShareBlocked {
		parts = append(parts, "screen_share_blocked")
	}
	return strings.Join(parts, " ")
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
	// What an administrator has taken away. Absent from a document written
	// before restrictions existed, which decodes to the zero value and is
	// exactly right: nothing taken away.
	Restrictions Restrictions `bson:"restrictions"`
	// When an operator asked, from here, for the session this account is
	// holding to end, and zero while nobody has. The one field of the account
	// this program writes and the server only reads: a running server ends
	// the session on its next heartbeat and zeroes it, and a login that
	// arrives after it zeroes it too, because a request written before
	// somebody signed in was about a session that had already ended.
	//
	// Omitted when zero so that an account created here is the document the
	// server's own create writes, field for field.
	SessionEndRequestedAt int64 `bson:"session_end_requested_at,omitempty"`
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
	// What was done: "kick", "force_mute", "force_unmute", "restrict_user",
	// "create_user", "update_user", "delete_user", "delete_room",
	// "create_room", "send_notice", "acknowledge_notice", "end_session".
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

// The seven actions this program is allowed to record. They are the server's
// own names on purpose: a reader of the audit log should not have to learn a
// second vocabulary to understand an entry written from a terminal. The last
// is the one the server never writes itself - from the panel, signing
// somebody out is a `kick` from a room or a ban, and `end_session` is the
// third thing, which only a program with no room to name has any use for.
const (
	ActionCreateUser   = "create_user"
	ActionUpdateUser   = "update_user"
	ActionDeleteUser   = "delete_user"
	ActionRestrictUser = "restrict_user"
	ActionDeleteRoom   = "delete_room"
	ActionSendNotice   = "send_notice"
	ActionEndSession   = "end_session"
)

// Summary is the count line at the top of the screen.
type Summary struct {
	Users        int64
	Admins       int64
	Rooms        int64
	AuditEntries int64
	// Sessions is every session document, open and closed alike, which is what
	// the tab bar counts for the other three as well. How many of them are
	// somebody who is online right now is a question about time, and the
	// sessions screen answers it on its own status line rather than here: a
	// number in the tab bar that changed while nobody did anything would read
	// as the tool being unable to count.
	Sessions int64
}
