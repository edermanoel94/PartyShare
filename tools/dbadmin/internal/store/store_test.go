package store

import (
	"context"
	"errors"
	"fmt"
	"os"
	"slices"
	"testing"
	"time"

	"go.mongodb.org/mongo-driver/v2/bson"
)

// The variable that turns these on. They write to a database, so they run
// against one somebody nominated and never against a default that might be the
// production one. tools/dbadmin/README.md says how to start a throwaway.
const uriVariable = "DBADMIN_TEST_MONGO_URI"

// testStore opens a store on a database of its own, dropped when the test
// ends. Its own and not a shared one, so that the tests can run in parallel
// and none of them has to clean up after another's failure.
func testStore(t *testing.T) *Store {
	t.Helper()

	uri := os.Getenv(uriVariable)
	if uri == "" {
		t.Skipf("set %s to a MongoDB these tests may write to", uriVariable)
	}

	// Random, and not a timestamp. These tests run in parallel, and the wall
	// clock two of them read on the way in is coarser than a nanosecond: two
	// databases named for the same instant are two tests writing to one
	// collection, and the one that finishes first drops it under the other.
	suffix, err := RandomHex(8)
	if err != nil {
		t.Fatalf("RandomHex: %v", err)
	}
	name := "dbadmin_test_" + suffix
	opened, err := Open(context.Background(), Config{
		URI:      uri,
		Database: name,
		Timeout:  10 * time.Second,
		Actor:    Actor{ID: "dbadmin:test", Username: "test (dbadmin)"},
	})
	if err != nil {
		t.Fatalf("Open: %v", err)
	}

	t.Cleanup(func() {
		if err := opened.client.Database(name).Drop(context.Background()); err != nil {
			t.Errorf("could not drop %s: %v", name, err)
		}
		if err := opened.Close(); err != nil {
			t.Errorf("Close: %v", err)
		}
	})
	return opened
}

// subdocument flattens a nested BSON value into a map, whichever of the two
// shapes the driver produced.
//
// A nested object inside a bson.M decodes to bson.D, which is an ordered slice
// and not a map: reading it by key needs this. The order is not part of what
// the server reads, so flattening it here is not throwing anything away.
func subdocument(t *testing.T, value any) map[string]any {
	t.Helper()
	switch nested := value.(type) {
	case bson.M:
		return nested
	case bson.D:
		flat := make(map[string]any, len(nested))
		for _, element := range nested {
			flat[element.Key] = element.Value
		}
		return flat
	default:
		t.Fatalf("expected a subdocument, got %T", value)
		return nil
	}
}

func mustCreate(t *testing.T, database *Store, spec NewAccount) Account {
	t.Helper()
	account, err := database.CreateAccount(context.Background(), spec)
	if err != nil {
		t.Fatalf("CreateAccount(%s): %v", spec.Username, err)
	}
	return account
}

