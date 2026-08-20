package ui

import (
	"strconv"
	"strings"

	"github.com/charmbracelet/bubbles/table"
	"github.com/charmbracelet/bubbles/textinput"
	tea "github.com/charmbracelet/bubbletea"

	"github.com/edermanoel94/PartyShare/tools/dbadmin/internal/store"
)

// The sizes the l key moves through. Every one of them is a page somebody
// might want and the largest is the store's own ceiling.
var auditLimits = []int{100, store.DefaultAuditLimit, 500, 1000, store.MaxAuditLimit}

type auditMode int

const (
	auditList auditMode = iota
	auditDetail
)

type auditModel struct {
	store   Database
	table   table.Model
	entries []store.AuditEntry
	visible []store.AuditEntry

	// kinds is which value each column of the table is showing, which depends
	// on how many of them fitted the window.
	kinds     []columnKind
	filter    textinput.Model
	filtering bool
	// Set by the a key from a selected entry. Unlike the text filter, which is
	// applied here over what was read, this one goes into the query: the log
	// is the collection that grows forever, and "the last two hundred entries
	// of this actor" is a different set from "this actor's entries among the
	// last two hundred".
	actorID       string
	actorLabel    string
	limit         int
	mode          auditMode
	target        store.AuditEntry
	width, height int
	loaded        bool
}

func newAuditModel(entries Database) *auditModel {
	filter := textinput.New()
	filter.Prompt = "filter "
	filter.Placeholder = "action, actor, target or detail"
	filter.CharLimit = 64

	created := &auditModel{store: entries, filter: filter, limit: store.DefaultAuditLimit}
	columns, kinds := created.columns(80)
	created.kinds = kinds
	created.table = table.New(table.WithFocused(true), table.WithColumns(columns))
	created.table.SetStyles(tableStyles())
	return created
}

// The columns of the audit table. The time and the action are what an entry
// is; the target, which is an identifier, is the first to go.
var auditColumns = []columnSpec{
	{kind: columnTime, title: "TIME", minimum: 19, priority: 0},
	{kind: columnActor, title: "ACTOR", minimum: 12, flexible: true, priority: 2},
	{kind: columnAction, title: "ACTION", minimum: 12, priority: 1},
	{kind: columnTarget, title: "TARGET", minimum: 12, priority: 4},
	{kind: columnDetail, title: "DETAIL", minimum: 12, flexible: true, priority: 3},
}

func (m *auditModel) columns(width int) ([]table.Column, []columnKind) {
	return fitColumns(width, auditColumns)
}

func (m *auditModel) setSize(width, height int) {
	m.width, m.height = width, height

	columns, kinds := m.columns(width)
	m.table.SetColumns(columns)
	m.kinds = kinds
	m.table.SetWidth(width)

	m.table.SetHeight(max(height-m.headerLines()-tableHeaderLines, 3))

	// The rows carry values already cut to the column they sit in, so a new
	// width means new rows and not only new columns.
	m.applyFilter()
}

// headerLines is how much of the body the filter line above the table takes,
// counted in one place so that the table height and the view cannot disagree.
func (m *auditModel) headerLines() int {
	if m.filtering || m.filter.Value() != "" || m.actorID != "" {
		return 2
	}
	return 0
}

func (m *auditModel) query() store.AuditQuery {
	return store.AuditQuery{Limit: m.limit, ActorID: m.actorID}
}

func (m *auditModel) setEntries(entries []store.AuditEntry) {
	m.entries = entries
	m.loaded = true
	m.applyFilter()
}

