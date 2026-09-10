# Changelog

Every version PartyShare has published, newest first. A version is one merge to
master: `.github/workflows/tag.yml` writes the entry from the pull request title
when it bumps the version, so this list and the GitHub releases say the same
thing. The Windows installer shows it on its "What's new" page, which is why the
entries are in English: that page is where they are read, and everything else on
it is.

A version raised by hand in `CMakeLists.txt` skips that step, so whoever raises
it writes the entry here in the same commit.

## 0.1.66 (2026-09-10)

- A viewer who leaves or takes over the screen share no longer holds its bitrate down

## 0.1.65 (2026-09-10)

- A second login or a room switch no longer leaves a ghost holding the screen share

## 0.1.64 (2026-09-09)

- Full screen for the shared screen, room size from the admin panel, and a nudge

## 0.1.63 (2026-09-08)

- A project page for GitHub Pages, with the program running

## 0.1.62 (2026-09-08)

- The share button chooses between the entire screen and one window

## 0.1.61 (2026-09-07)

- The admin panel lines up its top row and gains a Sessions tab

## 0.1.60 (2026-09-07)

- Voice gate, admin console, emoji picker, process metrics and a faster Windows CI

## 0.1.59 (2026-09-07)

- The macOS DMG survives a busy hdiutil

## 0.1.58 (2026-09-06)

- The login screen asks for the server address and tests the connection

## 0.1.57 (2026-09-06)

- Settings is back on the login screen, and stays on the room-choosing screen

## 0.1.56 (2026-09-06)

- The Windows installer gains the MIT licence and a What's new page

## 0.1.55 (2026-09-06)

- Settings moves from the login screen to the room-choosing screen

## 0.1.54 (2026-09-04)

- The ended session is announced instead of leaving the client in the room with "wrong password"

## 0.1.53 (2026-09-03)

- Admin joins the room screen, and the status bar stops cutting words in half

## 0.1.52 (2026-09-03)

- Steps 6 to 12 of the audio plan, in one branch

## 0.1.51 (2026-09-03)

- The noise suppression level becomes a choice

## 0.1.50 (2026-09-03)

- The gain control is now the second-generation one

## 0.1.49 (2026-09-03)

- Lost audio is asked for again, on both legs

## 0.1.48 (2026-09-03)

- Audio now travels with redundancy

## 0.1.47 (2026-09-03)

- The macOS .dmg now ships with audio and video

## 0.1.46 (2026-09-03)

- The screen stops freezing when whoever is sharing leaves the room

## 0.1.45 (2026-09-03)

- dbadmin now warns and ends the session of whoever is connected

## 0.1.44 (2026-09-02)

- Leaving the room resets mute, and the share picks a chosen monitor

## 0.1.43 (2026-09-02)

- The footer says when a new version has been published

## 0.1.42 (2026-09-02)

- A Linux server installer, and the server tarball in the release

## 0.1.41 (2026-09-02)

- Room capacity, status icons and an offline server at sign-in

## 0.1.40 (2026-09-02)

- The first person in a room has a microphone on Windows

## 0.1.39 (2026-09-02)

- Direct message from the administrator with an OK button, and presence with IP in dbadmin

## 0.1.38 (2026-09-02)

- What the end-to-end audit on macOS found: five fixes

## 0.1.37 (2026-08-28)

- The invisible focus, the crash on exit and the name that turned into an id

## 0.1.36 (2026-08-27)

- The logs now say who, by name, and which room

## 0.1.35 (2026-08-27)

- The release chapter now says what the tag really produces
- The SFU test gives up when the connection dies, instead of waiting ten seconds

## 0.1.34 (2026-08-27)

- The documentation becomes a book of fifteen chapters

## 0.1.33 (2026-08-27)

- The footer now says which build is running

## 0.1.32 (2026-08-26)

- Rooms get names, and one that is not given one keeps its own code

## 0.1.31 (2026-08-26)

- An ordinary user's password, changed by that user

## 0.1.30 (2026-08-26)

- A restriction written outside the server now applies to a session that is already open

## 0.1.29 (2026-08-26)

- Clickable links in the chat, and an alert when somebody joins or leaves the room

## 0.1.28 (2026-08-26)

- The screen that changes hands, and the microphone that came back muted after a reconnection

## 0.1.27 (2026-08-26)

- The screen sound gets a volume, and noise suppression gets a checkbox

## 0.1.26 (2026-08-25)

- dbadmin gets a rooms screen, and a Go change stops running the whole CI

## 0.1.25 (2026-08-25)

- Seeing the rooms is not administration, and an ordinary user is entitled to one

## 0.1.24 (2026-08-25)

- Every room now goes to the database and outlives whoever was in it

## 0.1.23 (2026-08-25)

- The SFU connection state only existed in debug, which release does not compile

## 0.1.22 (2026-08-25)

- The client's IP in the server's connection logs

## 0.1.21 (2026-08-25)

- Switching servers without closing the program, and the network status back on the main screen

## 0.1.20 (2026-08-24)

- Do not print the database password when the connection fails

## 0.1.19 (2026-08-24)

- gofmt flagged all seventeen dbadmin files, and none of them was wrong

## 0.1.18 (2026-08-24)

- Hearing the shared screen's audio

## 0.1.17 (2026-08-24)

- The bitrate ceiling did not follow the resolution, and 1080p60 went out at half

## 0.1.16 (2026-08-24)

- Validation on Linux and macOS, and the five warnings clang-tidy found

## 0.1.15 (2026-08-24)

- Save in Settings, metrics as charts, and smoother rendering

## 0.1.14 (2026-08-24)

- Two defects found testing release v0.1.13

## 0.1.13 (2026-08-24)

- Screen share quality and hardware encoding on Windows

## 0.1.12 (2026-08-23)

- The disabled volume slider painted the whole slab

## 0.1.11 (2026-08-23)

- The dbadmin test waited for the sentence and looked at the screen behind it

## 0.1.10 (2026-08-23)

- An interface with corners, and a config.ini that is born ready and keeps what you choose

## 0.1.9 (2026-08-23)

- Room moderation: what an administrator takes from an account, and not only from a visit

## 0.1.8 (2026-08-22)

- Room chat, with the conversation living exactly as long as the room

## 0.1.7 (2026-08-22)

- The glob that only matched on the machine of whoever wrote it

## 0.1.6 (2026-08-22)

- The call negotiated everything and carried no packet: two copies of libsrtp in one binary

## 0.1.5 (2026-08-22)

- macOS: make the media layer link, and stop publishing a broken bundle

## 0.1.4 (2026-08-21)

- The Windows MSI never had the media layer inside, and there was no way to know

## 0.1.3 (2026-08-21)

- The track existed and the test sent audio into it; existing and being open are different moments

## 0.1.2 (2026-08-21)

- A shortcut carries no argument, and that was the only way to say where the server was

## 0.1.1 (2026-08-21)

- Accounts and the audit log without depending on a running server
- The first Windows build, and the two defects it found
- The signature covered two files, and seven of the missing ones are why it was blocked
- The MSI installs: fix for the report in #5
- The libwebrtc spike runs on Windows, and the libc++ conflict does not exist there
- The media layer runs on Windows, on a build from source
- A short path to a running server and two clients in a room
- The echo canceller was not off, and the patch did not fail the way I said
- The table said which flag turns the client off, but not how to bring up the server alone
- The SFU asked for an ephemeral port per participant, and the firewall paid for it
- Raising the version was something somebody had to remember, and now the merge remembers

## 0.1.0 (2026-08-20)

- First version published by tag; what came before is in the git history.