// The account document is what the C++ server reads back. A field renamed here
// is not a broken test, it is an account the server cannot see: every reader in
// mongo_store.cpp is tolerant of a missing field and answers empty for it, so
// a misspelled "display_name" would produce an account with no name and a
// misspelled "role" would silently produce a plain user.
func TestCreateAccountWritesTheDocumentTheServerReads(t *testing.T) {
	t.Parallel()
	database := testStore(t)

	created := mustCreate(t, database, NewAccount{
		Username:    "ana",
		Password:    "a good password",
		DisplayName: "Ana Souza",
		Avatar:      "https://example.invalid/ana.png",
		Role:        RoleAdmin,
	})

	var document bson.M
	err := database.users.FindOne(context.Background(),
		bson.D{{Key: "user_id", Value: created.UserID}}).Decode(&document)
	if err != nil {
		t.Fatalf("could not read the document back: %v", err)
	}

	want := map[string]any{
		"user_id":           created.UserID,
		"username":          "ana",
		"display_name":      "Ana Souza",
		"avatar":            "https://example.invalid/ana.png",
		"role":              "admin",
		"salt_hex":          created.SaltHex,
		"password_hash_hex": created.PasswordHashHex,
	}
	for key, value := range want {
		if document[key] != value {
			t.Errorf("%s is %v, want %v", key, document[key], value)
		}
	}

	// int64 and not int32: dv::store reads it through int_field, which takes
	// either, but the server writes int64 and a mixed collection is a thing
	// somebody has to discover later.
	if _, isInt64 := document["created_at"].(int64); !isInt64 {
		t.Errorf("created_at is %T, want int64", document["created_at"])
	}

	// Always written, all four flags, even on an account with nothing taken
	// away. The server's own account_to_document does the same, and the point
	// is that a reader never has to tell "no restrictions" apart from "written
	// by a version that did not know about them".
	restrictions := subdocument(t, document["restrictions"])
	for _, flag := range []string{"banned", "muted", "silenced", "screen_share_blocked"} {
		value, isBool := restrictions[flag].(bool)
		if !isBool {
			t.Errorf("restrictions.%s is %T, want a bool", flag, restrictions[flag])
			continue
		}
		if value {
			t.Errorf("a new account arrived with %s already taken away", flag)
		}
	}
	if len(restrictions) != 4 {
		t.Errorf("restrictions carries %d fields, want the four the server reads", len(restrictions))
	}

	// Nothing this program invents. An extra field is not read by the server
	// and is exactly how two writers of one collection drift apart.
	for key := range document {
		if _, expected := want[key]; !expected &&
			key != "created_at" && key != "_id" && key != "restrictions" {
			t.Errorf("the document carries an unexpected field %q", key)
		}
	}

	// The credential the server will check on the next login.
	derived, err := DeriveKeyHex("a good password", created.SaltHex)
	if err != nil {
		t.Fatalf("DeriveKeyHex: %v", err)
	}
	if derived != created.PasswordHashHex {
		t.Error("the stored hash does not match the password, so the account cannot log in")
	}
}

func TestCreateAccountFillsInTheDisplayNameAndRefusesEmptyCredentials(t *testing.T) {
	t.Parallel()
	database := testStore(t)

	created := mustCreate(t, database, NewAccount{Username: "bruno", Password: "x"})
	if created.DisplayName != "bruno" {
		t.Errorf("display name is %q, want the username as the server's own fallback",
			created.DisplayName)
	}
	if created.Role != RoleUser {
		t.Errorf("role is %q, want the role that can do less", created.Role)
	}

	_, err := database.CreateAccount(context.Background(),
		NewAccount{Username: "carla", Password: ""})
	if !errors.Is(err, ErrEmptyCredentials) {
		t.Errorf("an empty password gave %v, want ErrEmptyCredentials", err)
	}
}

func TestCreateAccountRefusesADuplicateUsername(t *testing.T) {
	t.Parallel()
	database := testStore(t)

	mustCreate(t, database, NewAccount{Username: "ana", Password: "one"})
	_, err := database.CreateAccount(context.Background(),
		NewAccount{Username: "ana", Password: "another"})
	if !errors.Is(err, ErrUserExists) {
		t.Errorf("a duplicate username gave %v, want ErrUserExists", err)
	}
}