func (m *auditModel) applyFilter() {
	needle := strings.ToLower(strings.TrimSpace(m.filter.Value()))
	m.visible = m.visible[:0]
	for _, entry := range m.entries {
		if needle == "" || strings.Contains(strings.ToLower(entryHaystack(entry)), needle) {
			m.visible = append(m.visible, entry)
		}
	}

	columns := m.table.Columns()
	rows := make([]table.Row, 0, len(m.visible))
	for _, entry := range m.visible {
		row := make(table.Row, 0, len(m.kinds))
		for i, kind := range m.kinds {
			row = append(row, truncate(auditCell(entry, kind), columns[i].Width))
		}
		rows = append(rows, row)
	}
	m.table.SetRows(rows)
	m.table.SetCursor(0)
}

// auditCell is one value of one entry, as a column shows it.
func auditCell(entry store.AuditEntry, kind columnKind) string {
	switch kind {
	case columnTime:
		if at := entry.When(); !at.IsZero() {
			return at.Format("2006-01-02 15:04:05")
		}
		return "-"
	case columnActor:
		return entry.ActorUsername
	case columnAction:
		return entry.Action
	case columnTarget:
		return shortID(entry.TargetID)
	case columnDetail:
		return entry.Detail
	case columnUsername, columnDisplayName, columnRole, columnUserID, columnCreated:
	}
	return ""
}

func entryHaystack(entry store.AuditEntry) string {
	return strings.Join([]string{
		entry.ActorUsername, entry.ActorID, entry.Action,
		entry.TargetID, entry.RoomID, entry.Detail,
	}, " ")
}

// shortID keeps the first eight characters of a thirty two character
// identifier. Enough to match one against another on the screen, and short
// enough that the detail column keeps its width.
func shortID(id string) string {
	if len(id) <= 12 {
		return id
	}
	return id[:8] + "…"
}

func (m *auditModel) capturesKeys() bool {
	return m.filtering || m.mode != auditList
}

