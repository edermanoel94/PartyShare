// Package ui is the terminal interface: three screens over the users, rooms
// and audit collections, and the messages that carry a database answer back
// into them.
//
// Every call to the database happens inside a tea.Cmd and never inside Update.
// That is what keeps the screen answering while a query is in flight, and it
// is why nothing below returns a value from the store directly.
package ui

import (
	"context"
	"errors"
	"fmt"
	"strconv"
	"strings"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"

	"github.com/edermanoel94/PartyShare/tools/dbadmin/internal/store"
)

type tab int

const (
	tabUsers tab = iota
	tabRooms
	tabAudit
	tabCount
)

// How many lines everything but the body takes: the header, the tab bar, the
// status line, the help line, and the three blank lines between them.
//
// Counted here, once, so that the body height and what is actually drawn
// cannot drift apart. A window too short to hold this and a few rows of a
// table gives up the blank lines first, which is what compactChromeHeight is.
const (
	chromeHeight        = 8
	compactChromeHeight = 5
	minimumBody         = 3
)

// What a command sends back.
type (
	// accountsMsg is a fresh user list.
	accountsMsg []store.Account
	// roomsMsg is a fresh room list.
	roomsMsg []store.Room
	// auditEntriesMsg is a fresh page of the audit log.
	auditEntriesMsg []store.AuditEntry
	// summaryMsg is the counts in the header.
	summaryMsg store.Summary
	// doneMsg is a change that was made and recorded. focus is the account the
	// list should land on once it has been read again.
	doneMsg struct {
		text  string
		focus string
	}
	// warningMsg is a change that was made and not recorded, which is the one
	// case where a green line would be a lie and a red one would suggest the
	// change did not happen.
	warningMsg struct {
		text   string
		detail string
	}
	// failureMsg is a change that did not happen.
	failureMsg struct{ err error }
)

type statusKind int

const (
	statusNone statusKind = iota
	statusOK
	statusWarn
	statusError
)

type status struct {
	kind statusKind
	text string
}

// Model is the whole program.
type Model struct {
	store   Database
	users   *usersModel
	rooms   *roomsModel
	audit   *auditModel
	tab     tab
	summary store.Summary
	status  status
	// focus is where the user list should put its cursor after the next read,
	// carried across the round trip that a change and its refresh make.
	focus         string
	width, height int
	quitting      bool
}

// New builds the program over an open store.
func New(database Database) Model {
	return Model{
		store: database,
		users: newUsersModel(database),
		rooms: newRoomsModel(database),
		audit: newAuditModel(database),
		// A first size, so that the first frame is laid out rather than
		// squeezed into the eighty columns of a default.
		width:  100,
		height: 30,
	}
}

// Init reads everything the three screens show. All of it at once: they are
// independent queries and the screen is more useful for having whichever
// arrives first.
func (m Model) Init() tea.Cmd {
	return tea.Batch(
		loadAccounts(m.store),
		loadSummary(m.store),
		loadRooms(m.store),
		loadAudit(m.store, m.audit.query()),
	)
}

func loadAccounts(database Database) tea.Cmd {
	return func() tea.Msg {
		accounts, err := database.Accounts(context.Background())
		if err != nil {
			return failureMsg{err: err}
		}
		return accountsMsg(accounts)
	}
}

func loadSummary(database Database) tea.Cmd {
	return func() tea.Msg {
		summary, err := database.Summary(context.Background())
		if err != nil {
			return failureMsg{err: err}
		}
		return summaryMsg(summary)
	}
}

func loadRooms(database Database) tea.Cmd {
	return func() tea.Msg {
		rooms, err := database.Rooms(context.Background())
		if err != nil {
			return failureMsg{err}
		}
		return roomsMsg(rooms)
	}
}

func loadAudit(database Database, query store.AuditQuery) tea.Cmd {
	return func() tea.Msg {
		entries, err := database.Audit(context.Background(), query)
		if err != nil {
			return failureMsg{err: err}
		}
		return auditEntriesMsg(entries)
	}
}

// outcomeFor turns the error of a change into the line the status bar shows.
//
// The audit failure is its own colour on purpose. The change stands, so
// reporting it as an error would send somebody looking for an account that is
// already there; reporting it as a success would hide the one thing an audit
// log exists to make impossible to hide.
func outcomeFor(err error, done string) tea.Msg {
	if errors.Is(err, store.ErrAuditNotWritten) {
		return warningMsg{text: done, detail: err.Error()}
	}
	return failureMsg{err: err}
}