func TestEveryChangeIsRecorded(t *testing.T) {
	t.Parallel()
	database := testStore(t)

	created := mustCreate(t, database, NewAccount{
		Username: "ana", Password: "one", Role: RoleAdmin,
	})
	// A second administrator, so that the edits below are not refused by the
	// rule that keeps the last one in place.
	second := mustCreate(t, database, NewAccount{
		Username: "bruno", Password: "two", Role: RoleAdmin,
	})

	edited := second
	edited.Role = RoleUser
	edited.DisplayName = "Bruno Lima"
	if err := database.UpdateAccount(context.Background(), edited); err != nil {
		t.Fatalf("UpdateAccount: %v", err)
	}
	if err := database.SetPassword(context.Background(), second.UserID, "three"); err != nil {
		t.Fatalf("SetPassword: %v", err)
	}
	if err := database.DeleteAccount(context.Background(), second.UserID); err != nil {
		t.Fatalf("DeleteAccount: %v", err)
	}

	entries, err := database.Audit(context.Background(), AuditQuery{})
	if err != nil {
		t.Fatalf("Audit: %v", err)
	}

	// Two creates, an update, a password and a delete.
	if len(entries) != 5 {
		t.Fatalf("the log has %d entries, want 5", len(entries))
	}
	for _, entry := range entries {
		if entry.ActorID != "dbadmin:test" || entry.ActorUsername != "test (dbadmin)" {
			t.Errorf("entry %q is attributed to %q / %q, want the session actor",
				entry.Action, entry.ActorID, entry.ActorUsername)
		}
		if entry.TimestampSeconds == 0 {
			t.Errorf("entry %q has no timestamp", entry.Action)
		}
		if entry.ID == "" {
			t.Errorf("entry %q has no identifier", entry.Action)
		}
	}

	// Gathered and sorted rather than indexed. The five actions happen inside
	// one second, so the order the log returns them in is not something it
	// promises, and two of them are an update_user against the same account:
	// a map of one detail per action and target keeps whichever arrived last,
	// which is a test that passes or fails by coin toss.
	details := map[string][]string{}
	for _, entry := range entries {
		key := entry.Action + " " + entry.TargetID
		details[key] = append(details[key], entry.Detail)
	}
	want := map[string][]string{
		ActionCreateUser + " " + created.UserID: {"username=ana role=admin"},
		ActionCreateUser + " " + second.UserID:  {"username=bruno role=admin"},
		ActionUpdateUser + " " + second.UserID:  {"password", "role=user display_name"},
		ActionDeleteUser + " " + second.UserID:  {"username=bruno"},
	}
	for key, expected := range want {
		got := slices.Clone(details[key])
		slices.Sort(got)
		slices.Sort(expected)
		if !slices.Equal(got, expected) {
			t.Errorf("%s recorded %q, want %q", key, got, expected)
		}
	}
}

func TestUpdateAccountKeepsWhatItDoesNotOwn(t *testing.T) {
	t.Parallel()
	database := testStore(t)

	created := mustCreate(t, database, NewAccount{Username: "ana", Password: "one"})

	edited := created
	edited.Username = "ana.souza"
	edited.DisplayName = ""
	edited.CreatedAt = 0
	edited.SaltHex = "not a salt"
	edited.PasswordHashHex = "not a hash"
	if err := database.UpdateAccount(context.Background(), edited); err != nil {
		t.Fatalf("UpdateAccount: %v", err)
	}

	stored, err := database.FindAccount(context.Background(), created.UserID)
	if err != nil {
		t.Fatalf("FindAccount: %v", err)
	}
	if stored.Username != "ana.souza" {
		t.Errorf("username is %q, want the edited one", stored.Username)
	}
	if stored.DisplayName != "ana.souza" {
		t.Errorf("display name is %q, want the username it fell back to", stored.DisplayName)
	}
	// The two things an edit form has no business rewriting: when the account
	// came into existence, and the credentials only SetPassword derives.
	if stored.CreatedAt != created.CreatedAt {
		t.Errorf("created_at moved from %d to %d", created.CreatedAt, stored.CreatedAt)
	}
	if stored.SaltHex != created.SaltHex || stored.PasswordHashHex != created.PasswordHashHex {
		t.Error("the credentials were overwritten by an edit")
	}
}

func TestUpdateAccountRefusesATakenUsername(t *testing.T) {
	t.Parallel()
	database := testStore(t)

	mustCreate(t, database, NewAccount{Username: "ana", Password: "one"})
	bruno := mustCreate(t, database, NewAccount{Username: "bruno", Password: "two"})

	edited := bruno
	edited.Username = "ana"
	if err := database.UpdateAccount(context.Background(), edited); !errors.Is(err, ErrUserExists) {
		t.Errorf("renaming onto a taken username gave %v, want ErrUserExists", err)
	}

	// The same name it already has is not a collision with itself.
	unchanged := bruno
	unchanged.DisplayName = "Bruno"
	if err := database.UpdateAccount(context.Background(), unchanged); err != nil {
		t.Errorf("keeping the username gave %v", err)
	}
}

