// Package e2e drives the whole program, as a person does, against a real
// MongoDB.
//
// The screens are covered against a fake and the store against a database, and
// each of those halves can pass while the seam between them does not: a form
// that fills a field the store ignores, a create the screen reports and the
// database never received. This is the test that would notice, and it is also
// the one that says the account a terminal just created is an account the C++
// server can authenticate.
//
// It runs when DBADMIN_TEST_MONGO_URI names a MongoDB it may write to, and
// skips otherwise. tools/dbadmin/README.md says how to start a throwaway one.
package e2e

import (
	"context"
	"io"
	"os"
	"strings"
	"testing"
	"time"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/x/exp/teatest"
	"go.mongodb.org/mongo-driver/v2/bson"
	"go.mongodb.org/mongo-driver/v2/mongo"
	"go.mongodb.org/mongo-driver/v2/mongo/options"

	"github.com/edermanoel94/PartyShare/tools/dbadmin/internal/store"
	"github.com/edermanoel94/PartyShare/tools/dbadmin/internal/ui"
)

const (
	uriVariable = "DBADMIN_TEST_MONGO_URI"
	waitTimeout = 20 * time.Second
)

type session struct {
	model  *teatest.TestModel
	output io.Reader
	frames strings.Builder
}

func (s *session) drawn() string {
	written, _ := io.ReadAll(s.output)
	s.frames.Write(written)
	return s.frames.String()
}

