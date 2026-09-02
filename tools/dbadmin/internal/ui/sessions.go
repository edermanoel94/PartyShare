package ui

import (
	"strings"
	"time"

	"github.com/charmbracelet/bubbles/table"
	"github.com/charmbracelet/bubbles/textinput"
	tea "github.com/charmbracelet/bubbletea"

	"github.com/edermanoel94/PartyShare/tools/dbadmin/internal/store"
)

type sessionsMode int

const (
	sessionsList sessionsMode = iota
	sessionsDetail
)

// How many sessions the l key steps through, the same ladder the audit screen
// offers and for the same reason: the first number answers "who is here" and
// the last one answers "when was Bruno last on this machine".
var sessionLimits = []int{200, 500, 1000, 2000}

type sessionsModel struct {
	store    Database
	table    table.Model
	sessions []store.Session
	visible  []store.Session

	// Usernames by user id, so a row can name a person rather than a thirty
	// two character identifier. Fed from the account list the users screen
	// already reads, exactly as the rooms screen resolves an owner.
	usernames map[string]string

	// now is the clock every "is this online" question is asked against, taken
	// once per frame rather than per row.
	//
	// A field and not time.Now() inline, because the answer has to be the same
	// for every row of one screen: a table where the first row was measured
	// against a different instant from the last is a table that can show two
	// people as online and offline for the same last_seen_at. It is also what
	// lets a test say what time it is.
	now func() time.Time

	kinds     []columnKind
	filter    textinput.Model
	filtering bool
	mode      sessionsMode
	target    store.Session
	limit     int

	width, height int
	loaded        bool
}

func newSessionsModel(sessions Database) *sessionsModel {
	filter := textinput.New()
	filter.Prompt = "filter "
	filter.Placeholder = "account or address"
	filter.CharLimit = 64

	created := &sessionsModel{
		store:     sessions,
		filter:    filter,
		usernames: map[string]string{},
		now:       time.Now,
		limit:     store.DefaultSessionLimit,
	}
	columns, kinds := created.columns(80)
	created.kinds = kinds
	created.table = table.New(table.WithFocused(true), table.WithColumns(columns))
	created.table.SetStyles(tableStyles())
	return created
}

// The account and whether they are here are the two columns that never go:
// between them they are the whole question this screen exists to answer. The
// address is next, because it is the other half of what was asked for and
// there is nowhere else in this program to read it. The two timestamps go
// first, in the order that leaves the more useful one behind - when somebody
// arrived matters less than when they were last heard from, which is what
// decides whether the row above is telling the truth.
var sessionColumns = []columnSpec{
	{kind: columnSessionUser, title: "ACCOUNT", minimum: 12, flexible: true, priority: 0},
	{kind: columnSessionState, title: "STATE", minimum: 7, priority: 1},
	{kind: columnSessionIP, title: "ADDRESS", minimum: 15, flexible: true, priority: 2},
	{kind: columnSessionLastSeen, title: "LAST SEEN", minimum: 19, priority: 3},
	{kind: columnSessionConnected, title: "CONNECTED", minimum: 19, priority: 4},
}

func (m *sessionsModel) columns(width int) ([]table.Column, []columnKind) {
	return fitColumns(width, sessionColumns)
}

func (m *sessionsModel) setSize(width, height int) {
	m.width, m.height = width, height

	columns, kinds := m.columns(width)
	m.table.SetColumns(columns)
	m.kinds = kinds
	m.table.SetWidth(width)
	m.table.SetHeight(max(height-m.headerLines()-tableHeaderLines, 3))

	m.applyFilter()
}

func (m *sessionsModel) headerLines() int {
	if m.filtering || m.filter.Value() != "" {
		return 2
	}
	return 0
}

func (m *sessionsModel) setSessions(sessions []store.Session) {
	m.sessions = sessions
	m.loaded = true
	m.applyFilter()
}

// setAccounts is how the account column learns names, from whatever the users
// screen last read. Neither screen owns the other's data; both are filled from
// the same refresh.
func (m *sessionsModel) setAccounts(accounts []store.Account) {
	usernames := make(map[string]string, len(accounts))
	for _, account := range accounts {
		usernames[account.UserID] = account.Username
	}
	m.usernames = usernames
	m.applyFilter()
}

