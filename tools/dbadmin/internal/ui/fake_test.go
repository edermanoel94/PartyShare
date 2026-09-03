package ui

import (
	"context"
	"errors"
	"sort"
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
	rooms    []store.Room
	sessions []store.Session
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
	// Room identifiers this screen asked to delete, kept apart from `deleted`
	// so that a test can tell a room deletion from an account one rather than
	// matching on the shape of an identifier.
	deletedRooms []string
	queries      []store.AuditQuery
	// The limits the sessions screen asked for, so that a test can say the l
	// key changed the query rather than only the help line.
	sessionLimits []int
	// The accounts the sessions screen asked to sign out, and the notices the
	// users screen sent, each kept whole so that a test can say what was
	// written rather than that something was.
	ended   []string
	notices []noticeCall
}

// noticeCall is one SendNotice as the screen made it.
type noticeCall struct {
	userID string
	text   string
}

// restrictionCall is one SetRestrictions, kept whole so that a test can say
// which of the four flags the screen actually sent rather than inferring it
// from a redraw.
type restrictionCall struct {
	userID       string
	restrictions store.Restrictions
}

// setFailure is how a test makes the next change fail once the screen is
// already up. Under the lock, because the reads the screen started at launch
// are still running on their own goroutines and look at the same field.
func (f *fakeDatabase) setFailure(err error) {
	defer f.lock()()
	f.failure = err
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
		Rooms:        int64(len(f.rooms)),
		AuditEntries: int64(len(f.entries)),
		Sessions:     int64(len(f.sessions)),
	}
	for _, account := range f.accounts {
		if account.IsAdmin() {
			summary.Admins++
		}
	}
	return summary, nil
}

// Sessions answers the rows in the order the real store's query produces them:
// everything still open first, newest heartbeat first within each group. The
// screen leans on that order - "who is online" has to be at the top of the
// first page - so a fake that handed them back in insertion order would be
// testing a screen nobody ships.
func (f *fakeDatabase) Sessions(_ context.Context, limit int) ([]store.Session, error) {
	defer f.lock()()
	if f.failure != nil {
		return nil, f.failure
	}
	f.sessionLimits = append(f.sessionLimits, limit)

	sessions := append([]store.Session(nil), f.sessions...)
	sort.SliceStable(sessions, func(i, j int) bool {
		if sessions[i].Open() != sessions[j].Open() {
			return sessions[i].Open()
		}
		return sessions[i].LastSeenAt > sessions[j].LastSeenAt
	})
	if clamped := store.ClampSessionLimit(limit); len(sessions) > clamped {
		sessions = sessions[:clamped]
	}
	return sessions, nil
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

func (f *fakeDatabase) Rooms(context.Context) ([]store.Room, error) {
	defer f.lock()()
	if f.failure != nil {
		return nil, f.failure
	}
	return append([]store.Room(nil), f.rooms...), nil
}

func (f *fakeDatabase) DeleteRoom(_ context.Context, roomID string) error {
	defer f.lock()()
	f.deletedRooms = append(f.deletedRooms, roomID)
	if f.failure != nil {
		return f.failure
	}
	kept := f.rooms[:0]
	for _, room := range f.rooms {
		if room.ID != roomID {
			kept = append(kept, room)
		}
	}
	f.rooms = kept
	f.append(store.ActionDeleteRoom, roomID, "name=gone")
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

func (f *fakeDatabase) SendNotice(_ context.Context, userID, text string) (store.Notice, error) {
	defer f.lock()()
	f.notices = append(f.notices, noticeCall{userID: userID, text: text})
	if f.failure != nil {
		return store.Notice{}, f.failure
	}
	notice := store.Notice{
		ID: "notice-" + userID, UserID: userID, Text: text, CreatedAt: time.Now().Unix(),
	}
	f.append(store.ActionSendNotice, userID, "notice="+notice.ID+" "+text)
	return notice, nil
}

func (f *fakeDatabase) EndSession(_ context.Context, userID string) error {
	defer f.lock()()
	f.ended = append(f.ended, userID)
	if f.failure != nil {
		return f.failure
	}
	for i, account := range f.accounts {
		if account.UserID == userID {
			f.accounts[i].SessionEndRequestedAt = time.Now().Unix()
		}
	}
	f.append(store.ActionEndSession, userID, "address=203.0.113.7")
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

// roomsAndAccounts is twoAccounts with rooms in it: one owned by an account
// that exists, and one whose owner has been deleted out from under it, which
// is the case the owner column has to say something sensible about.
func roomsAndAccounts() *fakeDatabase {
	database := twoAccounts()
	now := time.Now().Unix()
	database.rooms = []store.Room{
		{
			ID: "8F42A1", Name: "standup", OwnerID: "id-ana",
			Persistent: true, CreatedAt: now - 3600,
		},
		{
			ID: "9FF0D8", Name: "retro", OwnerID: "id-nobody",
			Persistent: true, CreatedAt: now - 120,
		},
	}
	return database
}

// sessionsAndAccounts is the three states a session can be in, over the two
// accounts twoAccounts creates plus one whose account is gone.
//
// All three, because the screen exists to tell them apart: one person here,
// one row a killed server left behind, and one visit that finished. A fixture
// with only the first would let the difference between "open" and "online"
// disappear without a test noticing.
func sessionsAndAccounts() *fakeDatabase {
	database := twoAccounts()
	now := time.Now()
	database.sessions = []store.Session{
		{
			UserID:      "id-ana",
			IP:          "203.0.113.7",
			ConnectedAt: now.Add(-time.Hour).Unix(),
			LastSeenAt:  now.Add(-2 * time.Second).Unix(),
		},
		{
			UserID:      "id-bruno",
			IP:          "203.0.113.8",
			ConnectedAt: now.Add(-3 * time.Hour).Unix(),
			LastSeenAt:  now.Add(-2 * time.Hour).Unix(),
		},
		{
			UserID:      "id-nobody",
			IP:          "198.51.100.4",
			ConnectedAt: now.Add(-5 * time.Hour).Unix(),
			LastSeenAt:  now.Add(-4 * time.Hour).Unix(),
			EndedAt:     now.Add(-4 * time.Hour).Unix(),
		},
	}
	return database
}

var errDatabaseUnreachable = errors.New("could not reach the database")