func (m *auditModel) Update(message tea.Msg) tea.Cmd {
	key, isKey := message.(tea.KeyMsg)
	if !isKey {
		return nil
	}

	if m.mode == auditDetail {
		if key.String() == "esc" || key.String() == "enter" || key.String() == "q" {
			m.mode = auditList
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
		changed := m.actorID != ""
		m.actorID, m.actorLabel = "", ""
		m.filter.SetValue("")
		m.applyFilter()
		m.setSize(m.width, m.height)
		if changed {
			return loadAudit(m.store, m.query())
		}
		return nil
	case "a":
		return m.toggleActor()
	case "l":
		m.limit = nextLimit(m.limit)
		return loadAudit(m.store, m.query())
	case "enter":
		if cursor := m.table.Cursor(); cursor >= 0 && cursor < len(m.visible) {
			m.target = m.visible[cursor]
			m.mode = auditDetail
		}
		return nil
	}

	var command tea.Cmd
	m.table, command = m.table.Update(key)
	return command
}

func (m *auditModel) toggleActor() tea.Cmd {
	if m.actorID != "" {
		m.actorID, m.actorLabel = "", ""
		m.setSize(m.width, m.height)
		return loadAudit(m.store, m.query())
	}

	cursor := m.table.Cursor()
	if cursor < 0 || cursor >= len(m.visible) {
		return nil
	}
	entry := m.visible[cursor]
	if entry.ActorID == "" {
		return nil
	}
	m.actorID = entry.ActorID
	m.actorLabel = entry.ActorUsername
	if m.actorLabel == "" {
		m.actorLabel = shortID(entry.ActorID)
	}
	m.setSize(m.width, m.height)
	return loadAudit(m.store, m.query())
}

func nextLimit(current int) int {
	for i, limit := range auditLimits {
		if limit == current {
			return auditLimits[(i+1)%len(auditLimits)]
		}
	}
	return store.DefaultAuditLimit
}

func (m *auditModel) updateFilter(key tea.KeyMsg) tea.Cmd {
	switch key.String() {
	case "esc":
		m.filter.SetValue("")
		m.filtering = false
		m.filter.Blur()
		m.applyFilter()
		m.setSize(m.width, m.height)
		return nil
	case "enter":
		m.filtering = false
		m.filter.Blur()
		m.setSize(m.width, m.height)
		return nil
	}

	var command tea.Cmd
	m.filter, command = m.filter.Update(key)
	m.applyFilter()
	return command
}

func (m *auditModel) View() string {
	if m.mode == auditDetail {
		return centre(m.detailView(), m.width, m.height)
	}
	if !m.loaded {
		return emptyStyle.Render("Reading the audit log…")
	}
	if len(m.entries) == 0 {
		message := "The audit log is empty. It fills as administrators act, " +
			"through the client panel or through this program."
		if m.actorID != "" {
			message = "This actor has nothing in the audit log."
		}
		return m.bodyHeader() + emptyStyle.Render(message)
	}

	body := m.table.View()
	if len(m.visible) == 0 {
		body = emptyStyle.Render("No entry matches this filter.")
	}
	return m.bodyHeader() + body
}

// bodyHeader is the line that says which query produced what is below, and is
// there only when the answer is not "everything".
func (m *auditModel) bodyHeader() string {
	if m.headerLines() == 0 {
		return ""
	}
	line := ""
	if m.filtering || m.filter.Value() != "" {
		line = m.filter.View()
	}
	if m.actorID != "" {
		badge := metaStyle.Render("actor ") + successStyle.Render(m.actorLabel) +
			metaStyle.Render(" · a to clear")
		if line != "" {
			line += metaStyle.Render("   ")
		}
		line += badge
	}
	return line + "\n\n"
}

func (m *auditModel) detailView() string {
	content := cardContent(m.width)
	when := "unknown"
	if at := m.target.When(); !at.IsZero() {
		when = at.Format("2006-01-02 15:04:05 MST")
	}

	lines := []string{
		cardTitleStyle.Render(m.target.Action),
		cardLabelStyle.Render("When    ") + cardValueStyle.Render(when),
		cardLabelStyle.Render("Actor   ") + cardValueStyle.Render(actorLabel(m.target)),
		cardLabelStyle.Render("Target  ") + cardValueStyle.Render(orDash(m.target.TargetID)),
		cardLabelStyle.Render("Room    ") + cardValueStyle.Render(orDash(m.target.RoomID)),
		cardLabelStyle.Render("Entry   ") + metaStyle.Render(orDash(m.target.ID)),
		"",
		cardLabelStyle.Render("Detail"),
		cardValueStyle.Width(content).Render(orDash(m.target.Detail)),
		cardFooterStyle.Render(helpLine(content, keyHint("esc", "close"))),
	}
	return cardStyle.Width(cardWidth(m.width)).Render(strings.Join(lines, "\n"))
}

// actorLabel keeps both halves of who acted, for the reason the server keeps
// both: the account may be gone, and a log that then reads "who: unknown" has
// lost the thing it was written for.
func actorLabel(entry store.AuditEntry) string {
	switch {
	case entry.ActorUsername != "" && entry.ActorID != "":
		return entry.ActorUsername + "  " + metaStyle.Render(entry.ActorID)
	case entry.ActorUsername != "":
		return entry.ActorUsername
	case entry.ActorID != "":
		return entry.ActorID
	default:
		return "-"
	}
}

func orDash(value string) string {
	if value == "" {
		return "-"
	}
	return value
}

func (m *auditModel) help() string {
	if m.mode == auditDetail {
		return ""
	}
	if m.filtering {
		return helpLine(m.width, keyHint("enter", "keep filter"), keyHint("esc", "clear"))
	}
	return helpLine(
		m.width,
		keyHint("↑↓", "move"),
		keyHint("enter", "details"),
		keyHint("a", "only this actor"),
		keyHint("l", "read "+strconv.Itoa(nextLimit(m.limit))),
		keyHint("/", "filter"),
		keyHint("r", "refresh"),
		keyHint("tab", "users"),
		keyHint("q", "quit"),
	)
}
