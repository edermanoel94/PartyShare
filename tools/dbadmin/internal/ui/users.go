package ui

import (
	"context"
	"strings"

	"github.com/charmbracelet/bubbles/table"
	"github.com/charmbracelet/bubbles/textinput"
	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"

	"github.com/edermanoel94/PartyShare/tools/dbadmin/internal/store"
)

// What the users screen is showing. Everything but usersList takes over the
// body and the keyboard, which is why a mode and not a stack of booleans: two
// booleans allow a state where a form and a confirmation are both open.
type userMode int

const (
	usersList userMode = iota
	usersCreate
	usersEdit
	usersPassword
	usersDelete
	usersDetail
)

// The fields of the create form, by position. Named because the form is read
// back by index and a bare 3 in the middle of a submit handler is a bug
// waiting for somebody to insert a field above it.
const (
	createUsername = iota
	createPassword
	createConfirm
	createDisplayName
	createAvatar
	createRole
)

const (
	editUsername = iota
	editDisplayName
	editAvatar
	editRole
)

const (
	passwordNew = iota
	passwordConfirm
)

type usersModel struct {
	store    Database
	table    table.Model
	accounts []store.Account
	// visible is accounts after the filter, and is what the table rows are
	// built from. The table hands back a cursor, and the cursor indexes this.
	visible []store.Account
	// kinds is which value each column of the table is showing, which depends
	// on how many of them fitted the window.
	kinds     []columnKind
	filter    textinput.Model
	filtering bool
	mode      userMode
	form      *form
	// target is the account the open form, confirmation or detail card is
	// about, captured when it opened. Read from the table on every keystroke
	// instead, a refresh arriving mid-edit would move it under the form.
	target        store.Account
	width, height int
	loaded        bool
}

func newUsersModel(accounts Database) *usersModel {
	filter := textinput.New()
	filter.Prompt = "filter "
	filter.Placeholder = "username or display name"
	filter.CharLimit = 64

	created := &usersModel{store: accounts, filter: filter}
	columns, kinds := created.columns(80)
	created.kinds = kinds
	created.table = table.New(table.WithFocused(true), table.WithColumns(columns))
	created.table.SetStyles(tableStyles())
	return created
}

func tableStyles() table.Styles {
	styles := table.DefaultStyles()
	styles.Header = styles.Header.
		BorderStyle(lipgloss.NormalBorder()).
		BorderForeground(subtle).
		BorderBottom(true).
		Bold(true).
		Foreground(muted)
	styles.Selected = styles.Selected.
		Foreground(accent).
		Background(ground).
		Bold(true)
	styles.Cell = styles.Cell.Padding(0, 1)
	return styles
}

// The columns of the users table, in the order they are read and with the
// order they are given up in when the window is narrow.
//
// The username goes last, because a list of accounts with no usernames in it
// is not a list of accounts. The identifier goes first: it is the one column
// whose value is also on the detail card, one keystroke away.
var userColumns = []columnSpec{
	{kind: columnUsername, title: "USERNAME", minimum: 10, flexible: true, priority: 0},
	{kind: columnDisplayName, title: "DISPLAY NAME", minimum: 12, flexible: true, priority: 2},
	{kind: columnRole, title: "ROLE", minimum: 5, priority: 1},
	{kind: columnUserID, title: "USER ID", minimum: 14, priority: 4},
	{kind: columnCreated, title: "CREATED", minimum: 16, priority: 3},
}

func (m *usersModel) columns(width int) ([]table.Column, []columnKind) {
	return fitColumns(width, userColumns)
}

func (m *usersModel) setSize(width, height int) {
	m.width, m.height = width, height

	columns, kinds := m.columns(width)
	m.table.SetColumns(columns)
	m.kinds = kinds
	m.table.SetWidth(width)
	// The table's height counts its rows and not its own two header lines, so
	// they come off the body first. A frame taller than the window is not a
	// cosmetic problem: the renderer scrolls the top of it out of view.
	tableHeight := height - tableHeaderLines
	if m.filtering || m.filter.Value() != "" {
		// The filter line above the table takes two of the body's rows.
		tableHeight -= 2
	}
	m.table.SetHeight(max(tableHeight, 3))

	// The rows carry values already cut to the column they sit in, so a new
	// width means new rows and not only new columns.
	m.applyFilter(m.selectedID())
}

// setAccounts takes a fresh list, keeping the cursor on the same account
// rather than the same row, so that a refresh after a delete does not leave
// the selection on whoever moved up into the gap.
func (m *usersModel) setAccounts(accounts []store.Account) {
	selected := m.selectedID()
	m.accounts = accounts
	m.loaded = true
	m.applyFilter(selected)
}

