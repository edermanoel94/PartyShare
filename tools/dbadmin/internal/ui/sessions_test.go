package ui

import (
	"strings"
	"testing"
)

func TestTheSessionsScreenNamesAccountsAndAddresses(t *testing.T) {
	t.Parallel()
	screen := start(t, sessionsAndAccounts())
	screen.awaits(t, "Ana Souza")

	screen.press("3")
	// The account is a user id in the document and a username on the screen,
	// resolved from the list the users screen reads.
	screen.awaits(t, "ana")
	screen.awaits(t, "203.0.113.7")

	final := screen.finish(t)
	// The other half of what this screen was asked for: an address per
	// connection, which is nowhere else in this program.
	if !strings.Contains(final, "203.0.113.8") {
		t.Fatalf("the second session's address was never drawn:\n%s", final)
	}
	// A session whose account has been deleted shows the identifier rather
	// than a blank, which would read like a bug in the column.
	if !strings.Contains(final, "id-nobod") {
		t.Fatalf("a session whose account is gone should still name it:\n%s", final)
	}
}

func TestTheSessionsScreenTellsOnlineFromStaleFromEnded(t *testing.T) {
	t.Parallel()
	screen := start(t, sessionsAndAccounts())
	screen.press("3")
	screen.awaits(t, "online")

	final := screen.finish(t)
	// Three states and not two, and each one checked on its own row rather
	// than anywhere on the screen: the status line says "online" too, so a
	// search over the whole frame would pass even if every row said the same
	// thing.
	//
	// A row that is open and has not been heard from is a server that was
	// killed rather than stopped, and calling that "online" would be this
	// program repeating a claim the database is only making because nobody
	// closed the row.
	for _, want := range []struct{ address, state string }{
		{"203.0.113.7", "online"},
		{"203.0.113.8", "stale"},
		{"198.51.100.4", "ended"},
	} {
		row := lineContaining(final, want.address)
		if row == "" {
			t.Fatalf("no row for %s:\n%s", want.address, final)
		}
		if !strings.Contains(row, want.state) {
			t.Errorf("the row for %s should read %q:\n%s", want.address, want.state, row)
		}
	}
}

// lineContaining is the one line of a frame that holds `needle`, or empty when
// none does. Assertions about a table are assertions about a row, and a whole
// frame contains almost every word somewhere.
func lineContaining(frame, needle string) string {
	for _, line := range strings.Split(frame, "\n") {
		if strings.Contains(line, needle) {
			return line
		}
	}
	return ""
}

func TestTheSessionsStatusLineCountsWhoIsHere(t *testing.T) {
	t.Parallel()
	screen := start(t, sessionsAndAccounts())
	screen.press("3")

	// The tab counts documents, like every other tab. The status line is where
	// the answer to "how many people are on the platform" lives, because that
	// is a question about time and not about the size of a collection.
	screen.awaits(t, "Sessions (3)")
	screen.awaits(t, "1 account online")
	screen.finish(t)
}

func TestASessionCardExplainsAStaleRow(t *testing.T) {
	t.Parallel()
	screen := start(t, sessionsAndAccounts())
	screen.press("3")
	screen.awaits(t, "203.0.113.7")

	// Down to the stale row - the open ones come first, newest heartbeat
	// first, so it is the second.
	screen.press("down")
	screen.press("enter")
	screen.awaits(t, "id-bruno")
	// Explained rather than asserted, because "stale" on its own reads like a
	// fault in this program rather than a fact about a server that died.
	screen.awaits(t, "never closed")
	screen.finish(t)
}

func TestTheSessionLimitIsPartOfTheQuery(t *testing.T) {
	t.Parallel()
	database := sessionsAndAccounts()
	screen := start(t, database)
	screen.press("3")
	screen.awaits(t, "203.0.113.7")

	screen.press("l")
	screen.awaits(t, "reading the newest 500")
	screen.finish(t)

	if !contains(database.sessionLimits, 500) {
		t.Fatalf("the limit key did not change the query, the limits were %v",
			database.sessionLimits)
	}
}

func TestFilteringSessionsSearchesTheAddressToo(t *testing.T) {
	t.Parallel()
	screen := start(t, sessionsAndAccounts())
	screen.press("3")
	screen.awaits(t, "203.0.113.7")

	// An address and not a name, because the address is the thing somebody
	// arrives with: a line in a firewall log, or a number read out over the
	// phone, and the question is which account it belongs to.
	screen.press("/")
	screen.typeText("203.0.113.8")
	screen.awaits(t, "bruno")

	final := screen.finish(t)
	if strings.Contains(final, "203.0.113.7") {
		t.Fatalf("the filter left the other session on screen:\n%s", final)
	}
}

func TestTheUserCardSaysWhetherTheyAreConnected(t *testing.T) {
	t.Parallel()
	screen := start(t, sessionsAndAccounts())
	screen.awaits(t, "Ana Souza")

	// The users screen, not the sessions one: this is the question somebody
	// has while looking at an account, and the answer comes from the other
	// collection.
	screen.press("enter")
	screen.awaits(t, "Presence")
	screen.awaits(t, "online")
	screen.awaits(t, "203.0.113.7")
	screen.finish(t)
}

func TestAnEmptySessionsCollectionSaysWhatWritesIt(t *testing.T) {
	t.Parallel()
	screen := start(t, &fakeDatabase{})
	screen.press("3")
	// The one thing an operator staring at an empty screen needs to know is
	// which program is supposed to fill it, because it is not this one.
	screen.awaits(t, "The server writes one every time somebody signs in")
	screen.finish(t)
}

// The sessions screen is the one screen here with no write of any kind. Worth
// a test rather than a comment: the collection records what the server saw,
// and a presence history an administrator can edit is not evidence of
// anything.
func TestTheSessionsScreenChangesNothing(t *testing.T) {
	t.Parallel()
	database := sessionsAndAccounts()
	screen := start(t, database)
	screen.press("3")
	screen.awaits(t, "203.0.113.7")

	for _, key := range []string{"d", "n", "e", "p", "m", "y"} {
		screen.press(key)
	}
	screen.finish(t)

	if len(database.deleted) != 0 || len(database.deletedRooms) != 0 ||
		len(database.created) != 0 || len(database.updated) != 0 ||
		len(database.restricted) != 0 || len(database.passwords) != 0 {
		t.Fatalf("the sessions screen wrote something: deleted %v, rooms %v, created %v, "+
			"updated %v, restricted %v, passwords %v",
			database.deleted, database.deletedRooms, database.created,
			database.updated, database.restricted, database.passwords)
	}
}