// online is how many of what was read count as somebody who is here. What the
// status line reports, because the tab bar counts documents.
func (m *sessionsModel) online() int {
	now := m.now()
	count := 0
	for _, session := range m.sessions {
		if session.Online(now) {
			count++
		}
	}
	return count
}

func (m *sessionsModel) applyFilter() {
	needle := strings.ToLower(strings.TrimSpace(m.filter.Value()))
	m.visible = m.visible[:0]
	for _, session := range m.sessions {
		if needle == "" || strings.Contains(strings.ToLower(m.haystack(session)), needle) {
			m.visible = append(m.visible, session)
		}
	}

	now := m.now()
	columns := m.table.Columns()
	rows := make([]table.Row, 0, len(m.visible))
	for _, session := range m.visible {
		row := make(table.Row, 0, len(m.kinds))
		for i, kind := range m.kinds {
			row = append(row, truncate(m.cell(session, kind, now), columns[i].Width))
		}
		rows = append(rows, row)
	}
	m.table.SetRows(rows)
	m.table.SetCursor(0)
}

func (m *sessionsModel) accountLabel(session store.Session) string {
	if username, known := m.usernames[session.UserID]; known && username != "" {
		return username
	}
	if session.UserID == "" {
		return "-"
	}
	// A session whose account was deleted out from under it. Worth seeing as
	// what it is rather than as a blank, which reads like a bug.
	return shortID(session.UserID)
}

// stateLabel is the one cell somebody reads this screen for.
//
// Three answers and not two, because "open" and "online" are different claims
// and the difference is the thing an operator has to be able to see. A row
// that is open and stale is a server that stopped without closing its
// sessions, and calling that "online" would be this program repeating a lie
// the database is telling it.
func stateLabel(session store.Session, now time.Time) string {
	switch {
	case session.Online(now):
		return "online"
	case session.Open():
		return "stale"
	default:
		return "ended"
	}
}

func (m *sessionsModel) cell(session store.Session, kind columnKind, now time.Time) string {
	switch kind {
	case columnSessionUser:
		return m.accountLabel(session)
	case columnSessionState:
		return stateLabel(session, now)
	case columnSessionIP:
		if session.IP == "" {
			return "-"
		}
		return session.IP
	case columnSessionConnected:
		return timeLabel(session.ConnectedAt)
	case columnSessionLastSeen:
		return timeLabel(session.LastSeenAt)
	case columnUsername, columnDisplayName, columnRole, columnUserID, columnCreated,
		columnRestrictions, columnTime, columnActor, columnAction, columnTarget, columnDetail,
		columnRoomID, columnRoomName, columnRoomOwner, columnRoomCreated:
	}
	return ""
}

// timeLabel is a stored second as a person reads it, and a dash for the zero
// that means the server never wrote one.
func timeLabel(seconds int64) string {
	if seconds == 0 {
		return "-"
	}
	return time.Unix(seconds, 0).Format("2006-01-02 15:04:05")
}

func (m *sessionsModel) haystack(session store.Session) string {
	return strings.Join([]string{
		session.UserID,
		m.accountLabel(session),
		session.IP,
		stateLabel(session, m.now()),
	}, " ")
}

func (m *sessionsModel) selected() (store.Session, bool) {
	cursor := m.table.Cursor()
	if cursor < 0 || cursor >= len(m.visible) {
		return store.Session{}, false
	}
	return m.visible[cursor], true
}

func (m *sessionsModel) capturesKeys() bool {
	return m.filtering || m.mode != sessionsList
}

// query is how many rows the next read asks for.
func (m *sessionsModel) query() int { return m.limit }

func (m *sessionsModel) Update(message tea.Msg) tea.Cmd {
	key, isKey := message.(tea.KeyMsg)
	if !isKey {
		return nil
	}

	if m.mode == sessionsDetail {
		switch key.String() {
		case "esc", "q", "enter":
			m.mode = sessionsList
		}
		return nil
	}

	if m.filtering {
		return m.updateFilter(key)
	}

	switch key.String() {
	case "/":
		m.filtering = true
		m.filter.Focus()
		m.setSize(m.width, m.height)
		return textinput.Blink
	case "esc":
		if m.filter.Value() != "" {
			m.filter.SetValue("")
			m.setSize(m.width, m.height)
		}
		return nil
	case "enter":
		if session, ok := m.selected(); ok {
			m.target = session
			m.mode = sessionsDetail
		}
		return nil
	case "l":
		m.limit = nextLimit(sessionLimits, m.limit, store.DefaultSessionLimit)
		return loadSessions(m.store, m.limit)
	}

	m.table, _ = m.table.Update(key)
	return nil
}

