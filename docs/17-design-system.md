# 17. Design system

How the client looks, and how to add to it without it starting to look like two
programs. Everything visual comes from one of three places, and the order is
the rule:

1. **Tokens** in `client/src/ui/theme.hpp`: every colour, distance, corner,
   type size and duration the interface may use.
2. **The stylesheet** in `client/src/ui/theme.cpp`: what the standard Qt widgets
   look like, written with those tokens.
3. **Components** in `client/src/ui/components/`: the widgets a stylesheet
   cannot express, drawn by hand from the same tokens.

A colour written where it is drawn, a margin that "looked right", a font built
beside a label: each of those is a fourth place, and a fourth place is how the
login error ended up a literal red that vanished on a dark window.

## 1. The shape of the window

```
QMainWindow
├─ shell                       QWidget, no margins
│  ├─ AppSidebar               the navigation rail; hidden on the sign-in page
│  └─ pages                    QStackedWidget
│     ├─ sign in               a Card in the middle of the page
│     ├─ rooms                 AppHeader, three StatCards, a grid of room cards
│     ├─ room                  AppHeader, the stage with a FloatingBar over it,
│     │                        and the side panel: faces, tabs, people or chat
│     └─ administration        AdminPanel
├─ ToastHost                   notifications, over the top of the pages
└─ status bar                  state, network verdict, call numbers, version
```

The rail has two places, not four: the list of rooms and the room are one,
because which of them is up is decided by whether there is a call and never by
a click. `MainWindow::go_to_page` is the one function every change of page
passes through, and it is what tells the rail where the window is.

The call controls float over the stage rather than taking a strip from it.
While somebody else's screen is on the stage they slide away after three
seconds without the pointer moving and come back on the first movement; with
nothing shared they stay. They are still there in full screen, for the same
price.

The side panel of the room is `QSplitter`'s second pane and cannot be collapsed
to nothing. Below 780 by 560 the window does not go.

## 2. Tokens

All in the `dv::ui::theme` namespace.

### Colour

`theme::colors()` returns the `Colors` in force. Each field is named for what it
is *for*, not for what it looks like:

| Token | For |
| --- | --- |
| `window`, `chrome` | Behind everything; the rail |
| `surface`, `surface_hover`, `surface_pressed` | Cards, fields, lists, and their states |
| `surface_raised` | What floats: a notification, the call controls |
| `border`, `border_strong` | A hairline; the border of something under the pointer |
| `text`, `muted` | Primary and secondary text |
| `accent`, `accent_hover`, `accent_pressed`, `on_accent` | The thing to press, and what is written on it |
| `accent_soft`, `accent_text` | A control that is *on*; the accent as text on a surface |
| `danger`, `warn`, `success`, `info` and their `_soft` | The four states, and the wash behind each |
| `canvas`, `canvas_text` | Behind a shared screen. Nearly black in both schemes |
| `shadow` | Under a raised surface. Carries its own alpha |

Two rules. A state is never colour alone: the toast has an icon, the full room
has the word "full", the speaking participant has a ring *and* a tooltip. And
`accent` is a background; text in the accent colour uses `accent_text`, which is
the shade that clears 4.5:1 on a surface.

In the stylesheet a token is `@{name}`, with the braces - `@accent` without them
would eat the front of `@accent_hover`.

### Space, corners, type, motion

| Token | Values |
| --- | --- |
| `theme::space::kXs` … `kXxl` | 4, 8, 12, 16, 24, 32 |
| `kChipRadius`, `kControlRadius`, `kCardRadius` | 8, 10, 14 |
| `theme::TextRole` | `Caption` 9 pt, `Body` 10, `Label` 10 semibold, `Title` 12, `Heading` 14.5, `Display` 20.5 |
| `theme::motion::kFastMs`, `kBaseMs`, `kSlowMs` | 120, 180, 260 |

The type sizes are a modular scale: each step 1.2 times the last, from a ten
point body. Give a label its role with `theme::set_role(label, role)` - it sets
a property the stylesheet reads, so a change of theme still reaches it - and use
`theme::font(role)` only where there is no label to set one on, in a paint
event.

Every animation takes its duration from `theme::motion::duration(ms)`, which
returns zero when `[ui] animations` is off. Zero means the last frame at once:
check for it and skip the animation rather than running one of no length.