// Update is the whole keyboard of the program.
//
// Keys are offered to the active screen first whenever it is typing into
// something, because "n" is a new account on a list and the letter n inside a
// display name.
func (m Model) Update(message tea.Msg) (tea.Model, tea.Cmd) {
	switch typed := message.(type) {
	case tea.WindowSizeMsg:
		m.width, m.height = typed.Width, typed.Height
		m.layout()
		return m, nil

	case accountsMsg:
		m.users.setAccounts(typed)
		// The rooms screen shows who owns each room, and an owner is a user id
		// in the document. This is where it learns the names.
		m.rooms.setAccounts(typed)
		if m.focus != "" {
			m.users.focusOn(m.focus)
			m.focus = ""
		}
		return m, nil

	case roomsMsg:
		m.rooms.setRooms(typed)
		return m, nil

	case auditEntriesMsg:
		m.audit.setEntries(typed)
		return m, nil

	case summaryMsg:
		m.summary = store.Summary(typed)
		return m, nil

	case doneMsg:
		m.status = status{kind: statusOK, text: typed.text}
		m.focus = typed.focus
		return m, m.refresh()

	case warningMsg:
		m.status = status{
			kind: statusWarn,
			text: typed.text + ", but " + typed.detail,
		}
		return m, m.refresh()

	case failureMsg:
		m.status = status{kind: statusError, text: typed.err.Error()}
		return m, nil

	case tea.KeyMsg:
		return m.updateKey(typed)
	}

	// Everything else is a cursor blink or a timer belonging to whatever has
	// the keyboard.
	return m, m.delegate(message)
}

func (m Model) updateKey(key tea.KeyMsg) (tea.Model, tea.Cmd) {
	if key.String() == "ctrl+c" {
		m.quitting = true
		return m, tea.Quit
	}

	if m.capturesKeys() {
		return m, m.delegate(key)
	}

	switch key.String() {
	case "q":
		m.quitting = true
		return m, tea.Quit
	case "tab":
		m.tab = (m.tab + 1) % tabCount
		m.status = status{}
		m.layout()
		return m, nil
	case "shift+tab":
		m.tab = (m.tab + tabCount - 1) % tabCount
		m.status = status{}
		m.layout()
		return m, nil
	case "1":
		m.tab = tabUsers
		m.layout()
		return m, nil
	case "2":
		m.tab = tabRooms
		m.layout()
		return m, nil
	case "3":
		m.tab = tabAudit
		m.layout()
		return m, nil
	case "r":
		m.status = status{}
		return m, m.refresh()
	}

	return m, m.delegate(key)
}

// refresh reads whatever the current screen shows, plus the counts. Every
// screen and not one, because a change on any of them writes an audit entry
// that the audit screen would otherwise not know about, and deleting a room
// changes a count the tab bar shows.
func (m Model) refresh() tea.Cmd {
	return tea.Batch(
		loadAccounts(m.store),
		loadSummary(m.store),
		loadRooms(m.store),
		loadAudit(m.store, m.audit.query()),
	)
}

func (m Model) capturesKeys() bool {
	switch m.tab {
	case tabRooms:
		return m.rooms.capturesKeys()
	case tabAudit:
		return m.audit.capturesKeys()
	case tabUsers, tabCount:
	}
	return m.users.capturesKeys()
}

func (m Model) delegate(message tea.Msg) tea.Cmd {
	switch m.tab {
	case tabRooms:
		return m.rooms.Update(message)
	case tabAudit:
		return m.audit.Update(message)
	case tabUsers, tabCount:
	}
	return m.users.Update(message)
}

func (m Model) layout() {
	m.users.setSize(m.innerWidth(), m.bodyHeight())
	m.rooms.setSize(m.innerWidth(), m.bodyHeight())
	m.audit.setSize(m.innerWidth(), m.bodyHeight())
}

// innerWidth is the width inside the one column of padding the frame keeps on
// each side, and never so narrow that a table has nowhere to go.
func (m Model) innerWidth() int { return max(m.width-2, 20) }

func (m Model) bodyHeight() int { return max(m.height-m.chromeHeight(), 1) }

// compact is a window too short for the frame it would rather draw.
func (m Model) compact() bool { return m.height < chromeHeight+minimumBody }

func (m Model) chromeHeight() int {
	if m.compact() {
		return compactChromeHeight
	}
	return chromeHeight
}

// View draws the frame: a title line, the tabs, the body of whichever screen
// is active, and two lines at the bottom that always mean the same thing, a
// result and the keys.
func (m Model) View() string {
	if m.quitting {
		return ""
	}

	inner := m.innerWidth()

	body := m.users.View()
	help := m.users.help()
	switch m.tab {
	case tabRooms:
		body, help = m.rooms.View(), m.rooms.help()
	case tabAudit:
		body, help = m.audit.View(), m.audit.help()
	case tabUsers, tabCount:
	}

	// The body is padded and cut to exactly the height the frame reserved for
	// it. Neither is cosmetic: a frame taller than the window pushes its own
	// header into the scrollback, and one shorter leaves the help line
	// floating in the middle of the screen.
	bodyBox := lipgloss.NewStyle().
		Width(inner).MaxWidth(inner).
		Height(m.bodyHeight()).MaxHeight(m.bodyHeight())

	lines := []string{clamp(m.header(inner), inner)}
	if !m.compact() {
		lines = append(lines, "")
	}
	lines = append(lines, clamp(m.tabs(inner), inner))
	if !m.compact() {
		lines = append(lines, "")
	}
	lines = append(lines, bodyBox.Render(body))
	if !m.compact() {
		lines = append(lines, "")
	}
	lines = append(lines, clamp(m.statusLine(inner), inner), clamp(help, inner))

	frame := strings.Join(lines, "\n")

	return lipgloss.NewStyle().Padding(0, 1).Render(frame)
}

