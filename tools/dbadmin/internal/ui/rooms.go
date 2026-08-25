package ui

import (
	"context"
	"strings"
	"time"

	"github.com/charmbracelet/bubbles/table"
	"github.com/charmbracelet/bubbles/textinput"
	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"

	"github.com/edermanoel94/PartyShare/tools/dbadmin/internal/store"
)

type roomsMode int

const (
	roomsList roomsMode = iota
	roomsDelete
)

type roomsModel struct {
	store   Database
	table   table.Model
	rooms   []store.Room
	visible []store.Room

	// Owners by user id, so a room can show who made it instead of a thirty
	// two character identifier. Fed from the account list the users screen
	// already reads: an owner whose account has been deleted resolves to
	// nothing, and the identifier is shown instead of a blank.
	owners map[string]string

	kinds     []columnKind
	filter    textinput.Model
	filtering bool
	mode      roomsMode
	// target is the room the open confirmation is about.
	target        store.Room
	width, height int
	loaded        bool
}

func newRoomsModel(rooms Database) *roomsModel {
	filter := textinput.New()
	filter.Prompt = "filter "
	filter.Placeholder = "room, name or owner"
	filter.CharLimit = 64

	created := &roomsModel{store: rooms, filter: filter, owners: map[string]string{}}
	columns, kinds := created.columns(80)
	created.kinds = kinds
	created.table = table.New(table.WithFocused(true), table.WithColumns(columns))
	created.table.SetStyles(tableStyles())
	return created
}

// The room identifier is what somebody reads out to invite a person, so it is
// the column that never goes. The owner is the first to be dropped: it is the
// one value here that can be looked up on the users screen.
var roomColumns = []columnSpec{
	{kind: columnRoomID, title: "ROOM", minimum: 6, priority: 0},
	{kind: columnRoomName, title: "NAME", minimum: 10, flexible: true, priority: 1},
	{kind: columnRoomOwner, title: "OWNER", minimum: 12, flexible: true, priority: 3},
	{kind: columnRoomCreated, title: "CREATED", minimum: 19, priority: 2},
}

func (m *roomsModel) columns(width int) ([]table.Column, []columnKind) {
	return fitColumns(width, roomColumns)
}

func (m *roomsModel) setSize(width, height int) {
	m.width, m.height = width, height

	columns, kinds := m.columns(width)
	m.table.SetColumns(columns)
	m.kinds = kinds
	m.table.SetWidth(width)
	m.table.SetHeight(max(height-m.headerLines()-tableHeaderLines, 3))

	m.applyFilter()
}

func (m *roomsModel) headerLines() int {
	if m.filtering || m.filter.Value() != "" {
		return 2
	}
	return 0
}

func (m *roomsModel) setRooms(rooms []store.Room) {
	m.rooms = rooms
	m.loaded = true
	m.applyFilter()
}

// setAccounts is how the owner column learns names. Called with whatever the
// users screen last read, because both screens are filled from the same
// refresh and neither owns the other's data.
func (m *roomsModel) setAccounts(accounts []store.Account) {
	owners := make(map[string]string, len(accounts))
	for _, account := range accounts {
		owners[account.UserID] = account.Username
	}
	m.owners = owners
	m.applyFilter()
}

func (m *roomsModel) applyFilter() {
	needle := strings.ToLower(strings.TrimSpace(m.filter.Value()))
	m.visible = m.visible[:0]
	for _, room := range m.rooms {
		if needle == "" || strings.Contains(strings.ToLower(m.roomHaystack(room)), needle) {
			m.visible = append(m.visible, room)
		}
	}

	columns := m.table.Columns()
	rows := make([]table.Row, 0, len(m.visible))
	for _, room := range m.visible {
		row := make(table.Row, 0, len(m.kinds))
		for i, kind := range m.kinds {
			row = append(row, truncate(m.roomCell(room, kind), columns[i].Width))
		}
		rows = append(rows, row)
	}
	m.table.SetRows(rows)
	m.table.SetCursor(0)
}

func (m *roomsModel) ownerLabel(room store.Room) string {
	if username, known := m.owners[room.OwnerID]; known && username != "" {
		return username
	}
	if room.OwnerID == "" {
		return "-"
	}
	// An account that was deleted out from under its room. Worth seeing as
	// what it is rather than as a blank, which reads like a bug.
	return shortID(room.OwnerID)
}