// focusOn puts the cursor on one account by identifier, which is how a change
// and the refresh that follows it end with the row somebody just edited still
// under the cursor.
func (m *usersModel) focusOn(userID string) {
	for i, account := range m.visible {
		if account.UserID == userID {
			m.table.SetCursor(i)
			return
		}
	}
}

func (m *usersModel) selectedID() string {
	if cursor := m.table.Cursor(); cursor >= 0 && cursor < len(m.visible) {
		return m.visible[cursor].UserID
	}
	return ""
}

func (m *usersModel) selected() (store.Account, bool) {
	if cursor := m.table.Cursor(); cursor >= 0 && cursor < len(m.visible) {
		return m.visible[cursor], true
	}
	return store.Account{}, false
}

func (m *usersModel) applyFilter(keepSelected string) {
	needle := strings.ToLower(strings.TrimSpace(m.filter.Value()))
	m.visible = m.visible[:0]
	for _, account := range m.accounts {
		if needle == "" ||
			strings.Contains(strings.ToLower(account.Username), needle) ||
			strings.Contains(strings.ToLower(account.DisplayName), needle) ||
			strings.Contains(strings.ToLower(account.UserID), needle) {
			m.visible = append(m.visible, account)
		}
	}

	columns := m.table.Columns()
	rows := make([]table.Row, 0, len(m.visible))
	for _, account := range m.visible {
		row := make(table.Row, 0, len(m.kinds))
		for i, kind := range m.kinds {
			row = append(row, truncate(userCell(account, kind), columns[i].Width))
		}
		rows = append(rows, row)
	}
	m.table.SetRows(rows)

	for i, account := range m.visible {
		if account.UserID == keepSelected {
			m.table.SetCursor(i)
			return
		}
	}
	m.table.SetCursor(0)
}

// userCell is one value of one account, as a column shows it.
func userCell(account store.Account, kind columnKind) string {
	switch kind {
	case columnUsername:
		return account.Username
	case columnDisplayName:
		return account.DisplayName
	case columnRole:
		return string(account.Role)
	case columnUserID:
		return account.UserID
	case columnCreated:
		if when := account.Created(); !when.IsZero() {
			return when.Format("2006-01-02 15:04")
		}
		return "-"
	case columnTime, columnActor, columnAction, columnTarget, columnDetail:
	}
	return ""
}

// capturesKeys says whether the screen is typing, and therefore whether the
// single letter shortcuts of the application belong to it or to this model.
func (m *usersModel) capturesKeys() bool {
	return m.filtering || m.mode != usersList
}

func (m *usersModel) Update(message tea.Msg) tea.Cmd {
	key, isKey := message.(tea.KeyMsg)
	if !isKey {
		return nil
	}

	if m.mode != usersList {
		return m.updateOverlay(key)
	}
	if m.filtering {
		return m.updateFilter(key)
	}
	return m.updateList(key)
}

func (m *usersModel) updateList(key tea.KeyMsg) tea.Cmd {
	switch key.String() {
	case "/":
		m.filtering = true
		m.filter.Focus()
		m.setSize(m.width, m.height)
		return textinput.Blink
	case "esc":
		if m.filter.Value() != "" {
			m.filter.SetValue("")
			m.applyFilter(m.selectedID())
		}
		return nil
	case "n":
		m.openCreate()
		return textinput.Blink
	case "e":
		if account, ok := m.selected(); ok {
			m.openEdit(account)
			return textinput.Blink
		}
		return nil
	case "p":
		if account, ok := m.selected(); ok {
			m.openPassword(account)
			return textinput.Blink
		}
		return nil
	case "d":
		if account, ok := m.selected(); ok {
			m.target = account
			m.mode = usersDelete
		}
		return nil
	case "enter":
		if account, ok := m.selected(); ok {
			m.target = account
			m.mode = usersDetail
		}
		return nil
	}

	var command tea.Cmd
	m.table, command = m.table.Update(key)
	return command
}

func (m *usersModel) updateFilter(key tea.KeyMsg) tea.Cmd {
	switch key.String() {
	case "esc":
		m.filter.SetValue("")
		m.stopFiltering()
		return nil
	case "enter":
		m.stopFiltering()
		return nil
	}

	var command tea.Cmd
	m.filter, command = m.filter.Update(key)
	m.applyFilter(m.selectedID())
	return command
}

func (m *usersModel) stopFiltering() {
	m.filtering = false
	m.filter.Blur()
	m.applyFilter(m.selectedID())
	m.setSize(m.width, m.height)
}

