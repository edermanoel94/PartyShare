# 3. Configuration

Every knob both programs read, where it can be written, and which copy wins.

## Precedence

Lowest first:

1. Built-in defaults
2. The machine's `config.ini`, beside the executable
3. This user's `config.ini`
4. Environment variables prefixed with `DV_`
5. The command line

`--config=PATH` takes either `.ini` or `.json` by extension and **replaces** both
discovered files rather than joining them.

The client prints which of those files it read and which it did not find, on
every startup, at `info`. That log line is the answer to "I put the address in
and it still connects to localhost", which is almost always a file written one
directory away from the one being read.

## The two `config.ini` files

A flag is fine for one run and useless for a machine somebody else uses: an
installed client is started from a shortcut, and a shortcut carries no arguments.
So the client looks for a `config.ini` on its own, in two places, and neither has
to exist.

| Order | Windows | Linux | macOS |
| --- | --- | --- | --- |
| 1. The machine's | Beside `partyshare.exe` | Beside the binary | Inside the `.app` |
| 2. This user's | `%LOCALAPPDATA%\partyshare\` | `$XDG_CONFIG_HOME/partyshare/` | `~/Library/Application Support/partyshare/` |

The second wins over the first, and that split is the point: whoever installs a
machine writes the first one and it answers for every account on it, and a person
overrides it in the second without being asked for an administrator password.

**Edit the second one where there is a choice.** The first belongs to the
installer and is replaced on the next upgrade, which would take the address of
the server with it; the second is never touched by an installer, and on macOS the
first lives inside the signed `.app`, where editing it breaks the signature.

Neither has to be created by hand. The installer drops a fully commented
`config.ini` beside the executable, and the client writes a second copy into this
user's directory the first time it runs. Both are inert as they ship — every line
is commented out — so uncommenting one line is the whole edit:

```ini
[network]
signaling_url = ws://192.168.1.10:8080
```

Sections and keys are the same names the JSON form uses, so nothing has to be
learned twice. Comments start with `;` or `#`. A key the client does not know is
a startup error naming the line, rather than a line that quietly does nothing:

```text
configuration error [invalid_ini]: line 2: no such setting as [network] signalling_url
```

## The settings dialog writes here too

The client saves what you pick in **Settings** into this user's `config.ini`: the
microphone, the output device, the screen resolution and frame rate, both ends of
the bitrate range, the share sound mode and its volume, noise suppression, the
room chime, and whether it checks for new versions. The monitor is the one thing
on that screen that is not saved, because it is which screen to share next
rather than a setting. With more than one monitor the **Share screen** button
asks the same question in a menu each time a share starts, and the box in
Settings follows whatever was last chosen either way. Left alone, the primary
monitor is shared - one screen, never every screen stitched side by side.

**Check GitHub for new versions** sits in the Connection group rather than beside
the room chime, though both are `[ui]` settings. The question it answers is not
"do I want to be told" but "may this program talk to the internet on its own",
which is the same question the server address above it asks. Unticking it stops
the check immediately — an answer already on its way is dropped rather than
shown — and ticking it schedules one a few seconds out, so it is not a switch
whose effect cannot be seen for six hours.

**Resolution and frame rate** are `video.width`, `video.height` and `video.fps`,
and the dialog offers 720p and 1080p at 30 or 60 fps. Both take effect at once,
including mid-call: a running share restarts on the same monitor, which costs a
stutter and no renegotiation. 30 fps is right for a document or an editor; 60 is
for what 30 makes unwatchable — scrolling, a terminal redrawing, anything
animated.

The resolution is a ceiling and not the size sent. A monitor is fitted inside it
with its shape kept, so 1080p on a 3440x1440 ultrawide sends 1920x802 and never a
stretched 1920x1080, and a monitor smaller than the box is sent untouched rather
than upscaled.

Raising either asks more of the encoder, and 1080p at 60 asks a lot: on an NVIDIA
card it is encoded by the card and costs almost no processor, and without one it
is encoded in software and costs a great deal. Which one is happening is in the
log, and [chapter 2](02-build.md) explains how to read it.

The dialog says something when the maximum bitrate is lower than what the chosen
size is worth — 1080p at 60 is worth around four times what 720p at 30 is. It
*says* it rather than doing it: a ceiling you set to fit your link is not one the
client should raise behind your back. The configuration is free to name a size or
a rate the dialog does not offer, `width = 2560` is perfectly valid, and the
dialog shows that as a row of its own instead of quietly rounding you down.