func (m *sessionsModel) updateFilter(key tea.KeyMsg) tea.Cmd {
	switch key.String() {
	case "enter":
		m.filtering = false
		m.filter.Blur()
		m.setSize(m.width, m.height)
		return nil
	case "esc":
		m.filtering = false
		m.filter.Blur()
		m.filter.SetValue("")
		m.setSize(m.width, m.height)
		return nil
	}

	var command tea.Cmd
	m.filter, command = m.filter.Update(key)
	m.applyFilter()
	return command
}

func (m *sessionsModel) View() string {
	switch m.mode {
	case sessionsDetail:
		return centre(m.detailView(), m.width, m.height)
	case sessionsList:
	}

	if !m.loaded {
		return emptyStyle.Render("Reading the sessions…")
	}
	if len(m.sessions) == 0 {
		return emptyStyle.Render(
			"No sessions in this database. The server writes one every time somebody signs in, " +
				"so an empty list is a server that has not run against this database yet.")
	}

	body := m.table.View()
	if m.headerLines() > 0 {
		body = m.filterLine() + "\n\n" + body
	}
	return body
}

func (m *sessionsModel) filterLine() string {
	if m.filtering {
		return m.filter.View()
	}
	return metaStyle.Render("filter " + m.filter.Value())
}

// The card is where the identifier appears in full, which is the one thing the
// table cannot show, and where "stale" is explained rather than asserted.
func (m *sessionsModel) detailView() string {
	content := cardContent(m.width)
	now := m.now()

	ip := m.target.IP
	if ip == "" {
		ip = metaStyle.Render("not recorded")
	}

	lines := []string{
		cardTitleStyle.Render(m.accountLabel(m.target)),
		cardLabelStyle.Render("Identifier ") + cardValueStyle.Render(m.target.UserID),
		cardLabelStyle.Render("Address    ") + cardValueStyle.Render(ip),
		cardLabelStyle.Render("State      ") + stateBadge(m.target, now),
		cardLabelStyle.Render("Connected  ") + cardValueStyle.Render(timeLabel(m.target.ConnectedAt)),
		cardLabelStyle.Render("Last seen  ") + cardValueStyle.Render(timeLabel(m.target.LastSeenAt)),
		cardLabelStyle.Render("Ended      ") + cardValueStyle.Render(timeLabel(m.target.EndedAt)),
	}

	if m.target.Open() && !m.target.Online(now) {
		lines = append(lines, "",
			warningStyle.Width(content).Render(
				"This session was never closed and has not been heard from for longer than a "+
					"heartbeat allows. That is what a server killed rather than stopped leaves "+
					"behind. The next server to start against this database closes it."))
	}

	lines = append(lines, cardFooterStyle.Render(helpLine(content, keyHint("esc", "close"))))
	return cardStyle.Width(cardWidth(m.width)).Render(strings.Join(lines, "\n"))
}

// stateBadge is the state with a colour on it, for the one place there is room
// to spend on that. The table stays plain: a column of coloured words is a
// column somebody has to read twice.
func stateBadge(session store.Session, now time.Time) string {
	switch {
	case session.Online(now):
		return successStyle.Render("online")
	case session.Open():
		return warningStyle.Render("stale") + metaStyle.Render(" · open, but not answering")
	default:
		return metaStyle.Render("ended")
	}
}

func (m *sessionsModel) help() string {
	switch m.mode {
	case sessionsDetail:
		return ""
	case sessionsList:
	}
	if m.filtering {
		return helpLine(m.width, keyHint("enter", "keep filter"), keyHint("esc", "clear"))
	}
	return helpLine(
		m.width,
		keyHint("↑↓", "move"),
		keyHint("enter", "details"),
		keyHint("l", "read more"),
		keyHint("/", "filter"),
		keyHint("r", "refresh"),
		keyHint("tab", "next screen"),
		keyHint("q", "quit"),
	)
}