func (m Model) header(width int) string {
	left := titleStyle.Render("PartyShare") + metaStyle.Render(" · database admin")
	right := metaStyle.Render(fmt.Sprintf("%s · %s · as %s",
		m.store.Database(), m.store.Endpoint(), m.store.Actor().Username))

	gap := width - lipgloss.Width(left) - lipgloss.Width(right)
	if gap < 1 {
		// Too narrow for both. The connection is what a second window makes
		// ambiguous, so it is the half that stays.
		return right
	}
	return left + strings.Repeat(" ", gap) + right
}

func (m Model) tabs(width int) string {
	labels := map[tab]string{
		tabUsers: "Users " + countLabel(m.summary.Users),
		tabRooms: "Rooms " + countLabel(m.summary.Rooms),
		tabAudit: "Audit " + countLabel(m.summary.AuditEntries),
	}

	pieces := make([]string, 0, int(tabCount))
	for which := tabUsers; which < tabCount; which++ {
		style := tabInactiveStyle
		if which == m.tab {
			style = tabActiveStyle
		}
		pieces = append(pieces, style.Render(labels[which]))
	}
	rendered := lipgloss.JoinHorizontal(lipgloss.Bottom, pieces...)

	// The bar is one line drawn all the way across, so the tabs sit on a
	// rule rather than floating above nothing.
	gap := width - lipgloss.Width(rendered)
	if gap > 0 {
		rendered = lipgloss.JoinHorizontal(lipgloss.Bottom, rendered,
			tabGapStyle.Render(strings.Repeat(" ", gap)))
	}

	return rendered
}

func countLabel(count int64) string {
	return metaStyle.Render("(" + strconv.FormatInt(count, 10) + ")")
}

// summaryLine is what the status line says when there is nothing to report:
// the size of whatever the active screen is showing part of.
func (m Model) summaryLine() string {
	if m.tab == tabAudit {
		if m.summary.AuditEntries == 0 {
			return " "
		}
		line := fmt.Sprintf("%s · reading the newest %d",
			plural(m.summary.AuditEntries, "entry", "entries"), m.audit.limit)
		if m.audit.actorLabel != "" {
			line += " by " + m.audit.actorLabel
		}
		return line
	}

	if m.tab == tabRooms {
		if m.summary.Rooms == 0 {
			return " "
		}
		return plural(m.summary.Rooms, "room", "rooms")
	}

	if m.summary.Users == 0 {
		return " "
	}
	return plural(m.summary.Users, "account", "accounts") + " · " +
		plural(m.summary.Admins, "administrator", "administrators")
}

// plural writes a count with the right noun after it. Small, and worth it: a
// status line that says "1 entries" is a line somebody wrote once and nobody
// read again.
func plural(count int64, singular, many string) string {
	if count == 1 {
		return "1 " + singular
	}
	return strconv.FormatInt(count, 10) + " " + many
}

func (m Model) statusLine(width int) string {
	if m.status.kind == statusNone {
		// Never an empty string: the line has to keep its height, or the help
		// line jumps up and down as messages come and go.
		return metaStyle.Render(m.summaryLine())
	}

	text := truncate(m.status.text, width)
	switch m.status.kind {
	case statusOK:
		return successStyle.Render("✓ " + text)
	case statusWarn:
		return warningStyle.Render("! " + text)
	case statusError:
		return dangerStyle.Render("✗ " + text)
	case statusNone:
	}
	return " "
}

// centre places a card in the middle of the body area.
func centre(card string, width, height int) string {
	return lipgloss.Place(width, height, lipgloss.Center, lipgloss.Center, card)
}

// cardWidth is how wide a card may be: narrow enough to read, and never wider
// than the window. It is what a card's style is given, which lipgloss counts
// as the text plus the padding, with the border on top of it.
func cardWidth(width int) int {
	return min(max(width-8, 24), 64)
}

// cardContent is the room left for text inside a card, once the three columns
// of padding on each side are taken off. Anything laid out inside a card is
// measured against this and not against cardWidth, or lipgloss wraps it and
// the card grows a line nobody asked for.
func cardContent(width int) int {
	return cardWidth(width) - 6
}