// The rule the server states in two places: a system with nobody able to
// administer it is a system that needs the database edited by hand to be
// recovered, and this program is that hand.
func TestTheLastAdministratorIsProtected(t *testing.T) {
	t.Parallel()
	database := testStore(t)

	only := mustCreate(t, database, NewAccount{
		Username: "ana", Password: "one", Role: RoleAdmin,
	})
	mustCreate(t, database, NewAccount{Username: "bruno", Password: "two"})

	demoted := only
	demoted.Role = RoleUser
	if err := database.UpdateAccount(context.Background(), demoted); !errors.Is(err, ErrLastAdmin) {
		t.Errorf("demoting the last administrator gave %v, want ErrLastAdmin", err)
	}
	if err := database.DeleteAccount(context.Background(), only.UserID); !errors.Is(
		err, ErrLastAdmin) {
		t.Errorf("deleting the last administrator gave %v, want ErrLastAdmin", err)
	}

	// The refusal is about the last one, not about administrators.
	second := mustCreate(t, database, NewAccount{
		Username: "carla", Password: "three", Role: RoleAdmin,
	})
	if err := database.DeleteAccount(context.Background(), second.UserID); err != nil {
		t.Errorf("deleting the second administrator gave %v", err)
	}
	if err := database.UpdateAccount(context.Background(), demoted); !errors.Is(
		err, ErrLastAdmin) {
		t.Errorf("after the second one is gone, demoting gave %v, want ErrLastAdmin", err)
	}
}

// The restrictions subdocument is what the server reads back through
// restrictions_from in mongo_store.cpp. A flag renamed here is not a broken
// test, it is a ban nobody is under: every reader there is tolerant of a
// missing field and answers false for it.
func TestSetRestrictionsWritesTheSubdocumentTheServerReads(t *testing.T) {
	t.Parallel()
	database := testStore(t)

	created := mustCreate(t, database, NewAccount{Username: "ana", Password: "one"})
	want := Restrictions{Banned: true, Silenced: true}
	if err := database.SetRestrictions(context.Background(), created.UserID, want); err != nil {
		t.Fatalf("SetRestrictions: %v", err)
	}

	var document bson.M
	err := database.users.FindOne(context.Background(),
		bson.D{{Key: "user_id", Value: created.UserID}}).Decode(&document)
	if err != nil {
		t.Fatalf("could not read the document back: %v", err)
	}
	restrictions := subdocument(t, document["restrictions"])
	for flag, value := range map[string]bool{
		"banned": true, "muted": false, "silenced": true, "screen_share_blocked": false,
	} {
		if restrictions[flag] != value {
			t.Errorf("restrictions.%s is %v, want %v", flag, restrictions[flag], value)
		}
	}

	// And it comes back through the reader this program uses, so the form that
	// opens next shows what is actually in force.
	back, err := database.FindAccount(context.Background(), created.UserID)
	if err != nil {
		t.Fatalf("FindAccount: %v", err)
	}
	if back.Restrictions != want {
		t.Errorf("read back %+v, want %+v", back.Restrictions, want)
	}
}

func TestSetRestrictionsRecordsWhatMoved(t *testing.T) {
	t.Parallel()
	database := testStore(t)

	created := mustCreate(t, database, NewAccount{Username: "ana", Password: "one"})
	if err := database.SetRestrictions(context.Background(), created.UserID,
		Restrictions{Silenced: true}); err != nil {
		t.Fatalf("SetRestrictions: %v", err)
	}
	// Only the flag that changed, then only the flag that changed back.
	if err := database.SetRestrictions(context.Background(), created.UserID,
		Restrictions{Silenced: true, Muted: true}); err != nil {
		t.Fatalf("SetRestrictions: %v", err)
	}

	entries, err := database.Audit(context.Background(), AuditQuery{})
	if err != nil {
		t.Fatalf("Audit: %v", err)
	}
	// Collected rather than indexed: the log is ordered by a wall clock in
	// whole seconds, and three writes inside one second have no order between
	// them. What is being checked is that each change named the flag that
	// moved and only that flag, which does not depend on which came back
	// first.
	var details []string
	for _, entry := range entries {
		if entry.Action != ActionRestrictUser {
			continue
		}
		if entry.TargetID != created.UserID {
			t.Errorf("an entry names %q, want the account", entry.TargetID)
		}
		details = append(details, entry.Detail)
	}
	slices.Sort(details)
	if !slices.Equal(details, []string{"muted=true", "silenced=true"}) {
		t.Errorf("the log recorded %q, want one entry per flag that moved", details)
	}
}