The minimum the dialog offers stops at `video.floor_bitrate_kbps`, 300 kbps by
default. The floor is how far congestion control may squeeze the picture when the
link cannot carry the minimum, and a configuration whose floor sits above its
minimum is one the client refuses to start on — so the dialog will not let you
save one. Lower both together if you need to go under 300.

## The settings

Every key below is `[section] key` in the INI form, `section.key` in the JSON
form, and `DV_SECTION_KEY` in the environment.

### `[network]`

| Key | Default | |
| --- | --- | --- |
| `signaling_url` | `ws://127.0.0.1:8080` | The server. The one line most installations change |
| `stun_servers` | `stun:stun.l.google.com:19302` | Comma separated. May be empty on a closed LAN |
| `turn_url`, `turn_username`, `turn_password` | — | Only when NAT defeats STUN |
| `ice_port_range_begin`, `ice_port_range_end` | 0, 0 | The UDP range the SFU binds media in. [Chapter 4](04-server-and-database.md) |
| `reconnect_initial_delay_ms` | 500 | Client reconnection backoff, doubling to the ceiling below |
| `reconnect_max_delay_ms` | 30000 | |

### `[video]`

| Key | Default | |
| --- | --- | --- |
| `width`, `height` | 1280, 720 | A ceiling the monitor is fitted inside, not the size sent |
| `fps` | 30 | 1 to 120 |
| `auto_bitrate` | false | Works the range out from the size and rate above. Off by default: a file that already names a bitrate names it for a reason |
| `min_bitrate_kbps` | 1500 | Where the encoder starts and what it aims for on a healthy link |
| `max_bitrate_kbps` | 3000 | The ceiling |
| `floor_bitrate_kbps` | 300 | The lowest congestion control may squeeze to. Must not sit above the minimum |
| `codec` | `H264` | A string so VP9 and AV1 need no schema change |

### `[audio]`

| Key | Default | |
| --- | --- | --- |
| `input_device`, `output_device` | empty | Empty means the system default. The name has to be the one the system shows |
| `sample_rate_hz`, `channels` | 48000, 1 | |
| `bitrate_kbps` | 96 | The ceiling the SFU's offer puts on Opus. 96 rather than 48 because a screen share is stereo and carries music |
| `frame_duration_ms` | 20 | |
| `noise_suppression` | true | Tuned for one voice in a room and treats everything else as the room. Turn it off for an instrument or a record playing behind you |
| `echo_cancellation` | true | Turning it off only makes sense with headphones, and not always |
| `automatic_gain_control` | true | Off leaves your voice the size the microphone caught it, which is better for an instrument and worse for conversation |

Only `noise_suppression` appears in the settings dialog, and it applies at once,
mid-call included. The other two are this file only.

### `[screen_audio]`

What a screen share carries besides the picture. Client side only — the sound
rides inside the sharer's own audio track, and the server never sees it as a
separate thing. [Chapter 9](09-screen-audio.md) is the whole subject.

| Key | Default | |
| --- | --- | --- |
| `mode` | `system` | `none`, `system` (everything the machine plays except this client) or `process` (one application, chosen in Settings) |
| `volume_percent` | 100 | 0 to 200. Only the screen side moves; your voice keeps its own gain control |

Needs Windows 10 build 20348 or newer. On Linux and macOS the capture does not
exist yet and the option does not appear. A mode this build does not recognise
reads as `none`: falling back to capturing the machine because a word was not
understood is the one unacceptable answer.

### `[ui]`

