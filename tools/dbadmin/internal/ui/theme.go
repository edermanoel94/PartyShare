package ui

import (
	"github.com/charmbracelet/lipgloss"
)

// The palette. Adaptive rather than fixed, because a terminal tool is read on
// a light background about as often as on a dark one, and a colour chosen for
// one of them is unreadable on the other.
var (
	accent  = lipgloss.AdaptiveColor{Light: "#0B6E63", Dark: "#2DD4BF"}
	admin   = lipgloss.AdaptiveColor{Light: "#A15C07", Dark: "#F0B429"}
	muted   = lipgloss.AdaptiveColor{Light: "#5B6570", Dark: "#8B949E"}
	subtle  = lipgloss.AdaptiveColor{Light: "#D3D9E0", Dark: "#30363D"}
	danger  = lipgloss.AdaptiveColor{Light: "#B42318", Dark: "#FF7B72"}
	warning = lipgloss.AdaptiveColor{Light: "#A15C07", Dark: "#F0B429"}
	success = lipgloss.AdaptiveColor{Light: "#116B33", Dark: "#56D364"}
	ground  = lipgloss.AdaptiveColor{Light: "#E9F5F3", Dark: "#123B38"}
)

var (
	titleStyle = lipgloss.NewStyle().Bold(true).Foreground(accent)
	metaStyle  = lipgloss.NewStyle().Foreground(muted)

	tabActiveStyle = lipgloss.NewStyle().Bold(true).Foreground(accent).
			Padding(0, 2).Border(lipgloss.Border{Bottom: "━"}, false, false, true, false).
			BorderForeground(accent)
	tabInactiveStyle = lipgloss.NewStyle().Foreground(muted).
				Padding(0, 2).Border(lipgloss.Border{Bottom: "─"}, false, false, true, false).
				BorderForeground(subtle)
	tabGapStyle = lipgloss.NewStyle().
			Border(lipgloss.Border{Bottom: "─"}, false, false, true, false).
			BorderForeground(subtle)

	successStyle = lipgloss.NewStyle().Foreground(success)
	warningStyle = lipgloss.NewStyle().Foreground(warning)
	dangerStyle  = lipgloss.NewStyle().Foreground(danger)
	helpStyle    = lipgloss.NewStyle().Foreground(muted)
	helpKeyStyle = lipgloss.NewStyle().Foreground(accent)

	adminBadgeStyle = lipgloss.NewStyle().Foreground(admin)
	emptyStyle      = lipgloss.NewStyle().Foreground(muted).Padding(1, 2)

	cardStyle = lipgloss.NewStyle().
			Border(lipgloss.RoundedBorder()).
			BorderForeground(accent).
			Padding(1, 3)
	cardTitleStyle  = lipgloss.NewStyle().Bold(true).Foreground(accent).MarginBottom(1)
	cardLabelStyle  = lipgloss.NewStyle().Foreground(muted)
	cardValueStyle  = lipgloss.NewStyle()
	cardFooterStyle = lipgloss.NewStyle().Foreground(muted).MarginTop(1)
)

// dangerCard is the confirmation box. Its own border colour because the one
// screen that destroys something should not look like the six that do not.
func dangerCard() lipgloss.Style {
	return cardStyle.BorderForeground(danger)
}

// tableHeaderLines is how many lines a bubbles table draws above its rows: the
// column titles and the rule under them. Its height setting counts the rows
// alone, so anything laying a table out has to add these back.
const tableHeaderLines = 2

// keyHint renders one entry of the help line, "n new", with the key picked out
// so the line can be scanned rather than read.
func keyHint(key, label string) string {
	return helpKeyStyle.Render(key) + " " + helpStyle.Render(label)
}

// helpLine joins as many hints as fit the width, with a separator quiet enough
// to disappear.
//
// Dropped rather than truncated, and dropped from the end, because the hints
// are written most useful first: a narrow window loses "q quit", which every
// terminal program shares, and keeps "n new", which only this one knows.
func helpLine(width int, hints ...string) string {
	separator := helpStyle.Render(" · ")

	line := ""
	for i, hint := range hints {
		next := hint
		if i > 0 {
			next = separator + hint
		}
		if width > 0 && lipgloss.Width(line+next) > width {
			break
		}
		line += next
	}
	return line
}

// clamp cuts a line to the width, counting what is drawn rather than what is
// stored: a style is escape sequences around the text, and they take no
// columns.
func clamp(line string, width int) string {
	if width <= 0 {
		return line
	}
	return lipgloss.NewStyle().MaxWidth(width).Render(line)
}

// truncate shortens a value to fit a column, marking that it did.
func truncate(value string, width int) string {
	if width <= 0 {
		return ""
	}
	runes := []rune(value)
	if len(runes) <= width {
		return value
	}
	if width == 1 {
		return "…"
	}
	return string(runes[:width-1]) + "…"
}