// A form submitted with the boxes it already had ticked is not an
// administrative action, and an audit log full of those is a log nobody reads.
func TestSetRestrictionsRecordsNothingWhenNothingMoved(t *testing.T) {
	t.Parallel()
	database := testStore(t)

	created := mustCreate(t, database, NewAccount{Username: "ana", Password: "one"})
	if err := database.SetRestrictions(context.Background(), created.UserID,
		Restrictions{}); err != nil {
		t.Fatalf("SetRestrictions: %v", err)
	}

	entries, err := database.Audit(context.Background(), AuditQuery{})
	if err != nil {
		t.Fatalf("Audit: %v", err)
	}
	if len(entries) != 1 || entries[0].Action != ActionCreateUser {
		t.Errorf("wrote %d entries, want only the create", len(entries))
	}
}

func TestTheLastAdministratorCannotBeBanned(t *testing.T) {
	t.Parallel()
	database := testStore(t)

	only := mustCreate(t, database, NewAccount{
		Username: "ana", Password: "one", Role: RoleAdmin,
	})
	if err := database.SetRestrictions(context.Background(), only.UserID,
		Restrictions{Banned: true}); !errors.Is(err, ErrLastAdmin) {
		t.Errorf("banning the last administrator gave %v, want ErrLastAdmin", err)
	}

	// The other three take nothing away that administering needs. An
	// administrator who may not use a microphone can still administer.
	if err := database.SetRestrictions(context.Background(), only.UserID,
		Restrictions{Muted: true, Silenced: true, ScreenShareBlocked: true}); err != nil {
		t.Errorf("restricting the last administrator otherwise gave %v", err)
	}

	// And with a second one there, banning the first is allowed.
	mustCreate(t, database, NewAccount{Username: "carla", Password: "two", Role: RoleAdmin})
	if err := database.SetRestrictions(context.Background(), only.UserID,
		Restrictions{Banned: true}); err != nil {
		t.Errorf("banning one of two administrators gave %v", err)
	}
}

func TestSetRestrictionsReportsAMissingAccount(t *testing.T) {
	t.Parallel()
	database := testStore(t)

	err := database.SetRestrictions(context.Background(), "nobody", Restrictions{Banned: true})
	if !errors.Is(err, ErrUserNotFound) {
		t.Errorf("restricting an account that is not there gave %v, want ErrUserNotFound", err)
	}
}

// The words this program writes and the words the server writes have to be the
// same words, because they end up in one audit log and on one screen.
func TestDescribeIsTheServersVocabulary(t *testing.T) {
	t.Parallel()

	if got := (Restrictions{}).Describe(); got != "" {
		t.Errorf("nothing taken away describes as %q, want empty", got)
	}
	all := Restrictions{Banned: true, Muted: true, Silenced: true, ScreenShareBlocked: true}
	if got := all.Describe(); got != "banned muted silenced screen_share_blocked" {
		t.Errorf("Describe is %q", got)
	}
	if !all.Any() || (Restrictions{}).Any() {
		t.Error("Any does not agree with what is set")
	}
}

