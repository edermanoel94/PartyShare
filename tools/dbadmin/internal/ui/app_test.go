package ui

import (
	"bytes"
	"io"
	"strings"
	"testing"
	"time"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/x/exp/teatest"

	"github.com/edermanoel94/PartyShare/tools/dbadmin/internal/store"
)

// The screens are driven the way a person drives them: a terminal of a known
// size, keystrokes in, and the frames that came out. Nothing here calls Update
// directly, because a test that does cannot tell a key that is handled from a
// key that a form swallowed.
const (
	testWidth  = 120
	testHeight = 32
	// Generous. Every wait below is on work that takes microseconds against
	// the fake, so reaching this means something is stuck rather than slow.
	waitTimeout = 5 * time.Second
)

// session is a running program and everything it has drawn so far.
//
// The frames are accumulated rather than read once, because the program's
// output is a stream that is consumed as it is read: a second look at what was
// already drawn finds nothing, and every assertion below is about a screen
// that was drawn at some point.
type session struct {
	model  *teatest.TestModel
	output io.Reader
	frames bytes.Buffer
}

func start(t *testing.T, database Database) *session {
	t.Helper()

	model := teatest.NewTestModel(t, New(database),
		teatest.WithInitialTermSize(testWidth, testHeight))
	return &session{model: model, output: model.Output()}
}

// drawn is everything the program has written, draining whatever it has
// written since the last look.
//
// The reader underneath is a buffer, so it answers io.EOF whenever it happens
// to be empty rather than when the program is finished. That is not the end of
// anything and is deliberately ignored.
func (s *session) drawn() string {
	written, _ := io.ReadAll(s.output)
	s.frames.Write(written)
	return s.frames.String()
}

