package ui

import (
	"sort"

	"github.com/charmbracelet/bubbles/table"
)

// Which value a column shows. The rows are built from the columns that
// survived the width, so a cell has to say what it is rather than trust a
// position that may not be there.
type columnKind int

const (
	columnUsername columnKind = iota
	columnDisplayName
	columnRole
	columnUserID
	columnCreated
	columnRestrictions

	columnTime
	columnActor
	columnAction
	columnTarget
	columnDetail

	columnRoomID
	columnRoomName
	columnRoomOwner
	columnRoomCreated

	columnSessionUser
	columnSessionIP
	columnSessionState
	columnSessionConnected
	columnSessionLastSeen
)

// columnSpec is one column before it knows how wide it will be.
type columnSpec struct {
	kind  columnKind
	title string
	// The narrowest the column is worth showing at.
	minimum int
	// Whether leftover width is shared with it. Names grow usefully; a date
	// and a role do not.
	flexible bool
	// Lower is kept first when the window cannot hold them all.
	priority int
}

// Every cell a table draws sits inside this much padding, one column on each
// side, and it counts against the width the same as a character does.
const cellPadding = 2

// fitColumns decides which columns fit the width and how wide each one is.
//
// Dropping a column rather than letting the table overflow. A table wider than
// its window is not a table with a scrollbar: lipgloss wraps the line, every
// row becomes two, and the screen turns into a wall nobody can read. What is
// dropped is decided here, by priority, so that a narrow window loses the
// creation date and keeps the username.
//
// The order of the result is the order given, which is the order they are
// read in, and is deliberately not the order they are dropped in.
func fitColumns(width int, specs []columnSpec) ([]table.Column, []columnKind) {
	byPriority := make([]columnSpec, len(specs))
	copy(byPriority, specs)
	sort.SliceStable(byPriority, func(i, j int) bool {
		return byPriority[i].priority < byPriority[j].priority
	})

	kept := map[columnKind]int{}
	used := 0
	for _, spec := range byPriority {
		cost := spec.minimum + cellPadding
		if used+cost > width && len(kept) > 0 {
			// Not a break: a wide column may not fit where a narrow one after
			// it does, and dropping the date is better than dropping the role
			// that would have fitted beside it.
			continue
		}
		kept[spec.kind] = spec.minimum
		used += cost
	}

	// Whatever is left over goes to the columns that grow usefully, one column
	// at a time so that two of them end up within one of each other.
	var growing []columnKind
	for _, spec := range specs {
		if _, in := kept[spec.kind]; in && spec.flexible {
			growing = append(growing, spec.kind)
		}
	}
	if len(growing) == 0 {
		// A window so narrow that only fixed columns fitted. The last of them
		// takes the remainder anyway, because the alternative is a table whose
		// rule stops short of the screen for no reason a reader can see.
		for _, spec := range specs {
			if _, in := kept[spec.kind]; in {
				growing = []columnKind{spec.kind}
			}
		}
	}
	for i := 0; used < width && len(growing) > 0; i++ {
		kept[growing[i%len(growing)]]++
		used++
	}

	var columns []table.Column
	var kinds []columnKind
	for _, spec := range specs {
		if columnWidth, in := kept[spec.kind]; in {
			columns = append(columns, table.Column{Title: spec.title, Width: columnWidth})
			kinds = append(kinds, spec.kind)
		}
	}
	return columns, kinds
}
