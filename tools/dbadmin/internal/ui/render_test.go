package ui

import (
	"context"
	"strings"
	"testing"

	"github.com/charmbracelet/lipgloss"
)

// loaded is a model with both screens read, laid out for a window.
func loaded(t *testing.T, width, height int) Model {
	t.Helper()

	database := twoAccounts()
	model := New(database)
	model.width, model.height = width, height
	model.layout()

	accounts, err := database.Accounts(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	model.users.setAccounts(accounts)

	entries, err := database.Audit(context.Background(), model.audit.query())
	if err != nil {
		t.Fatal(err)
	}
	model.audit.setEntries(entries)

	summary, err := database.Summary(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	model.summary = summary
	return model
}

// A frame taller than the window is not a cosmetic problem. The renderer can
// only draw upwards as far as it has drawn before, so the extra lines push the
// header into the scrollback and every redraw fights the terminal.
func TestTheFrameFitsTheWindow(t *testing.T) {
	t.Parallel()

	sizes := []struct{ width, height int }{
		{120, 32}, {80, 24}, {200, 60}, {60, 14}, {42, 12}, {30, 10},
	}
	for _, size := range sizes {
		model := loaded(t, size.width, size.height)
		for _, screen := range []tab{tabUsers, tabAudit} {
			model.tab = screen
			frame := model.View()

			if lines := strings.Count(frame, "\n") + 1; lines != size.height {
				t.Errorf("at %dx%d, tab %d drew %d lines, want %d",
					size.width, size.height, screen, lines, size.height)
			}
			for number, line := range strings.Split(frame, "\n") {
				if drawn := lipgloss.Width(line); drawn > size.width {
					t.Errorf("at %dx%d, tab %d line %d is %d columns wide",
						size.width, size.height, screen, number+1, drawn)
				}
			}
		}
	}
}

// The overlays are cards over the body, and the body is what they have to fit
// inside: a card that is taller pushes the help line off the screen.
func TestTheOverlaysFitTheWindow(t *testing.T) {
	t.Parallel()

	const width, height = 100, 28
	for name, open := range map[string]func(m Model){
		"create":   func(m Model) { m.users.openCreate() },
		"edit":     func(m Model) { m.users.openEdit(m.users.accounts[0]) },
		"password": func(m Model) { m.users.openPassword(m.users.accounts[0]) },
		"delete": func(m Model) {
			m.users.target = m.users.accounts[0]
			m.users.mode = usersDelete
		},
		"details": func(m Model) {
			m.users.target = m.users.accounts[0]
			m.users.mode = usersDetail
		},
		"audit entry": func(m Model) {
			m.tab = tabAudit
			m.audit.target = m.audit.entries[0]
			m.audit.mode = auditDetail
		},
	} {
		model := loaded(t, width, height)
		open(model)
		frame := model.View()

		if lines := strings.Count(frame, "\n") + 1; lines != height {
			t.Errorf("the %s overlay drew %d lines, want %d", name, lines, height)
		}
		for number, line := range strings.Split(frame, "\n") {
			if drawn := lipgloss.Width(line); drawn > width {
				t.Errorf("the %s overlay is %d columns wide on line %d",
					name, drawn, number+1)
			}
		}
	}
}

// A narrow window drops columns rather than wrapping them. The wrapped
// alternative is what this looked like before: every row became two, and the
// screen turned into a wall.
func TestANarrowWindowKeepsWhatMatters(t *testing.T) {
	t.Parallel()

	wide := loaded(t, 120, 24).View()
	for _, want := range []string{"USERNAME", "DISPLAY NAME", "ROLE", "USER ID", "CREATED"} {
		if !strings.Contains(wide, want) {
			t.Errorf("a wide window is missing the %s column", want)
		}
	}

	narrow := loaded(t, 44, 14).View()
	for _, want := range []string{"USERNAME", "ROLE", "ana", "admin"} {
		if !strings.Contains(narrow, want) {
			t.Errorf("a narrow window dropped %q, which it cannot do:\n%s", want, narrow)
		}
	}
	if strings.Contains(narrow, "USER ID") {
		t.Errorf("a narrow window kept the identifier column:\n%s", narrow)
	}

	model := loaded(t, 44, 14)
	model.tab = tabAudit
	audit := model.View()
	for _, want := range []string{"TIME", "ACTION", "create_user"} {
		if !strings.Contains(audit, want) {
			t.Errorf("the narrow audit table dropped %q:\n%s", want, audit)
		}
	}
}

// A frame printed by `go test -v -run TestTheFrameLooksLikeThis`, which is how
// the layout is looked at while it is being changed.
func TestTheFrameLooksLikeThis(t *testing.T) {
	t.Parallel()

	model := loaded(t, 120, 24)
	t.Logf("users\n%s", model.View())

	model.tab = tabAudit
	t.Logf("audit\n%s", model.View())

	model.tab = tabUsers
	model.users.openCreate()
	t.Logf("new account\n%s", model.View())

	narrow := loaded(t, 44, 14)
	t.Logf("narrow users\n%s", narrow.View())
	narrow.tab = tabAudit
	t.Logf("narrow audit\n%s", narrow.View())
}