func (m *roomsModel) roomCell(room store.Room, kind columnKind) string {
	switch kind {
	case columnRoomID:
		return room.ID
	case columnRoomName:
		return room.Name
	case columnRoomOwner:
		return m.ownerLabel(room)
	case columnRoomCreated:
		if room.CreatedAt == 0 {
			return "-"
		}
		return time.Unix(room.CreatedAt, 0).Format("2006-01-02 15:04:05")
	case columnUsername, columnDisplayName, columnRole, columnUserID, columnCreated,
		columnRestrictions, columnTime, columnActor, columnAction, columnTarget, columnDetail:
	}
	return ""
}

func (m *roomsModel) roomHaystack(room store.Room) string {
	return strings.Join([]string{room.ID, room.Name, room.OwnerID, m.ownerLabel(room)}, " ")
}

func (m *roomsModel) selected() (store.Room, bool) {
	cursor := m.table.Cursor()
	if cursor < 0 || cursor >= len(m.visible) {
		return store.Room{}, false
	}
	return m.visible[cursor], true
}

func (m *roomsModel) capturesKeys() bool {
	return m.filtering || m.mode != roomsList
}

func (m *roomsModel) Update(message tea.Msg) tea.Cmd {
	key, isKey := message.(tea.KeyMsg)
	if !isKey {
		return nil
	}

	if m.mode == roomsDelete {
		switch key.String() {
		case "y":
			target := m.target
			m.mode = roomsList
			return func() tea.Msg {
				if err := m.store.DeleteRoom(context.Background(), target.ID); err != nil {
					return outcomeFor(err, "Room "+target.ID+" was deleted")
				}
				return doneMsg{text: "Room " + target.ID + " was deleted"}
			}
		case "n", "esc", "q":
			m.mode = roomsList
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
	case "d":
		if room, ok := m.selected(); ok {
			m.target = room
			m.mode = roomsDelete
		}
		return nil
	}

	m.table, _ = m.table.Update(key)
	return nil
}

func (m *roomsModel) updateFilter(key tea.KeyMsg) tea.Cmd {
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

func (m *roomsModel) View() string {
	switch m.mode {
	case roomsDelete:
		return centre(m.deleteView(), m.width, m.height)
	case roomsList:
	}

	if !m.loaded {
		return emptyStyle.Render("Reading the rooms…")
	}
	if len(m.rooms) == 0 {
		return emptyStyle.Render(
			"No rooms in this database. The server writes one every time somebody creates a room.")
	}

	body := m.table.View()
	if m.headerLines() > 0 {
		body = m.filterLine() + "\n\n" + body
	}
	return body
}

func (m *roomsModel) filterLine() string {
	if m.filtering {
		return m.filter.View()
	}
	return metaStyle.Render("filter " + m.filter.Value())
}

// The warning is the whole point of this card. Deleting the document is not
// the same as the server closing the room, and somebody who thinks it is will
// delete a room people are sitting in and wonder why nothing happened.
func (m *roomsModel) deleteView() string {
	content := cardContent(m.width)
	lines := []string{
		cardTitleStyle.Render("Delete room " + m.target.ID + "?"),
		cardLabelStyle.Render("Name  ") + cardValueStyle.Render(m.target.Name),
		cardLabelStyle.Render("Owner ") + cardValueStyle.Render(m.ownerLabel(m.target)),
		"",
		lipgloss.NewStyle().Width(content).Render(
			"The room is removed from the database, so it does not come back at the " +
				"next start. A running server keeps what it already has in memory: " +
				"nobody is evicted and the identifier goes on working until that " +
				"process ends. Close it from the admin panel if the server is up."),
		"",
		helpLine(content, keyHint("y", "delete"), keyHint("n", "keep")),
	}
	return dangerCard().Width(cardWidth(m.width)).Render(strings.Join(lines, "\n"))
}

func (m *roomsModel) help() string {
	switch m.mode {
	case roomsDelete:
		return ""
	case roomsList:
	}
	if m.filtering {
		return helpLine(m.width, keyHint("enter", "keep filter"), keyHint("esc", "clear"))
	}
	return helpLine(
		m.width,
		keyHint("↑↓", "move"),
		keyHint("d", "delete"),
		keyHint("/", "filter"),
		keyHint("r", "refresh"),
		keyHint("tab", "next screen"),
		keyHint("q", "quit"),
	)
}