| Key | Default | |
| --- | --- | --- |
| `room_sounds` | true | The chime when somebody joins or leaves. [Chapter 10](10-join-leave-alerts.md) |
| `check_for_updates` | true | Ask GitHub whether a newer release exists. The **Updates** box in Settings writes this. [Chapter 14](14-release.md#how-anybody-finds-out-there-is-a-new-version) |

`check_for_updates` is one HTTPS request to one address, five seconds after the
window opens and every six hours after that. It carries nothing but the version
already written in the status bar, downloads nothing and installs nothing: the
whole result is that line becoming `0.1.41 · 0.1.42 available`, with a link to
the release page. Off, no request is made at all — which is the right setting
for a machine with no route out, where every check is a timeout.

### `[logging]`

| Key | Default | |
| --- | --- | --- |
| `level` | `info` | `trace`, `debug`, `info`, `warn`, `error`, `fatal`, `off` |
| `file_path` | empty | A log file besides the console |
| `crash_directory` | empty | Empty means the platform's own state directory |
| `crash_reports` | true | Off writes no stack traces of the machine to disk at all |

### `[server]` and `[database]`

Read by the server only, and covered in [chapter 4](04-server-and-database.md).

| Key | Default | |
| --- | --- | --- |
| `server.bind_address` | `0.0.0.0` | |
| `server.port` | 8080 | |
| `server.max_participants_per_room` | 20 | The largest room anybody may create, 2 to 50. A room's own size is chosen when it is created, up to this |
| `server.heartbeat_interval_ms` | 5000 | Also how long a restriction written straight into the database takes to bite |
| `server.heartbeat_timeout_ms` | 15000 | |
| `server.users_file` | empty | Development account list, plain text passwords |
| `database.enabled` | false | |
| `database.uri` | `mongodb://127.0.0.1:27017` | |
| `database.name` | `partyshare` | |
| `database.timeout_ms` | 2000 | Deliberately short: the store is called with the server's lock held |

## Environment variables

The `DV_` prefixed form of the same settings. The complete list is in
`shared/src/config/config.cpp`, and it is:

```text
DV_CONFIG_FILE
DV_SIGNALING_URL          DV_TURN_URL          DV_TURN_USERNAME   DV_TURN_PASSWORD
DV_ICE_PORT_RANGE_BEGIN   DV_ICE_PORT_RANGE_END
DV_VIDEO_WIDTH            DV_VIDEO_HEIGHT      DV_VIDEO_FPS       DV_VIDEO_CODEC
DV_VIDEO_AUTO_BITRATE     DV_VIDEO_MIN_BITRATE_KBPS
DV_VIDEO_MAX_BITRATE_KBPS DV_VIDEO_FLOOR_BITRATE_KBPS
DV_AUDIO_INPUT_DEVICE     DV_AUDIO_OUTPUT_DEVICE
DV_AUDIO_SAMPLE_RATE_HZ   DV_AUDIO_CHANNELS    DV_AUDIO_BITRATE_KBPS
DV_AUDIO_FRAME_DURATION_MS
DV_AUDIO_ECHO_CANCELLATION DV_AUDIO_NOISE_SUPPRESSION
DV_AUDIO_AUTOMATIC_GAIN_CONTROL
DV_SERVER_BIND_ADDRESS    DV_SERVER_PORT       DV_SERVER_MAX_PARTICIPANTS
DV_SERVER_USERS_FILE
DV_DATABASE_ENABLED       DV_DATABASE_URI      DV_DATABASE_NAME
DV_DATABASE_TIMEOUT_MS
DV_LOG_LEVEL              DV_LOG_FILE
DV_CRASH_DIRECTORY        DV_CRASH_REPORTS
```

The environment is the right place for a database URI carrying a password, which
is otherwise visible in `ps` for as long as the command runs:

```sh
export DV_DATABASE_ENABLED=1
export DV_DATABASE_URI="mongodb://ana:password@127.0.0.1:27017/?authSource=admin"
export DV_DATABASE_NAME=partyshare
```

Writing the configuration back out replaces the credentials in the URI with
asterisks, and omits `turn_password` entirely, so dumping it is not a way to read
either.

## Command line

Options take the `--key=value` form, with the value attached.

**The server refuses an option it does not recognise**, including a bare `--key`
with the value detached. A server is started by a script nobody is watching, and
a typo that is quietly ignored leaves it listening on a default nobody chose. The
client cannot do the same, because Qt reads its own options from that command
line, so it passes anything it does not recognise on to Qt.

### `partyshare-server --help`

| Option | Default | |
| --- | --- | --- |
| `--config=PATH` | — | Configuration file, read before anything else |
| `--bind-address=ADDRESS` | 0.0.0.0 | |
| `--port=PORT` | 8080 | 1 to 65535 |
| `--max-participants=N` | 20 | Largest room anybody may create, 2 to 50 |
| `--ice-port-range=A-B` | — | UDP range the SFU binds media in, as `50000-50100` |
| `--users-file=PATH` | — | Development account list |
| `--database-uri=URI` | — | MongoDB. **Giving one turns the database on** |
| `--database-name=NAME` | partyshare | |
| `--database-enabled=BOOL` | — | Turn the database off again, or on without a URI |
| `--create-admin=USER:PASS` | — | Create or promote that administrator, then exit |
| `--log-level=LEVEL` | info | |
| `--log-file=PATH` | — | |

### `partyshare --help`

| Option | |
| --- | --- |
| `--config=PATH` | `.ini` or `.json`. Replaces both discovered files |
| `--signaling-url=URL` | The server to connect to |
| `--input-device=NAME`, `--output-device=NAME` | Microphone and speakers |
| `--codec=NAME` | Video codec for the screen share |
| `--fps=N` | Screen capture rate, 1 to 120 |
| `--log-level=LEVEL`, `--log-file=PATH` | |