## 3. Themes

`[ui] theme` in `config.ini` is `system`, `light` or `dark`. `system` follows
the desktop and keeps following it while the program runs; the other two hold.
Settings → Appearance changes it live and writes it down.

`theme::set_mode` installs the palette and the stylesheet again, re-polishes
every widget, sets each window's title bar (Windows only; see
`theme_windows.cpp`) and emits `theme::notifier()->changed()`.

**Most widgets need to do nothing.** A stylesheet rule is re-resolved, and a
`paintEvent` that reads `theme::colors()` reads the new ones. Connect to
`changed()` only if the widget *keeps* something made from a colour: a rendered
`QIcon` (`Button`), text with an inline colour (`StatCard`'s status line), a
"what is on screen" cache (`MainWindow`'s network indicator).

**To add a theme**: write another function beside `light()` and `dark()` in
`theme.cpp` that fills every field of `Colors`, add a value to `theme::Mode`
with its word in `parse_mode` and `to_string`, add the word to `is_theme` in
`shared/src/config/config.cpp` and to the box in `SettingsDialog`. Measure the
pairs that carry text - `text` and `muted` on `surface` and `window`,
`accent_text` on `accent_soft`, `on_accent` on `accent` - against 4.5:1 before
anything else.

## 4. Components

| Component | File | What it is |
| --- | --- | --- |
| `AppSidebar` | `app_sidebar` | The navigation rail. Knows places as numbers and asks; never switches a page itself |
| `AppHeader` | `app_header` | The bar on top of a page: title, a quieter line, `beside_title()` and `actions()` |
| `Card`, `StatCard` | `stat_card` | A surface with a hairline; one number, its label, and whether that is good |
| `DataTable` | `data_table` | A read-only listing with an empty state and a filter |
| `SearchInput` | `search_input` | A line edit with a magnifying glass and a clear button |
| `PrimaryButton`, `SecondaryButton`, `DangerButton`, `QuietButton` | `buttons` | `QPushButton`s that set the property the stylesheet styles, and can carry an icon |
| `IconButton` | `icon_button` | A painted button that is an icon, with or without a word: rail items, call controls |
| `AppDialog` | `app_dialog` | Title, sentence, `body()`, and buttons in the right order |
| `AppForm` | `app_dialog` | Labelled fields at the house spacing, with a hint and an error line |
| `EmptyState`, `LoadingState` | `empty_state` | What stands where content would be when there is none, or none yet |
| `Toast`, `ToastHost` | `toast` | A notification, and the stack of them over a container |
| `Avatar` | `avatar` | A person as a letter on a colour worked out from their name |
| `FloatingBar` | `floating_bar` | A raised bar that floats over something instead of sitting in its layout |
| `Icon`, `paint_icon` | `icons` | Every pictogram, painted - there are no icon files |

Using one is constructing it:

```cpp
auto* header = new AppHeader(page);
header->set_title(QStringLiteral("Rooms"));
auto* create = new PrimaryButton(QStringLiteral("New room"), page);
create->set_leading_icon(Icon::Plus);
header->actions()->addWidget(create);

toasts_->show_toast(ToastKind::Success, QStringLiteral("Room code copied"));
```

`MainWindow::build_home_page` and `build_room_page` are the worked examples.

### How long a notification stays

Decided in one function, `toast_lifetime_ms`, by what the notification is about
and by whether it is going over a picture somebody is watching:

| Kind | On a page | Over a shared screen |
| --- | --- | --- |
| `Info`, `Success`, `Warning` | 2.5 s and 55 ms a character | the same |
| `Error` | until dismissed | twice that reading time |
| `Moderation` | until dismissed | until dismissed |

An error waits because it is the answer to something the person just did - but
not over a film, where nobody reaches for the mouse and a notification that
waits sits on the picture for the rest of it. An administrator muting or
restricting somebody always waits: it changes what a person in the room can do,
and gone in four seconds it leaves a voice that stopped for no visible reason.
The one window where it does not wait is the administrator's own, where it is
raised as `Info`: there it is the answer to a click made a moment ago, not
news, and somebody working down a list would be dismissing one per person.

The component cannot know what it is over, so it is told: `MainWindow::notify`
is the one place notifications are raised from, and it passes whether somebody
else's screen is on the stage *and* the room is the page that is up. A click
dismisses a notification, and so does Esc, Space or Enter once Tab has reached
it.

A notification that waits outlives the page it was raised over, which gives
the window two things to do. `go_to_page` calls `ToastHost::bring_to_front`,
because the page that arrives is drawn over what was there; and
`return_to_login` calls `dismiss_all`, because what is left up belongs to the
session that ended and the next person to read it may not be who it was for.

### Writing a new one

1. **It may include `ui/theme.hpp` and other components, and nothing else of
   ours.** Not `app/`, not `network/`, not `media/`, not a model. A component is
   told what to show - a string, a number, a tone - and reports what was done to
   it with a signal. `client/CMakeLists.txt` says the same above the list.
2. Colours from `theme::colors()` *inside* `paintEvent`, never stored. Distances
   from `theme::space`. Durations through `theme::motion::duration`.
3. Anything pressable takes the keyboard focus, draws a focus ring, and has an
   accessible name: an icon is not a name.
4. No `QGraphicsEffect` on anything that lives over the stage. An effect renders
   its widget offscreen and, for a shadow, blurs it on every repaint, and the
   stage repaints thirty times a second during a share. `theme::paint_shadow`
   is the shadow; an opacity on the painter is the fade (`Toast`).
5. Every `enum` in a header is `: std::uint8_t` - `performance-enum-size` is an
   error in CI, and the local clang-tidy does not look at headers unless told to
   ([chapter 2](02-build.md)).
6. Add the two files to `qt_add_executable` in `client/CMakeLists.txt`, and a
   row to the table above.

### Icons

Painted by `paint_icon` from paths on a 24 unit grid, not loaded. An SVG needs
the `qsvg` image plugin, which the release job prunes: an icon that was a file
would be missing from the packaged build and nowhere else. To add one: a value
in `Icon`, a `case` in `build()` in `icons.cpp`, strokes only, round caps.

The chevrons and the tick the stylesheet uses are the exception, because a
stylesheet can only point at an image. They are PNGs at three densities, written
by `assets/ui/make_arrows.py`.

## 5. Styling by property

The stylesheet picks widgets by *dynamic property*, which is what lets a plain
Qt widget - made in code or dropped into a form in Qt Designer - be one of ours:

| Property | On | Means |
| --- | --- | --- |
| `accent` = true | `QPushButton` | The one thing to press |
| `danger` = true | `QPushButton` | Leaving, deleting |
| `quiet` = `accent` / `danger` | `QPushButton` | A line of text until hovered |
| `toggle` = `accent` / `warn` | checkable `QPushButton` | What it wears while on |
| `code` = true | `QPushButton` | A room code that copies itself |
| `role` = `caption` / `title` / `heading` / `display` | `QLabel` | The type scale |
| `hint` = true, `error` = true | `QLabel` | Secondary text; a refusal |
| `eyebrow` = true | `QLabel` | Small capitals over a section |
| `chip` = true / `accent` / `warn` | `QLabel` | A short fact beside a title |
| `card` = true | `QFrame` | A surface with a hairline |
| `plain` = true | `QGroupBox` | No title, so no room for one |
| `flat` = true | `QListWidget` | Lives on a card already |
| `search` = true | `QLineEdit` | Leaves room for the glass |

A property set after a widget has been shown changes nothing until the style is
polished again; `theme::set_role` does that for you, and
`style()->unpolish(w); style()->polish(w);` does it by hand.

When two rules of the same specificity disagree the later one wins, in silence.
A rule that styles a *state* of a variant - `:focus`, `:hover` - has to name the
variant's property in its selector, or the generic rule further down takes it.
The long comment over the focus rules in `theme.cpp` is the history of that.

## 6. A screen in Qt Designer

`client/src/ui/password_dialog.ui` is the pattern. The form holds the widgets
and their wording; the class beside it holds what they do.

1. Create `client/src/ui/<name>.ui` in Designer, as a `QDialog` or `QWidget`.
   Lay it out with layouts only - no fixed positions - and take the margins and
   the spacing from the `theme::space` table: 24 round a dialog, 12 between
   rows, 8 between buttons.
2. Style nothing in the form. Ask for looks by adding dynamic properties
   (the green plus in the property editor): `role` = `heading` on the title,
   `hint` on the sentence under it, `danger` on a button that deletes. The
   table in section 5 is the vocabulary.
3. For a component that is not a standard widget, drop its base class and
   *promote* it: a `QPushButton` promoted to `dv::ui::PrimaryButton` with header
   `ui/components/buttons.hpp` - the form's own confirm button is one - or a
   `QLineEdit` to `dv::ui::SearchInput`. Designer builds a promoted widget from
   its parent alone, so the four buttons and the search box each have a
   constructor that takes only that. A component that needs more to exist - an
   `IconButton` its icon, a `StatCard` its label - cannot be promoted: build
   that one in code and add it to a layout the form left a place for.
4. In the class's `.cpp`, `#include "ui_<name>.h"`, and in the constructor:

   ```cpp
   Ui::PasswordDialog form;
   form.setupUi(this);
   current_ = form.current;   // keep only what the class talks to
   ```

   AUTOUIC is already on (`qt_standard_project_setup`) and finds the form by
   that include. List the `.ui` in `qt_add_executable` so a change rebuilds.
5. Connect in code, not in the form, anything more than "this button closes
   the dialog". A connection in a `.ui` is by name, and a renamed slot is found
   at run time.

Set tab stops in the form (Edit → Edit Tab Order) to reading order, and a buddy
on each label so its accelerator reaches its field.

## 7. Names

- Files are `snake_case`, one component per pair, under `ui/components/`; a
  platform half is `_windows.cpp` beside a `_stub.cpp`.
- Classes are `PascalCase` in `dv::ui`; tokens are in `dv::ui::theme`.
- Members end in `_`; functions and locals are `snake_case`; constants are
  `kPascalCase`. Qt's own virtuals keep Qt's names.
- A stylesheet property is one lowercase word, and a token is a
  `snake_case` role: `surface_raised`, never `grey_800`.
- A signal says what happened (`destination_chosen`, `dismissed`), not what
  should be done about it.

## 8. Where the interface meets the rest

The interface drives `client::app::CallSession` and touches nothing under it.
The session reports from networking and media threads, and every report becomes
a queued invocation of a `MainWindow` slot before a widget is touched - Qt
widgets belong to one thread. Those slots take `QString` and `QStringList`
rather than models, because a queued connection needs a registered metatype and
those need none.

So the direction of knowledge is one way:

```
CallSession  →  MainWindow slots  →  components
 (no Qt)        (knows both)         (know only the theme)
```

A component never calls the session; a page hands it text and listens for a
signal. `MainWindow::refresh_controls` is where the session's state becomes
which buttons are enabled, and it is the only place that happens.

Three things to keep when changing a page:

- The two checkable controls are connected to `toggled`, not `clicked`, so a
  screen reader's press action works; `refresh_controls` blocks their signals
  while it reports state back into them.
- A destructive or session-ending action is confirmed or is one deliberate
  click away from the call: signing out and changing the password are disabled
  inside a room.
- What a server refuses is said where it can be acted on: on the sign-in card
  while that is up, inside the New room dialog while that is, and as a
  notification otherwise.

## 9. Checking a change

**The tests first.** `tests/ui` builds `dv_ui_tests`, which makes the widgets
under an offscreen QApplication - no window reaches a screen - and checks what
a page relies on without looking: that no `@{token}` in the stylesheet is left
unfilled, that the text pairs clear their contrast in both themes, the
notification table of section 4, that the rail asks rather than switches, that
every icon is drawn, that nothing a participant types becomes markup in the
chat.

```
ctest --test-dir build/<preset> -L ui --output-on-failure
```

They link `dv_client_widgets`, the library the theme, the components and the
two self-drawing views are built as. A component added to
`client/CMakeLists.txt` goes in that library, and gets its tests in
`tests/ui/test_components.cpp`. What they do not reach is `MainWindow`, which
needs a session: the pages are checked by hand, as follows.

**Then by eye.** Run two clients against a local server, and look at it in both
themes.
The pages to visit are the four of section 1 plus Settings and one dialog; the
states worth a look are an empty lobby, a full room, somebody sharing, and a
refusal. Then, at 125 % and 150 % scale, that nothing is clipped: the icons are
painted and scale for free, the three PNGs do not.

`clang-format` and `clang-tidy` as [chapter 2](02-build.md) describes, with
`--header-filter` naming every header touched.