// An account written before restrictions existed has no subdocument at all,
// which has to read as "nothing taken away". The opposite reading is an
// account nobody can log in to after an upgrade.
func TestAnAccountWithoutRestrictionsHasNothingTakenAway(t *testing.T) {
	t.Parallel()
	database := testStore(t)

	created := mustCreate(t, database, NewAccount{Username: "ana", Password: "one"})
	if _, err := database.users.UpdateOne(context.Background(),
		bson.D{{Key: "user_id", Value: created.UserID}},
		bson.D{{Key: "$unset", Value: bson.D{{Key: "restrictions", Value: ""}}}}); err != nil {
		t.Fatalf("could not remove the subdocument: %v", err)
	}

	back, err := database.FindAccount(context.Background(), created.UserID)
	if err != nil {
		t.Fatalf("FindAccount: %v", err)
	}
	if back.Restrictions.Any() {
		t.Errorf("an account with no subdocument read as %+v", back.Restrictions)
	}
}

func TestSetPasswordRotatesTheSalt(t *testing.T) {
	t.Parallel()
	database := testStore(t)

	created := mustCreate(t, database, NewAccount{Username: "ana", Password: "one"})
	if err := database.SetPassword(context.Background(), created.UserID, "two"); err != nil {
		t.Fatalf("SetPassword: %v", err)
	}

	stored, err := database.FindAccount(context.Background(), created.UserID)
	if err != nil {
		t.Fatalf("FindAccount: %v", err)
	}
	if stored.SaltHex == created.SaltHex {
		t.Error("the salt was reused, which is what a salt exists to prevent")
	}

	derived, err := DeriveKeyHex("two", stored.SaltHex)
	if err != nil {
		t.Fatalf("DeriveKeyHex: %v", err)
	}
	if derived != stored.PasswordHashHex {
		t.Error("the new password does not derive to the stored hash")
	}
	old, err := DeriveKeyHex("one", stored.SaltHex)
	if err != nil {
		t.Fatalf("DeriveKeyHex: %v", err)
	}
	if old == stored.PasswordHashHex {
		t.Error("the old password still verifies")
	}

	if err := database.SetPassword(context.Background(), "no such account", "x"); !errors.Is(
		err, ErrUserNotFound) {
		t.Errorf("setting the password of a missing account gave %v, want ErrUserNotFound", err)
	}
}

func TestAccountsAreOldestFirst(t *testing.T) {
	t.Parallel()
	database := testStore(t)

	// Written with the times spread out, because the ordering this checks is
	// invisible when three documents share a second.
	for index, username := range []string{"third", "first", "second"} {
		account := mustCreate(t, database, NewAccount{Username: username, Password: "x"})
		offsets := map[string]int64{"first": -300, "second": -200, "third": -100}
		_, err := database.users.UpdateOne(context.Background(),
			bson.D{{Key: "user_id", Value: account.UserID}},
			bson.D{{Key: "$set", Value: bson.D{
				{Key: "created_at", Value: time.Now().Unix() + offsets[username]},
			}}})
		if err != nil {
			t.Fatalf("could not backdate account %d: %v", index, err)
		}
	}

	accounts, err := database.Accounts(context.Background())
	if err != nil {
		t.Fatalf("Accounts: %v", err)
	}
	var got []string
	for _, account := range accounts {
		got = append(got, account.Username)
	}
	want := []string{"first", "second", "third"}
	if fmt.Sprint(got) != fmt.Sprint(want) {
		t.Errorf("the accounts came back as %v, want %v", got, want)
	}
}

