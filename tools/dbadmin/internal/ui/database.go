package ui

import (
	"context"

	"github.com/edermanoel94/PartyShare/tools/dbadmin/internal/store"
)

// Database is everything the four screens ask of the database.
//
// An interface and not the concrete store for one reason: the interface is
// what lets the screens be driven, keystroke for keystroke, against a fake in
// a test that needs no MongoDB. The tests that do have one exercise the same
// screens over the real thing, so the fake buys speed in the common case
// rather than coverage nobody has.
//
// It is deliberately the whole surface, reads and writes together, rather than
// two interfaces: every screen that writes also reads back afterwards.
type Database interface {
	// What the header says about the connection.
	Database() string
	Endpoint() string
	Actor() store.Actor

	Accounts(ctx context.Context) ([]store.Account, error)
	Summary(ctx context.Context) (store.Summary, error)
	Rooms(ctx context.Context) ([]store.Room, error)
	// Sessions is read only and stays that way. Every other collection here
	// has a write beside its read, because an operator reaches for this
	// program to change something; this one records what the server saw, and a
	// presence history an administrator can edit is not evidence of anything.
	// Ending one is a write to the account, below, and never to a row here.
	Sessions(ctx context.Context, limit int) ([]store.Session, error)
	Audit(ctx context.Context, query store.AuditQuery) ([]store.AuditEntry, error)

	CreateAccount(ctx context.Context, spec store.NewAccount) (store.Account, error)
	UpdateAccount(ctx context.Context, updated store.Account) error
	SetPassword(ctx context.Context, userID, password string) error
	SetRestrictions(ctx context.Context, userID string, restrictions store.Restrictions) error
	DeleteAccount(ctx context.Context, userID string) error
	DeleteRoom(ctx context.Context, roomID string) error
	// The two things a running server does on this program's behalf, from a
	// document alone: hand somebody a notice, and sign somebody out.
	SendNotice(ctx context.Context, userID, text string) (store.Notice, error)
	EndSession(ctx context.Context, userID string) error
}

// The store is the implementation this program ships with. Checked here, at
// compile time, so that a method added to the interface fails the build rather
// than a call somewhere.
var _ Database = (*store.Store)(nil)