func (m *usersModel) updateOverlay(key tea.KeyMsg) tea.Cmd {
	if m.mode == usersDetail {
		if key.String() == "esc" || key.String() == "enter" || key.String() == "q" {
			m.mode = usersList
		}
		return nil
	}
	if m.mode == usersDelete {
		switch key.String() {
		case "y":
			target := m.target
			m.mode = usersList
			return func() tea.Msg {
				if err := m.store.DeleteAccount(context.Background(), target.UserID); err != nil {
					return outcomeFor(err, "Account \""+target.Username+"\" was deleted")
				}
				return doneMsg{text: "Account \"" + target.Username + "\" was deleted"}
			}
		case "n", "esc", "q":
			m.mode = usersList
		}
		return nil
	}

	result, command := m.form.Update(key)
	switch result {
	case formCancelled:
		m.mode = usersList
		return nil
	case formSubmitted:
		return m.submit()
	case formPending:
	}
	return command
}

func (m *usersModel) openCreate() {
	m.form = newForm("New account", "", "create",
		textField("Username", "", "ana", "What they log in with. Has to be unique."),
		secretField("Password", "Stored as scrypt, never in plain text."),
		secretField("Confirm password", ""),
		textField("Display name", "", "Ana Souza", "Shown in a room. Defaults to the username."),
		textField("Avatar", "", "https://…", "URL or path. Empty falls back to initials."),
		choiceField("Role", roleChoices(), 0,
			"An administrator can remove and mute participants and manage accounts."),
	)
	m.form.width = m.width
	m.mode = usersCreate
}

func (m *usersModel) openEdit(account store.Account) {
	role := 0
	if account.IsAdmin() {
		role = 1
	}
	m.form = newForm("Edit "+account.Username, "Identifier "+account.UserID, "save",
		textField("Username", account.Username, "", "Has to stay unique."),
		textField("Display name", account.DisplayName, "", ""),
		textField("Avatar", account.Avatar, "https://…", ""),
		choiceField("Role", roleChoices(), role,
			"The last administrator cannot be demoted."),
	)
	m.form.width = m.width
	m.target = account
	m.mode = usersEdit
}

func (m *usersModel) openPassword(account store.Account) {
	m.form = newForm("Password of "+account.Username,
		"The account keeps its identifier and its sessions. Only the stored "+
			"credentials change.", "set",
		secretField("New password", ""),
		secretField("Confirm password", ""),
	)
	m.form.width = m.width
	m.target = account
	m.mode = usersPassword
}

func roleChoices() []string {
	choices := make([]string, 0, len(store.Roles))
	for _, role := range store.Roles {
		choices = append(choices, string(role))
	}
	return choices
}

func (m *usersModel) submit() tea.Cmd {
	switch m.mode {
	case usersCreate:
		return m.submitCreate()
	case usersEdit:
		return m.submitEdit()
	case usersPassword:
		return m.submitPassword()
	case usersList, usersDelete, usersDetail:
	}
	return nil
}

func (m *usersModel) submitCreate() tea.Cmd {
	spec := store.NewAccount{
		Username:    m.form.value(createUsername),
		Password:    m.form.secret(createPassword),
		DisplayName: m.form.value(createDisplayName),
		Avatar:      m.form.value(createAvatar),
		Role:        store.RoleFromString(m.form.value(createRole)),
	}
	if spec.Username == "" {
		m.form.failure = "A username is required."
		return nil
	}
	if spec.Password == "" {
		m.form.failure = "A password is required."
		return nil
	}
	if spec.Password != m.form.secret(createConfirm) {
		m.form.failure = "The two passwords are not the same."
		return nil
	}

	m.mode = usersList
	return func() tea.Msg {
		account, err := m.store.CreateAccount(context.Background(), spec)
		if err != nil {
			return outcomeFor(err, "Account \""+spec.Username+"\" was created")
		}
		return doneMsg{
			text:  "Account \"" + account.Username + "\" was created as " + string(account.Role),
			focus: account.UserID,
		}
	}
}

func (m *usersModel) submitEdit() tea.Cmd {
	updated := m.target
	updated.Username = m.form.value(editUsername)
	updated.DisplayName = m.form.value(editDisplayName)
	updated.Avatar = m.form.value(editAvatar)
	updated.Role = store.RoleFromString(m.form.value(editRole))

	if updated.Username == "" {
		m.form.failure = "A username is required."
		return nil
	}

	m.mode = usersList
	return func() tea.Msg {
		if err := m.store.UpdateAccount(context.Background(), updated); err != nil {
			return outcomeFor(err, "Account \""+updated.Username+"\" was saved")
		}
		return doneMsg{
			text:  "Account \"" + updated.Username + "\" was saved",
			focus: updated.UserID,
		}
	}
}

