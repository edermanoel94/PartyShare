package ui

import (
	"strings"

	"github.com/charmbracelet/bubbles/textinput"
	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
)

// The three answers a form gives back to whoever is showing it.
type formResult int

const (
	formPending formResult = iota
	formSubmitted
	formCancelled
)

type fieldKind int

const (
	fieldText fieldKind = iota
	// fieldSecret is a text field that never shows what was typed. Used for
	// passwords, which are the only values this program takes that must not
	// end up in a screenshot or a scrollback buffer.
	fieldSecret
	// fieldChoice is a closed set of values, moved through with the arrow
	// keys. A role is one of two things, and a text field that accepts
	// "admni" and stores a plain user is a worse way to say so.
	fieldChoice
)

type field struct {
	label   string
	hint    string
	kind    fieldKind
	input   textinput.Model
	choices []string
	choice  int
}

func textField(label, value, placeholder, hint string) field {
	input := textinput.New()
	input.Prompt = ""
	input.Placeholder = placeholder
	input.CharLimit = 128
	input.SetValue(value)
	return field{label: label, hint: hint, kind: fieldText, input: input}
}

// noticeField is a text field the size of a notice rather than of a name.
//
// The limit is in characters, which is what a text input counts, and the
// server's is in bytes; the form's submit handler checks the second, so a
// message of five hundred accented characters is refused there with a
// sentence rather than by the server with none.
func noticeField(label, hint string) field {
	created := textField(label, "", "", hint)
	created.input.CharLimit = 500
	return created
}

func secretField(label, hint string) field {
	input := textinput.New()
	input.Prompt = ""
	input.EchoMode = textinput.EchoPassword
	input.EchoCharacter = '•'
	input.CharLimit = 128
	return field{label: label, hint: hint, kind: fieldSecret, input: input}
}

func choiceField(label string, choices []string, selected int, hint string) field {
	return field{label: label, hint: hint, kind: fieldChoice, choices: choices, choice: selected}
}

// form is a stack of fields with one of them focused: the create, edit and
// password screens are all one of these.
type form struct {
	title       string
	intro       string
	submitLabel string
	fields      []field
	focus       int
	// failure is what the caller writes back when a submitted form is not
	// acceptable, shown above the buttons rather than on the status line, so
	// that the reason sits next to the fields it is about.
	failure string
	width   int
}

func newForm(title, intro, submitLabel string, fields ...field) *form {
	created := &form{title: title, intro: intro, submitLabel: submitLabel, fields: fields}
	created.focusField(0)
	return created
}

func (f *form) focusField(index int) {
	for i := range f.fields {
		f.fields[i].input.Blur()
	}
	f.focus = index
	if f.fields[index].kind != fieldChoice {
		f.fields[index].input.Focus()
	}
}

func (f *form) move(delta int) {
	next := f.focus + delta
	switch {
	case next < 0:
		next = len(f.fields) - 1
	case next >= len(f.fields):
		next = 0
	}
	f.focusField(next)
}

// Update handles one message and says whether the form is finished.
//
// Enter submits from any field rather than only from the last one, because a
// form of four fields where three are already right is most of what an
// administrator edits.
func (f *form) Update(message tea.Msg) (formResult, tea.Cmd) {
	key, isKey := message.(tea.KeyMsg)
	if !isKey {
		var command tea.Cmd
		f.fields[f.focus].input, command = f.fields[f.focus].input.Update(message)
		return formPending, command
	}

	switch key.String() {
	case "esc":
		return formCancelled, nil
	case "enter":
		return formSubmitted, nil
	case "tab", "down":
		f.move(1)
		return formPending, nil
	case "shift+tab", "up":
		f.move(-1)
		return formPending, nil
	}

	current := &f.fields[f.focus]
	if current.kind == fieldChoice {
		switch key.String() {
		case "left", "h":
			current.choice = (current.choice - 1 + len(current.choices)) % len(current.choices)
		case "right", "l", " ":
			current.choice = (current.choice + 1) % len(current.choices)
		}
		return formPending, nil
	}

	// A field that is being typed into owns its keys: the single letter
	// shortcuts of the list belong to the list.
	var command tea.Cmd
	current.input, command = current.input.Update(message)
	return formPending, command
}

func (f *form) value(index int) string {
	if f.fields[index].kind == fieldChoice {
		return f.fields[index].choices[f.fields[index].choice]
	}
	return strings.TrimSpace(f.fields[index].input.Value())
}

// secret is value without the trimming: a password is whatever was typed,
// spaces included, because that is what the person will type again to log in.
func (f *form) secret(index int) string {
	return f.fields[index].input.Value()
}

// View draws the card: a title, one line per field, and the hint of whichever
// field is focused.
//
// One line per field and not three. A stack of boxed inputs is what a form
// looks like in a browser, and in a terminal it costs twenty lines for six
// answers, which on an eighty by twenty four window means the card no longer
// fits the screen it is centred in.
func (f *form) View() string {
	content := cardContent(f.width)

	labelWidth := 0
	for _, current := range f.fields {
		labelWidth = max(labelWidth, lipgloss.Width(current.label))
	}
	labelWidth += 2

	var body strings.Builder
	body.WriteString(cardTitleStyle.Render(f.title))
	body.WriteString("\n")
	if f.intro != "" {
		body.WriteString(metaStyle.Width(content).Render(f.intro))
		body.WriteString("\n\n")
	}

	for i := range f.fields {
		body.WriteString(f.renderField(i, content, labelWidth))
		body.WriteString("\n")
	}

	if hint := f.fields[f.focus].hint; hint != "" {
		body.WriteString("\n")
		body.WriteString(metaStyle.Width(content).Render(hint))
		body.WriteString("\n")
	}
	if f.failure != "" {
		body.WriteString("\n")
		body.WriteString(dangerStyle.Width(content).Render(f.failure))
		body.WriteString("\n")
	}

	body.WriteString(cardFooterStyle.Render(
		helpLine(content, keyHint("enter", f.submitLabel), keyHint("tab", "next field"),
			keyHint("esc", "cancel"))))

	return cardStyle.Width(cardWidth(f.width)).Render(body.String())
}

func (f *form) renderField(index, content, labelWidth int) string {
	current := f.fields[index]
	focused := index == f.focus

	marker := "  "
	labelStyle := cardLabelStyle
	if focused {
		marker = helpKeyStyle.Render("❯ ")
		labelStyle = lipgloss.NewStyle().Bold(true).Foreground(accent)
	}
	label := labelStyle.Width(labelWidth).Render(current.label)

	valueWidth := max(content-labelWidth-2, 8)
	if current.kind == fieldChoice {
		return marker + label + renderChoices(current, focused)
	}

	// One column short of the room left, so that the block the cursor draws
	// after the last character has somewhere to go.
	current.input.Width = valueWidth - 1
	return marker + label + current.input.View()
}

func renderChoices(current field, focused bool) string {
	var parts []string
	for i, choice := range current.choices {
		// The mark, and not only the colour: this is read on terminals that
		// have no colour at all, and a selected value that is merely a
		// different shade of the same word is not selected as far as they are
		// concerned.
		style := lipgloss.NewStyle().PaddingRight(2).Foreground(muted)
		mark := "○ "
		if i == current.choice {
			mark = "● "
			style = lipgloss.NewStyle().PaddingRight(2).Bold(true).Foreground(accent)
		}
		parts = append(parts, style.Render(mark+choice))
	}
	rendered := lipgloss.JoinHorizontal(lipgloss.Left, parts...)
	if focused {
		return rendered + metaStyle.Render("  ←/→")
	}
	return rendered
}
