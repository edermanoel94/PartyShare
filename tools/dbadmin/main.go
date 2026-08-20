// Command dbadmin is a terminal front end for the PartyShare database.
//
// It manages the two collections an operator has to reach without a running
// server: the accounts, and the record of what was done to them. The signaling
// server owns the same data through its admin panel, and this program exists
// for the moments when that panel is not available: a server that will not
// start because nobody remembers the administrator password, a database
// restored from a backup, a machine with a terminal and no desktop.
//
// It writes documents the server reads back unchanged, credentials included,
// and it records what it does in the same audit log for the same reason the
// server does: an administrative action nobody can point at afterwards is an
// administrative action that might as well not have been checked.
package main

import (
	"context"
	"flag"
	"fmt"
	"os"
	"os/user"
	"strconv"
	"time"

	tea "github.com/charmbracelet/bubbletea"

	"github.com/edermanoel94/PartyShare/tools/dbadmin/internal/store"
	"github.com/edermanoel94/PartyShare/tools/dbadmin/internal/ui"
)

// The defaults are the server's own, from dv::config::DatabaseConfig, so that
// running this program on the machine the server runs on needs no arguments.
const (
	defaultURI      = "mongodb://127.0.0.1:27017"
	defaultDatabase = "partyshare"
	defaultTimeout  = 5 * time.Second
)

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "dbadmin:", err)
		os.Exit(1)
	}
}

func run() error {
	// The same environment variables the server reads, so that a shell already
	// set up to start the server is already set up to administer it. The flags
	// win over them, and both lose to nothing else.
	uri := flag.String("uri", environment("DV_DATABASE_URI", defaultURI),
		"MongoDB connection string. Also DV_DATABASE_URI.")
	database := flag.String("database", environment("DV_DATABASE_NAME", defaultDatabase),
		"Database name. Also DV_DATABASE_NAME.")
	timeout := flag.Duration("timeout", environmentDuration("DV_DATABASE_TIMEOUT_MS",
		defaultTimeout), "How long any single database operation may take.")
	operator := flag.String("operator", "",
		"Name recorded in the audit log. Defaults to the operating system user.")
	flag.Usage = usage
	flag.Parse()

	if flag.NArg() > 0 {
		// A mistyped flag is otherwise indistinguishable from a correct one,
		// and the program runs on a default nobody chose. The server's own
		// parser refuses unknown arguments for the same reason.
		return fmt.Errorf("unexpected argument %q", flag.Arg(0))
	}

	actor := actorFor(*operator)

	// Connecting before starting the interface. A terminal program that clears
	// the screen and then says it cannot reach the database has hidden the
	// shell the operator needs to fix it.
	connection, err := store.Open(context.Background(), store.Config{
		URI:      *uri,
		Database: *database,
		Timeout:  *timeout,
		Actor:    actor,
	})
	if err != nil {
		return err
	}
	defer func() {
		if err := connection.Close(); err != nil {
			fmt.Fprintln(os.Stderr, "dbadmin: could not close the connection:", err)
		}
	}()

	program := tea.NewProgram(ui.New(connection), tea.WithAltScreen())
	_, err = program.Run()
	return err
}

func usage() {
	fmt.Fprintln(flag.CommandLine.Output(),
		"dbadmin manages the PartyShare accounts and audit log in MongoDB.\n\nOptions:")
	flag.PrintDefaults()
}

// actorFor decides whose name goes on the audit entries this session writes.
//
// A prefixed identifier rather than a user id, because whoever runs this has a
// shell and not an account: inventing one would put a login in the users
// collection that nobody can use, and reusing an administrator's identifier
// would attribute a change to somebody who was not at the keyboard. The prefix
// is what lets a reader filter the terminal's entries apart from the server's.
func actorFor(operator string) store.Actor {
	name := operator
	if name == "" {
		if current, err := user.Current(); err == nil && current.Username != "" {
			name = current.Username
		}
	}
	if name == "" {
		name = "unknown"
	}
	return store.Actor{ID: "dbadmin:" + name, Username: name + " (dbadmin)"}
}

func environment(name, fallback string) string {
	if value, found := os.LookupEnv(name); found && value != "" {
		return value
	}
	return fallback
}

// environmentDuration reads the server's variable, which is a number of
// milliseconds, and leaves the flag free to take "3s" like every other Go
// duration.
func environmentDuration(name string, fallback time.Duration) time.Duration {
	value, found := os.LookupEnv(name)
	if !found || value == "" {
		return fallback
	}
	milliseconds, err := strconv.Atoi(value)
	if err != nil || milliseconds <= 0 {
		return fallback
	}
	return time.Duration(milliseconds) * time.Millisecond
}