func (m *usersModel) submitPassword() tea.Cmd {
	password := m.form.secret(passwordNew)
	if password == "" {
		m.form.failure = "A password is required."
		return nil
	}
	if password != m.form.secret(passwordConfirm) {
		m.form.failure = "The two passwords are not the same."
		return nil
	}

	target := m.target
	m.mode = usersList
	return func() tea.Msg {
		if err := m.store.SetPassword(context.Background(), target.UserID, password); err != nil {
			return outcomeFor(err, "The password of \""+target.Username+"\" was changed")
		}
		return doneMsg{
			text:  "The password of \"" + target.Username + "\" was changed",
			focus: target.UserID,
		}
	}
}

func (m *usersModel) View() string {
	switch m.mode {
	case usersCreate, usersEdit, usersPassword:
		m.form.width = m.width
		return centre(m.form.View(), m.width, m.height)
	case usersDelete:
		return centre(m.deleteView(), m.width, m.height)
	case usersDetail:
		return centre(m.detailView(), m.width, m.height)
	case usersList:
	}

	if !m.loaded {
		return emptyStyle.Render("Reading the accounts…")
	}
	if len(m.accounts) == 0 {
		return emptyStyle.Render(
			"No accounts in this database yet. Press n to create the first one.")
	}

	body := m.table.View()
	if len(m.visible) == 0 {
		body = emptyStyle.Render("No account matches this filter.")
	}
	if m.filtering || m.filter.Value() != "" {
		return m.filter.View() + "\n\n" + body
	}
	return body
}

func (m *usersModel) deleteView() string {
	content := cardContent(m.width)
	lines := []string{
		cardTitleStyle.Render("Delete " + m.target.Username + "?"),
		cardLabelStyle.Render("Identifier ") + cardValueStyle.Render(m.target.UserID),
		cardLabelStyle.Render("Role       ") + roleLabel(m.target.Role),
		"",
		lipgloss.NewStyle().Width(content).Render(
			"The account is removed from the database. A running server keeps " +
				"whatever it already has in memory: a session opened before now goes on " +
				"working until it expires, and the account is not evicted from its room. " +
				"Do that from the admin panel if the server is up."),
		"",
		helpLine(content, keyHint("y", "delete"), keyHint("n", "keep")),
	}
	return dangerCard().Width(cardWidth(m.width)).Render(strings.Join(lines, "\n"))
}

func (m *usersModel) detailView() string {
	content := cardContent(m.width)
	created := "unknown"
	if when := m.target.Created(); !when.IsZero() {
		created = when.Format("2006-01-02 15:04:05")
	}
	avatar := m.target.Avatar
	if avatar == "" {
		avatar = metaStyle.Render("none, the client falls back to initials")
	}

	lines := []string{
		cardTitleStyle.Render(m.target.Username),
		cardLabelStyle.Render("Display name ") + cardValueStyle.Render(m.target.DisplayName),
		cardLabelStyle.Render("Role         ") + roleLabel(m.target.Role),
		cardLabelStyle.Render("Identifier   ") + cardValueStyle.Render(m.target.UserID),
		cardLabelStyle.Render("Avatar       ") + avatar,
		cardLabelStyle.Render("Created      ") + cardValueStyle.Render(created),
		// The salt and the hash are read by this program and shown by nothing.
		// A credential on a screen is a credential in a screenshot.
		cardLabelStyle.Render("Password     ") + metaStyle.Render("stored as scrypt, not shown"),
		cardFooterStyle.Render(helpLine(content, keyHint("esc", "close"))),
	}
	return cardStyle.Width(cardWidth(m.width)).Render(strings.Join(lines, "\n"))
}

func roleLabel(role store.Role) string {
	if role == store.RoleAdmin {
		return adminBadgeStyle.Render(string(role))
	}
	return cardValueStyle.Render(string(role))
}

func (m *usersModel) help() string {
	switch m.mode {
	case usersCreate, usersEdit, usersPassword, usersDelete, usersDetail:
		return ""
	case usersList:
	}
	if m.filtering {
		return helpLine(m.width, keyHint("enter", "keep filter"), keyHint("esc", "clear"))
	}
	return helpLine(
		m.width,
		keyHint("↑↓", "move"),
		keyHint("enter", "details"),
		keyHint("n", "new"),
		keyHint("e", "edit"),
		keyHint("p", "password"),
		keyHint("d", "delete"),
		keyHint("/", "filter"),
		keyHint("r", "refresh"),
		keyHint("tab", "audit"),
		keyHint("q", "quit"),
	)
}