func (s *session) awaits(t *testing.T, want string) {
	t.Helper()
	deadline := time.Now().Add(waitTimeout)
	for time.Now().Before(deadline) {
		if strings.Contains(s.drawn(), want) {
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatalf("%q was never drawn. What was:\n%s", want, s.drawn())
}

func (s *session) press(names ...string) {
	for _, name := range names {
		switch name {
		case "enter":
			s.model.Send(tea.KeyMsg{Type: tea.KeyEnter})
		case "tab":
			s.model.Send(tea.KeyMsg{Type: tea.KeyTab})
		case "esc":
			s.model.Send(tea.KeyMsg{Type: tea.KeyEsc})
		case "right":
			s.model.Send(tea.KeyMsg{Type: tea.KeyRight})
		default:
			s.model.Send(tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune(name)})
		}
	}
}

// open connects to a database of its own and hands back a program over it.
func open(t *testing.T) (*store.Store, *session) {
	t.Helper()

	uri := os.Getenv(uriVariable)
	if uri == "" {
		t.Skipf("set %s to a MongoDB this test may write to", uriVariable)
	}

	// Random, for the reason the store's own tests give: a name taken from
	// the clock is not unique between tests that start together.
	suffix, err := store.RandomHex(8)
	if err != nil {
		t.Fatalf("RandomHex: %v", err)
	}
	name := "dbadmin_e2e_" + suffix
	database, err := store.Open(context.Background(), store.Config{
		URI:      uri,
		Database: name,
		Timeout:  10 * time.Second,
		Actor:    store.Actor{ID: "dbadmin:e2e", Username: "e2e (dbadmin)"},
	})
	if err != nil {
		t.Fatalf("Open: %v", err)
	}

	t.Cleanup(func() {
		client, err := mongo.Connect(options.Client().ApplyURI(uri))
		if err != nil {
			t.Errorf("could not connect to drop %s: %v", name, err)
			return
		}
		defer func() { _ = client.Disconnect(context.Background()) }()
		if err := client.Database(name).Drop(context.Background()); err != nil {
			t.Errorf("could not drop %s: %v", name, err)
		}
		if err := database.Close(); err != nil {
			t.Errorf("Close: %v", err)
		}
	})

	model := teatest.NewTestModel(t, ui.New(database), teatest.WithInitialTermSize(120, 32))
	return database, &session{model: model, output: model.Output()}
}

// The whole of what this program is for, in one run: an empty database, an
// administrator created through the form, the account read back as the server
// would read it, and the entry that says who did it.
func TestAnAdministratorIsCreatedAndRecorded(t *testing.T) {
	database, screen := open(t)
	screen.awaits(t, "No accounts in this database yet")

	screen.press("n")
	screen.awaits(t, "New account")
	screen.model.Type("ana")
	screen.press("tab")
	screen.model.Type("a good password")
	screen.press("tab")
	screen.model.Type("a good password")
	screen.press("tab")
	screen.model.Type("Ana Souza")
	screen.press("tab")
	screen.press("tab")
	screen.press("right") // admin
	screen.press("enter")

	screen.awaits(t, "was created as admin")
	// The list was read back from the database, so this is the row as stored.
	screen.awaits(t, "Ana Souza")

	accounts, err := database.Accounts(context.Background())
	if err != nil {
		t.Fatalf("Accounts: %v", err)
	}
	if len(accounts) != 1 {
		t.Fatalf("the database holds %d accounts, want 1", len(accounts))
	}
	created := accounts[0]
	if created.Username != "ana" || created.DisplayName != "Ana Souza" {
		t.Errorf("the account was stored as %+v", created)
	}
	if !created.IsAdmin() {
		t.Error("the account is not an administrator, so nobody can administer this server")
	}

	// The point of the whole program: a login the C++ server accepts. It
	// derives the candidate with its own scrypt parameters and compares it to
	// what is stored, so a hash derived any other way is a password that never
	// works.
	derived, err := store.DeriveKeyHex("a good password", created.SaltHex)
	if err != nil {
		t.Fatalf("DeriveKeyHex: %v", err)
	}
	if derived != created.PasswordHashHex {
		t.Error("the stored credentials do not verify the password that was typed")
	}

	entries, err := database.Audit(context.Background(), store.AuditQuery{})
	if err != nil {
		t.Fatalf("Audit: %v", err)
	}
	if len(entries) != 1 {
		t.Fatalf("the log holds %d entries, want 1", len(entries))
	}
	entry := entries[0]
	if entry.Action != store.ActionCreateUser || entry.TargetID != created.UserID {
		t.Errorf("the entry is %+v, want a create_user against the new account", entry)
	}
	if entry.ActorUsername != "e2e (dbadmin)" {
		t.Errorf("the entry is attributed to %q, want the operator at the keyboard",
			entry.ActorUsername)
	}

	// And the screen shows it, from the database and not from memory. 3 rather
	// than tab: the rooms screen sits between the two now, and this test is
	// about the log rather than about how you get to it.
	screen.press("3")
	screen.awaits(t, "create_user")
	screen.awaits(t, "username=ana role=admin")
}

// The rule that makes this program the way back into a server nobody can
// administer, and the one it must not break itself.
func TestTheLastAdministratorIsRefusedOnTheScreen(t *testing.T) {
	database, screen := open(t)

	if _, err := database.CreateAccount(context.Background(), store.NewAccount{
		Username: "ana", Password: "one", Role: store.RoleAdmin,
	}); err != nil {
		t.Fatalf("CreateAccount: %v", err)
	}
	screen.press("r")
	screen.awaits(t, "ana")

	screen.press("d")
	screen.awaits(t, "Delete ana?")
	screen.press("y")
	screen.awaits(t, "the last administrator cannot be removed or demoted")

	accounts, err := database.Accounts(context.Background())
	if err != nil {
		t.Fatalf("Accounts: %v", err)
	}
	if len(accounts) != 1 {
		t.Fatalf("the refusal did not hold: the database holds %d accounts", len(accounts))
	}
}

// A password changed from a terminal has to be a password the server accepts,
// and the account has to keep everything else it had.
func TestChangingAPasswordThroughTheScreen(t *testing.T) {
	database, screen := open(t)

	created, err := database.CreateAccount(context.Background(), store.NewAccount{
		Username: "bruno", Password: "the old one", DisplayName: "Bruno Lima",
	})
	if err != nil {
		t.Fatalf("CreateAccount: %v", err)
	}
	screen.press("r")
	screen.awaits(t, "bruno")

	screen.press("p")
	screen.awaits(t, "Password of bruno")
	screen.model.Type("the new one")
	screen.press("tab")
	screen.model.Type("the new one")
	screen.press("enter")
	screen.awaits(t, "was changed")

	stored, err := database.FindAccount(context.Background(), created.UserID)
	if err != nil {
		t.Fatalf("FindAccount: %v", err)
	}
	derived, err := store.DeriveKeyHex("the new one", stored.SaltHex)
	if err != nil {
		t.Fatalf("DeriveKeyHex: %v", err)
	}
	if derived != stored.PasswordHashHex {
		t.Error("the new password does not verify against what was stored")
	}
	if stored.DisplayName != "Bruno Lima" || stored.CreatedAt != created.CreatedAt {
		t.Errorf("the account lost something it was not asked to change: %+v", stored)
	}
}

// The document, exactly as the server's own reader expects to find it. The
// store's tests check the same thing; this one checks it after the form, which
// is the path an operator actually uses.
func TestTheDocumentTheFormWritesIsTheServersDocument(t *testing.T) {
	database, screen := open(t)
	screen.awaits(t, "No accounts in this database yet")

	screen.press("n")
	screen.awaits(t, "New account")
	screen.model.Type("carla")
	screen.press("tab")
	screen.model.Type("x")
	screen.press("tab")
	screen.model.Type("x")
	screen.press("enter")
	screen.awaits(t, "was created")

	client, err := mongo.Connect(options.Client().ApplyURI(os.Getenv(uriVariable)))
	if err != nil {
		t.Fatalf("Connect: %v", err)
	}
	defer func() { _ = client.Disconnect(context.Background()) }()

	var document bson.M
	err = client.Database(database.Database()).Collection("users").
		FindOne(context.Background(), bson.D{{Key: "username", Value: "carla"}}).
		Decode(&document)
	if err != nil {
		t.Fatalf("could not read the document: %v", err)
	}

	for _, field := range []string{
		"user_id", "username", "display_name", "avatar", "role",
		"salt_hex", "password_hash_hex", "created_at",
	} {
		if _, present := document[field]; !present {
			t.Errorf("the document has no %q, so the server reads it as empty", field)
		}
	}
	if document["display_name"] != "carla" {
		t.Errorf("display_name is %v, want the username the form fell back to",
			document["display_name"])
	}
}