func TestAuditIsNewestFirstAndFiltersByActor(t *testing.T) {
	t.Parallel()
	database := testStore(t)

	now := time.Now().Unix()
	seed := []AuditEntry{
		{ActorID: "one", ActorUsername: "ana", Action: "kick", TimestampSeconds: now - 30},
		{ActorID: "two", ActorUsername: "bruno", Action: "create_user", TimestampSeconds: now - 20},
		{ActorID: "one", ActorUsername: "ana", Action: "delete_room", TimestampSeconds: now - 10},
	}
	for _, entry := range seed {
		if _, err := database.audit.InsertOne(context.Background(), bson.D{
			{Key: "actor_id", Value: entry.ActorID},
			{Key: "actor_username", Value: entry.ActorUsername},
			{Key: "action", Value: entry.Action},
			{Key: "timestamp_seconds", Value: entry.TimestampSeconds},
		}); err != nil {
			t.Fatalf("could not seed the log: %v", err)
		}
	}

	entries, err := database.Audit(context.Background(), AuditQuery{})
	if err != nil {
		t.Fatalf("Audit: %v", err)
	}
	if len(entries) != 3 {
		t.Fatalf("read %d entries, want 3", len(entries))
	}
	for i := 1; i < len(entries); i++ {
		if entries[i-1].TimestampSeconds < entries[i].TimestampSeconds {
			t.Errorf("entry %d is older than the one before it, so the log is not newest first",
				i)
			break
		}
	}

	byActor, err := database.Audit(context.Background(), AuditQuery{ActorID: "one"})
	if err != nil {
		t.Fatalf("Audit: %v", err)
	}
	if len(byActor) != 2 {
		t.Fatalf("the actor filter read %d entries, want 2", len(byActor))
	}
	for _, entry := range byActor {
		if entry.ActorID != "one" {
			t.Errorf("the filter let through an entry by %q", entry.ActorID)
		}
	}

	// A limit is a page, and a page of one is the newest.
	limited, err := database.Audit(context.Background(), AuditQuery{Limit: 1})
	if err != nil {
		t.Fatalf("Audit: %v", err)
	}
	if len(limited) != 1 || limited[0].Action != "delete_room" {
		t.Errorf("a limit of one read %v, want the newest entry", limited)
	}
}

func TestDeleteAccountReportsAMissingOne(t *testing.T) {
	t.Parallel()
	database := testStore(t)

	if err := database.DeleteAccount(context.Background(), "no such account"); !errors.Is(
		err, ErrUserNotFound) {
		t.Errorf("deleting a missing account gave %v, want ErrUserNotFound", err)
	}
}

func TestSummaryCounts(t *testing.T) {
	t.Parallel()
	database := testStore(t)

	mustCreate(t, database, NewAccount{Username: "ana", Password: "x", Role: RoleAdmin})
	mustCreate(t, database, NewAccount{Username: "bruno", Password: "x"})

	summary, err := database.Summary(context.Background())
	if err != nil {
		t.Fatalf("Summary: %v", err)
	}
	if summary.Users != 2 || summary.Admins != 1 {
		t.Errorf("counted %d accounts and %d administrators, want 2 and 1",
			summary.Users, summary.Admins)
	}
	// One entry per account created.
	if summary.AuditEntries != 2 {
		t.Errorf("counted %d audit entries, want 2", summary.AuditEntries)
	}
}

func TestClampAuditLimit(t *testing.T) {
	t.Parallel()

	if got := ClampAuditLimit(0); got != DefaultAuditLimit {
		t.Errorf("ClampAuditLimit(0) is %d, want the default", got)
	}
	if got := ClampAuditLimit(-1); got != DefaultAuditLimit {
		t.Errorf("ClampAuditLimit(-1) is %d, want the default", got)
	}
	if got := ClampAuditLimit(MaxAuditLimit + 1); got != MaxAuditLimit {
		t.Errorf("ClampAuditLimit above the ceiling is %d, want the ceiling", got)
	}
	if got := ClampAuditLimit(7); got != 7 {
		t.Errorf("ClampAuditLimit(7) is %d, want 7", got)
	}
}

func TestDescribeEndpointHidesCredentials(t *testing.T) {
	t.Parallel()

	cases := map[string]string{
		"mongodb://127.0.0.1:27017":                                                "127.0.0.1:27017",
		"mongodb://ana:secret@db.example.invalid:27017/x":                          "db.example.invalid:27017",
		"mongodb+srv://ana:secret@cluster.example.invalid/?retryWrites=true":       "cluster.example.invalid",
		"mongodb://a.example.invalid:27017,b.example.invalid:27017/?replicaSet=rs": "a.example.invalid:27017,b.example.invalid:27017",
	}
	for uri, want := range cases {
		if got := describeEndpoint(uri); got != want {
			t.Errorf("describeEndpoint(%q) is %q, want %q", uri, got, want)
		}
	}
}