// awaits blocks until the text has been drawn at least once, and fails with
// everything that was drawn instead, which is a screen somebody can read.
func (s *session) awaits(t *testing.T, want string) {
	t.Helper()
	deadline := time.Now().Add(waitTimeout)
	for time.Now().Before(deadline) {
		if strings.Contains(s.drawn(), want) {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatalf("%q was never drawn. What was:\n%s", want, s.drawn())
}

func (s *session) press(keys ...string) {
	for _, name := range keys {
		s.model.Send(key(name))
	}
}

func (s *session) typeText(text string) { s.model.Type(text) }

// settle waits for the screen to stop changing.
//
// A change is answered in two parts. doneMsg draws the status line and returns
// refresh() as a separate command, so the list that the sentence describes
// lands a frame or more later. Quitting as soon as "was deleted" appears
// catches the screen between the two: under GOMAXPROCS=1 that was most runs,
// not a rare one, and it is what turned two of these tests red on a commit
// that had not touched this program at all.
//
// Quiet rather than a particular string, for two reasons. What to wait for
// differs per test, and one of them is waiting for a row to be gone, which is
// an absence and not something a substring can be found in. And the counts in
// the header are no substitute: refresh() batches three reads that land
// independently, so "Users (1)" above two rows is a frame these tests have
// actually produced.
//
// Bounded, because a screen with a text field on it has a blinking cursor and
// never goes quiet. Reaching the bound is not a failure; it leaves those tests
// looking at exactly the screen they looked at before.
func (s *session) settle() {
	const between = 20 * time.Millisecond
	const enough = 3

	deadline := time.Now().Add(time.Second)
	drawn := len(s.drawn())
	for stable := 0; stable < enough && time.Now().Before(deadline); {
		time.Sleep(between)
		if grown := len(s.drawn()); grown != drawn {
			drawn = grown
			stable = 0
			continue
		}
		stable++
	}
}

// finish stops the program and returns the last frame, which is where an
// assertion about what is on the screen now rather than what was on it at some
// point belongs.
//
// It settles first, so that "now" means the screen an operator would be
// looking at rather than whichever half of a change happened to have arrived.
func (s *session) finish(t *testing.T) string {
	t.Helper()
	s.settle()
	if err := s.model.Quit(); err != nil {
		t.Fatalf("could not quit: %v", err)
	}
	s.model.WaitFinished(t, teatest.WithFinalTimeout(waitTimeout))

	final, ok := s.model.FinalModel(t, teatest.WithFinalTimeout(waitTimeout)).(Model)
	if !ok {
		t.Fatal("the program finished on a model of another type")
	}
	// Rendered rather than read from the stream: the renderer only writes the
	// lines that changed, so the last frame is not the last bytes.
	return final.View()
}

func key(name string) tea.KeyMsg {
	switch name {
	case "enter":
		return tea.KeyMsg{Type: tea.KeyEnter}
	case "tab":
		return tea.KeyMsg{Type: tea.KeyTab}
	case "shift+tab":
		return tea.KeyMsg{Type: tea.KeyShiftTab}
	case "esc":
		return tea.KeyMsg{Type: tea.KeyEsc}
	case "right":
		return tea.KeyMsg{Type: tea.KeyRight}
	case "down":
		return tea.KeyMsg{Type: tea.KeyDown}
	default:
		return tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune(name)}
	}
}

func TestTheUsersScreenShowsTheAccountsAndTheConnection(t *testing.T) {
	t.Parallel()
	screen := start(t, twoAccounts())
	screen.awaits(t, "Ana Souza")

	final := screen.finish(t)
	for _, want := range []string{
		// Which database is being edited, which is the question somebody with
		// two terminals open needs answered.
		"partyshare_test", "127.0.0.1:27017", "as test",
		"USERNAME", "DISPLAY NAME", "ROLE", "USER ID", "CREATED",
		"ana", "Ana Souza", "admin", "bruno", "Bruno Lima",
		"2 accounts · 1 administrator",
	} {
		if !strings.Contains(final, want) {
			t.Errorf("the screen does not show %q. It shows:\n%s", want, final)
		}
	}
}

func TestQuittingWithQ(t *testing.T) {
	t.Parallel()
	screen := start(t, twoAccounts())
	screen.awaits(t, "Ana Souza")

	screen.press("q")
	screen.model.WaitFinished(t, teatest.WithFinalTimeout(waitTimeout))
}

func TestCreatingAnAccountSendsWhatTheFormHolds(t *testing.T) {
	t.Parallel()
	database := twoAccounts()
	screen := start(t, database)
	screen.awaits(t, "Ana Souza")

	screen.press("n")
	screen.awaits(t, "New account")

	screen.typeText("carla")
	screen.press("tab")
	screen.typeText("a good password")
	screen.press("tab")
	screen.typeText("a good password")
	screen.press("tab")
	screen.typeText("Carla Reis")
	screen.press("tab") // avatar, left empty
	screen.press("tab") // role
	screen.press("right")
	screen.press("enter")

	screen.awaits(t, "was created")
	final := screen.finish(t)

	if len(database.created) != 1 {
		t.Fatalf("the screen sent %d creates, want 1", len(database.created))
	}
	created := database.created[0]
	if created.Username != "carla" || created.DisplayName != "Carla Reis" {
		t.Errorf("created %+v, want the names that were typed", created)
	}
	if created.Password != "a good password" {
		t.Errorf("the password arrived as %q", created.Password)
	}
	if created.Role != "admin" {
		t.Errorf("the role arrived as %q, want the one the arrow key selected", created.Role)
	}
	// The list was read again, so the new account is on the screen and not
	// only in the database.
	if !strings.Contains(final, "Carla Reis") {
		t.Errorf("the new account is not in the list:\n%s", final)
	}
}

// A password that is typed once is a password that is typed wrong, and this
// program is often the last way into a server.
func TestTheCreateFormRefusesTwoDifferentPasswords(t *testing.T) {
	t.Parallel()
	database := twoAccounts()
	screen := start(t, database)
	screen.awaits(t, "Ana Souza")

	screen.press("n")
	screen.awaits(t, "New account")
	screen.typeText("carla")
	screen.press("tab")
	screen.typeText("one password")
	screen.press("tab")
	screen.typeText("another password")
	screen.press("enter")

	screen.awaits(t, "The two passwords are not the same")
	final := screen.finish(t)

	if len(database.created) != 0 {
		t.Errorf("the screen sent %d creates, want none", len(database.created))
	}
	// The form is still open, with what was typed still in it.
	if !strings.Contains(final, "New account") {
		t.Errorf("the form closed on a refusal:\n%s", final)
	}
}

func TestPasswordsAreNeverOnTheScreen(t *testing.T) {
	t.Parallel()
	screen := start(t, twoAccounts())
	screen.awaits(t, "Ana Souza")

	screen.press("p")
	screen.awaits(t, "Password of ana")

	const secret = "hunter2andthensome"
	screen.typeText(secret)
	screen.awaits(t, "•")

	final := screen.finish(t)
	if strings.Contains(screen.drawn(), secret) || strings.Contains(final, secret) {
		t.Error("a typed password was drawn on the screen")
	}
}

func TestSettingAPassword(t *testing.T) {
	t.Parallel()
	database := twoAccounts()
	screen := start(t, database)
	screen.awaits(t, "Ana Souza")

	screen.press("p")
	screen.awaits(t, "Password of ana")
	screen.typeText("a new password")
	screen.press("tab")
	screen.typeText("a new password")
	screen.press("enter")

	screen.awaits(t, "The password of \"ana\" was changed")
	screen.finish(t)

	want := "id-ana a new password"
	if len(database.passwords) != 1 || database.passwords[0] != want {
		t.Errorf("the database was asked for %v, want [%q]", database.passwords, want)
	}
}

func TestDeleteAsksFirst(t *testing.T) {
	t.Parallel()
	database := twoAccounts()
	screen := start(t, database)
	screen.awaits(t, "Ana Souza")

	screen.press("d")
	screen.awaits(t, "Delete ana?")
	// The consequence a database tool cannot undo for the operator: the
	// server's own copy of the session.
	screen.awaits(t, "goes on")

	screen.press("n")
	if len(database.deleted) != 0 {
		t.Fatalf("answering no deleted %v", database.deleted)
	}

	screen.press("d")
	screen.awaits(t, "Delete ana?")
	screen.press("y")
	screen.awaits(t, "was deleted")
	final := screen.finish(t)

	if len(database.deleted) != 1 || database.deleted[0] != "id-ana" {
		t.Errorf("deleted %v, want the selected account", database.deleted)
	}
	if strings.Contains(final, "Ana Souza") {
		t.Errorf("the deleted account is still in the list:\n%s", final)
	}
}

func TestAFailedChangeIsReportedAndNothingIsLost(t *testing.T) {
	t.Parallel()
	database := twoAccounts()
	database.failure = errDatabaseUnreachable
	screen := start(t, database)
	screen.awaits(t, "Ana Souza")

	screen.press("d")
	screen.awaits(t, "Delete ana?")
	screen.press("y")

	screen.awaits(t, "could not reach the database")
	final := screen.finish(t)
	if !strings.Contains(final, "Ana Souza") {
		t.Errorf("the account vanished from a screen whose delete failed:\n%s", final)
	}
}

func TestFilteringTheUsers(t *testing.T) {
	t.Parallel()
	screen := start(t, twoAccounts())
	screen.awaits(t, "Bruno Lima")

	screen.press("/")
	screen.typeText("bru")
	screen.awaits(t, "filter bru")

	screen.press("enter")
	if drawn := screen.drawn(); !strings.Contains(drawn, "bruno") {
		t.Errorf("the filtered list lost its only match:\n%s", drawn)
	}

	screen.press("esc")
	final := screen.finish(t)
	if !strings.Contains(final, "Ana Souza") {
		t.Errorf("clearing the filter did not bring the other account back:\n%s", final)
	}
}

func TestTheAuditTabReadsTheLog(t *testing.T) {
	t.Parallel()
	database := twoAccounts()
	screen := start(t, database)
	screen.awaits(t, "Ana Souza")

	// 3 rather than tab: the rooms screen sits between the two now, and this
	// test is about the log rather than about how you get to it.
	screen.press("3")
	screen.awaits(t, "create_user")
	screen.awaits(t, "username=bruno role=user")

	// The entry as a card, where the identifiers are shown in full.
	screen.press("enter")
	screen.awaits(t, "id-bruno")
	screen.press("esc")

	// Only this actor: a different query, not a filter over what was read.
	screen.press("a")
	screen.awaits(t, "actor ana")
	screen.finish(t)

	var byActor int
	for _, query := range database.queries {
		if query.ActorID == "id-ana" {
			byActor++
		}
	}
	if byActor == 0 {
		t.Errorf("the actor key did not reach the database, the queries were %+v",
			database.queries)
	}
}

func TestTheAuditLimitIsPartOfTheQuery(t *testing.T) {
	t.Parallel()
	database := twoAccounts()
	screen := start(t, database)
	screen.awaits(t, "Ana Souza")

	screen.press("3")
	screen.awaits(t, "create_user")
	screen.press("l")
	screen.awaits(t, "read 1000")
	screen.finish(t)

	var limits []int
	for _, query := range database.queries {
		limits = append(limits, query.Limit)
	}
	if !contains(limits, 500) {
		t.Errorf("the limit key did not change the query, the limits were %v", limits)
	}
}

func contains(values []int, want int) bool {
	for _, value := range values {
		if value == want {
			return true
		}
	}
	return false
}

func TestAnEmptyDatabaseSaysWhatToDo(t *testing.T) {
	t.Parallel()
	screen := start(t, &fakeDatabase{})

	screen.awaits(t, "No accounts in this database yet")
	screen.press("2")
	screen.awaits(t, "No rooms in this database")
	screen.press("3")
	screen.awaits(t, "The audit log is empty")
	screen.finish(t)
}

// The cycle was `1 - tab` while there were two screens, which is an expression
// with the count welded into it. A third screen is what turns that into a
// wrong answer rather than a clever one.
func TestTabCyclesThroughEveryScreenAndBack(t *testing.T) {
	t.Parallel()
	screen := start(t, roomsAndAccounts())
	screen.awaits(t, "Ana Souza")

	screen.press("tab")
	screen.awaits(t, "8F42A1")
	screen.press("tab")
	screen.awaits(t, "create_user")
	screen.press("tab")
	screen.awaits(t, "Ana Souza")

	// And backwards, which used to be the same key doing the same thing.
	screen.press("shift+tab")
	screen.awaits(t, "create_user")
	screen.press("shift+tab")
	screen.awaits(t, "8F42A1")
	screen.finish(t)
}

// The single letter shortcuts belong to the list. Inside a field they are
// letters, and a display name that cannot contain a d is not a display name.
func TestShortcutsDoNotFireWhileTyping(t *testing.T) {
	t.Parallel()
	database := twoAccounts()
	screen := start(t, database)
	screen.awaits(t, "Ana Souza")

	screen.press("e")
	screen.awaits(t, "Edit ana")
	screen.press("tab")
	screen.typeText("dqnper")
	screen.awaits(t, "dqnper")

	screen.press("esc")
	final := screen.finish(t)
	if len(database.deleted) != 0 || len(database.updated) != 0 {
		t.Error("a letter typed into a field reached the list underneath")
	}
	if !strings.Contains(final, "USERNAME") {
		t.Errorf("escape did not close the form:\n%s", final)
	}
}

func TestEditingAnAccountSendsTheWholeAccount(t *testing.T) {
	t.Parallel()
	database := twoAccounts()
	screen := start(t, database)
	screen.awaits(t, "Ana Souza")

	screen.press("down") // bruno
	screen.press("e")
	screen.awaits(t, "Edit bruno")
	screen.press("tab") // display name
	screen.typeText(" Junior")
	screen.press("tab") // avatar
	screen.press("tab") // role
	screen.press("right")
	screen.press("enter")

	screen.awaits(t, "was saved")
	screen.finish(t)

	if len(database.updated) != 1 {
		t.Fatalf("the screen sent %d updates, want 1", len(database.updated))
	}
	updated := database.updated[0]
	if updated.UserID != "id-bruno" {
		t.Errorf("the update names %q, want the selected account", updated.UserID)
	}
	if updated.Username != "bruno" {
		t.Errorf("the username arrived as %q, want it unchanged", updated.Username)
	}
	if updated.DisplayName != "Bruno Lima Junior" {
		t.Errorf("the display name arrived as %q", updated.DisplayName)
	}
	if updated.Role != "admin" {
		t.Errorf("the role arrived as %q, want the promoted one", updated.Role)
	}
}

func TestRestrictingAnAccountSendsWhatTheFormHolds(t *testing.T) {
	t.Parallel()
	database := twoAccounts()
	screen := start(t, database)
	screen.awaits(t, "Ana Souza")

	screen.press("down") // bruno
	screen.press("m")
	screen.awaits(t, "Restrictions for bruno")

	screen.press("tab")   // cannot sign in, left at no
	screen.press("right") // cannot use the microphone: yes
	screen.press("tab")
	screen.press("right") // cannot write in the chat: yes
	screen.press("tab")   // cannot share their screen, left at no
	screen.press("enter")

	screen.awaits(t, "is now restricted")
	final := screen.finish(t)

	if len(database.restricted) != 1 {
		t.Fatalf("the screen sent %d restrictions, want 1", len(database.restricted))
	}
	call := database.restricted[0]
	if call.userID != "id-bruno" {
		t.Errorf("the change names %q, want the selected account", call.userID)
	}
	want := store.Restrictions{Muted: true, Silenced: true}
	if call.restrictions != want {
		t.Errorf("the restrictions arrived as %+v, want %+v", call.restrictions, want)
	}
	// The list was read again, so the column shows it rather than the screen
	// having only said so.
	if !strings.Contains(final, "mic chat") {
		t.Errorf("the restrictions column does not show the change:\n%s", final)
	}
}

// The form opens on what the account already has, so that saving it unchanged
// is a no-op rather than a way to lift everything by accident.
func TestTheRestrictionsFormOpensOnWhatIsInForce(t *testing.T) {
	t.Parallel()
	database := twoAccounts()
	database.accounts[1].Restrictions = store.Restrictions{Banned: true, Silenced: true}
	screen := start(t, database)
	screen.awaits(t, "Ana Souza")

	screen.press("down") // bruno
	screen.press("m")
	screen.awaits(t, "Restrictions for bruno")
	screen.press("enter")

	screen.awaits(t, "already those")
	screen.finish(t)

	if len(database.restricted) != 1 {
		t.Fatalf("the screen sent %d restrictions, want 1", len(database.restricted))
	}
	want := store.Restrictions{Banned: true, Silenced: true}
	if got := database.restricted[0].restrictions; got != want {
		t.Errorf("submitting the form unchanged sent %+v, want %+v", got, want)
	}
}

// A database that cannot be written to has to say so, and has to leave the
// list showing what is still true rather than what was asked for.
func TestARefusedRestrictionIsReported(t *testing.T) {
	t.Parallel()
	database := twoAccounts()
	database.failure = errDatabaseUnreachable
	screen := start(t, database)
	screen.awaits(t, "Ana Souza")

	screen.press("m")
	screen.awaits(t, "Restrictions for ana")
	screen.press("right") // cannot sign in: yes
	screen.press("enter")

	screen.awaits(t, "could not reach the database")
	final := screen.finish(t)
	if strings.Contains(final, "ban") {
		t.Errorf("a refused restriction reached the list:\n%s", final)
	}
}
