package ui

import (
	"context"
	"errors"
	"sync"
	"time"

	"github.com/edermanoel94/PartyShare/tools/dbadmin/internal/store"
)

// fakeDatabase is a Database that answers from memory.
//
// It is not a second implementation of the rules: what the last administrator
// rule does, and what a document looks like on disk, is settled by the tests
// in the store package against a real MongoDB. This one exists so that the
// screens can be driven by a keyboard in a test that takes milliseconds, and
// so that the error paths, which a real database will not produce on demand,
// can be asked for.
type fakeDatabase struct {
	mutex    sync.Mutex
	accounts []store.Account
	entries  []store.AuditEntry

	// Set by a test to make the next change fail. Its own field rather than a
	// per method one, because every screen reports a failure the same way and
	// the tests only need one of them at a time.
	failure error

	// What was asked of it, in order, so that a test can say "the screen sent
	// a create with this role" rather than inferring it from a redraw.
	created    []store.NewAccount
	updated    []store.Account
	passwords  []string
	restricted []restrictionCall
	deleted    []string
	queries    []store.AuditQuery
}

// restrictionCall is one SetRestrictions, kept whole so that a test can say
// which of the four flags the screen actually sent rather than inferring it
// from a redraw.
type restrictionCall struct {
	userID       string
	restrictions store.Restrictions
}

func (f *fakeDatabase) Database() string   { return "partyshare_test" }
func (f *fakeDatabase) Endpoint() string   { return "127.0.0.1:27017" }
func (f *fakeDatabase) Actor() store.Actor { return store.Actor{ID: "dbadmin:test", Username: "test"} }
func (f *fakeDatabase) lock() func()       { f.mutex.Lock(); return f.mutex.Unlock }

func (f *fakeDatabase) Accounts(context.Context) ([]store.Account, error) {
	defer f.lock()()
	return append([]store.Account(nil), f.accounts...), nil
}

func (f *fakeDatabase) Summary(context.Context) (store.Summary, error) {
	defer f.lock()()
	summary := store.Summary{
		Users:        int64(len(f.accounts)),
		AuditEntries: int64(len(f.entries)),
	}
	for _, account := range f.accounts {
		if account.IsAdmin() {
			summary.Admins++
		}
	}
	return summary, nil
}

func (f *fakeDatabase) Audit(_ context.Context, query store.AuditQuery) ([]store.AuditEntry, error) {
	defer f.lock()()
	f.queries = append(f.queries, query)

	var entries []store.AuditEntry
	for _, entry := range f.entries {
		if query.ActorID == "" || entry.ActorID == query.ActorID {
			entries = append(entries, entry)
		}
	}
	return entries, nil
}

func (f *fakeDatabase) CreateAccount(
	_ context.Context, spec store.NewAccount) (store.Account, error) {
	defer f.lock()()
	f.created = append(f.created, spec)
	if f.failure != nil {
		return store.Account{}, f.failure
	}

	account := store.Account{
		UserID:      "id-" + spec.Username,
		Username:    spec.Username,
		DisplayName: spec.DisplayName,
		Avatar:      spec.Avatar,
		Role:        store.RoleFromString(string(spec.Role)),
		CreatedAt:   time.Now().Unix(),
	}
	if account.DisplayName == "" {
		account.DisplayName = account.Username
	}
	f.accounts = append(f.accounts, account)
	f.append(store.ActionCreateUser, account.UserID,
		"username="+account.Username+" role="+string(account.Role))
	return account, nil
}

func (f *fakeDatabase) UpdateAccount(_ context.Context, updated store.Account) error {
	defer f.lock()()
	f.updated = append(f.updated, updated)
	if f.failure != nil {
		return f.failure
	}
	for i, account := range f.accounts {
		if account.UserID == updated.UserID {
			f.accounts[i] = updated
		}
	}
	f.append(store.ActionUpdateUser, updated.UserID, "role="+string(updated.Role))
	return nil
}

func (f *fakeDatabase) SetPassword(_ context.Context, userID, password string) error {
	defer f.lock()()
	f.passwords = append(f.passwords, userID+" "+password)
	if f.failure != nil {
		return f.failure
	}
	f.append(store.ActionUpdateUser, userID, "password")
	return nil
}

func (f *fakeDatabase) SetRestrictions(
	_ context.Context, userID string, restrictions store.Restrictions) error {
	defer f.lock()()
	f.restricted = append(f.restricted, restrictionCall{userID: userID, restrictions: restrictions})
	if f.failure != nil {
		return f.failure
	}
	for i, account := range f.accounts {
		if account.UserID == userID {
			f.accounts[i].Restrictions = restrictions
		}
	}
	f.append(store.ActionRestrictUser, userID, restrictions.Describe())
	return nil
}

func (f *fakeDatabase) DeleteAccount(_ context.Context, userID string) error {
	defer f.lock()()
	f.deleted = append(f.deleted, userID)
	if f.failure != nil {
		return f.failure
	}
	kept := f.accounts[:0]
	for _, account := range f.accounts {
		if account.UserID != userID {
			kept = append(kept, account)
		}
	}
	f.accounts = kept
	f.append(store.ActionDeleteUser, userID, "username=gone")
	return nil
}

// append records an entry the way the real store does, newest first, so that
// the audit screen sees the order it renders.
func (f *fakeDatabase) append(action, targetID, detail string) {
	f.entries = append([]store.AuditEntry{{
		ID:               "entry-" + action + "-" + targetID,
		ActorID:          "dbadmin:test",
		ActorUsername:    "test",
		Action:           action,
		TargetID:         targetID,
		Detail:           detail,
		TimestampSeconds: time.Now().Unix(),
	}}, f.entries...)
}

// twoAccounts is the fixture most of the screen tests start from: one
// administrator and one plain user.
func twoAccounts() *fakeDatabase {
	now := time.Now().Unix()
	return &fakeDatabase{
		accounts: []store.Account{
			{
				UserID: "id-ana", Username: "ana", DisplayName: "Ana Souza",
				Role: store.RoleAdmin, CreatedAt: now - 3600,
			},
			{
				UserID: "id-bruno", Username: "bruno", DisplayName: "Bruno Lima",
				Role: store.RoleUser, CreatedAt: now - 60,
			},
		},
		entries: []store.AuditEntry{{
			ID: "entry-1", ActorID: "id-ana", ActorUsername: "ana",
			Action: "create_user", TargetID: "id-bruno",
			Detail: "username=bruno role=user", TimestampSeconds: now - 60,
		}},
	}
}

var errDatabaseUnreachable = errors.New("could not reach the database")
