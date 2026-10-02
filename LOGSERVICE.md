# The log service

How a knob in the field tells us what went wrong, when its owner allows it.

This is the design, not yet the code. It is written for two readers: whoever
builds it (firmware and server), and a knob's owner who wants to know what
switching it on means.

## Why

Most of what goes wrong on a knob shows in its log: a link that drops, a
radio refusing a command, the audio held up for a moment, a crash. Today
that log only exists on the knob's own network (TCP port 3333, the last
16 kB), so we see it only on knobs on our desk. The log service brings it
to us from anywhere, with the owner's permission:

- **radios we do not have**: the IC-7300MK2, the IC-905 and the IC-7760 were
  built from wfview's descriptions; one session on a real radio, with its
  log, tells us what differs;
- **problems we cannot reproduce**: a network with long gaps, a headset that
  behaves differently, a router that drops the knob;
- **crashes**: the core dump and the minutes before it.

## The owner's side

### Switching it on

On the configuration page, a **Diagnostics** section:

| Field | |
|---|---|
| **Email address** | Required. For our reply about a report, and nothing else. We send one email to confirm it; until it is confirmed, nothing is sent from the knob. |
| **Keep a diagnostic log and send it when something goes wrong** | The switch. Off by default; on only with a confirmed address. |
| **Hide numbers and names** | On by default: telephone numbers keep their last three digits, contact names their initials. |
| **Detailed radio log** | Off by default. Logs every command to the radio and every answer: for testing a radio we have not seen. Switches itself off after 24 hours. |
| **Send log now** | A button, with a box for what happened. For anything the knob cannot notice by itself: a sound that is wrong, a screen that misbehaves. |
| **Delete the log** | Empties the log on the SD card. |

The owner never handles a log file: there is no download and nothing to
upload. Most hams are not IT people. The knob sends what is needed itself,
and the owner's part is the switch, the address and, when something sounds
wrong, the **Send log now** button.
| **Forget me** | Switches the service off and asks the server to delete every report from this knob and the email address. |

The section also shows:
- the state: off, waiting for the address to be confirmed, or on;
- the last report sent, with its number;
- what is waiting to be sent;
- how much of the SD card the log uses.

While the service is on, the face shows a small mark beside the link state,
so a knob never sends anything without its owner being able to see that it
might.

### What is sent, and what never is

Sent, in a report:
- the knob's log lines for the minutes around the problem: what it connected
  to, call and over events, link states, the audio's buffers, errors;
- the firmware, its version, the radio model, why the knob last restarted;
- after a crash, the core dump;
- the owner's description, for **Send log now**.

Never sent:
- passwords, tokens or the Google client secret: the firmware never logs
  them in the first place;
- audio. Nothing the microphone or the speaker carries is recorded, ever.

**Hide numbers and names** (on by default) applies on the knob, before a
report leaves it. What stays is what can show in a log line:
- the radio's or the reflector's address;
- callsigns, talkgroups and frequencies;
- the SIP server's name;
- the router's address.

### How long it is kept

- **On the knob:** the SD card keeps up to 7 days, and at most 64 MB; the
  oldest file goes first.
- **On the server:** 30 days after a report arrives, then it is deleted.
- **The email address:** kept while the service is on; **Forget me**, or
  switching the service off, deletes it.

This is the data protection policy in short; the page links to the full
text.

## When the knob sends a report

Not on a timer: only when there is a reason, and only when the knob is idle,
with no call, no over and no download running. A report covers the window around
its reason: by default 10 minutes before and 2 minutes after, at most 512 kB
of log before compression.

### Automatic reasons

| Reason | Noticed |
|---|---|
| **A crash** | At the next start: the reset reason is a panic, an abort or a watchdog, or a core dump is stored. The report carries the dump. |
| **Audio held up** | The playback waited 100 ms or more in the jack's DMA or the headset's tap, or ran dry 3 times within a minute. |
| **A link that will not stay** | The radio, the reflector, the SIP account or the web SDR dropped 3 times within 10 minutes, or would not come up for 5 minutes after a start. |
| **A radio refusing commands** | More than 20 refused commands (NG) in a session. In the detailed radio log, the first one already. |
| **An update that failed** | A download or install that did not finish, or a rollback at start. |

Each reason has its own limits, so a knob in trouble does not send the same
thing all day:
- at most once an hour for the same reason;
- the same signature (reason and the error that triggered it) at most once
  a day;
- at most 10 reports a day in all.

### By hand

**Send log now** sends the last 15 minutes, or a chosen window, with the
owner's text. It is not counted against the limits.

## On the knob

### Writing the log to the SD card

```
ESP_LOG* -> netlog's vprintf hook -> the TCP ring (as today)
                                   \-> the log service's ring (PSRAM, 64 kB)
                                          -> "logwr" task -> SD card
```

- The hook only copies into the RAM ring and never waits. If the ring is
  full, because the card is slow or missing, lines are dropped and counted.
  The next written line says how many went.
- Each line gets the wall-clock time (SNTP) in front of the uptime the log
  already carries: `2026-10-02 08:13:44.512 I (12345) sip: ...`.
- The writer task, priority 1 on core 0, appends in blocks of 8 kB, at least
  every 5 seconds, and syncs the file every 30 seconds.
  - Its stack is in PSRAM: it only ever touches the card, never flash.
  - SD writes go through the SDMMC host's own DMA. They do not stop the flash
    cache, so they cannot hold up the audio as an NVS write did.
- **Files.** The card has no long file names (`CONFIG_FATFS_LFN_NONE`), so
  the files are `LOG/L0000001.TXT` and up, at most 1 MB each, numbered
  across restarts.
  - `LOG/INDEX.TXT` keeps, one line a file: its number, first and last time,
    the boot it belongs to, the firmware.
  - Old files are deleted to stay inside 7 days and 64 MB.
- **Without a card**, or with a full or broken one, the service says so on
  the page and keeps only its RAM ring (about 10 minutes). Reports still
  work, with less history.
- **Before a restart** the knob orders, the ring is flushed. A crash loses at
  most the last 5 seconds; the core dump covers the moment itself.

### Noticing a reason

Each reason in the table is a counter or a flag set where the problem is
already detected and logged today:
- the playback's stall note;
- the radio clients' link states and NG counts;
- the OTA errors;
- the reset reason at start.

The log service checks them once a second. When one trips, it records a
**pending report**:
- the reason;
- the signature;
- the window's start and end;
- for a crash, a pointer to the core dump.

It goes as a small file in `LOG/OUTBOX/`, so it survives a restart and a
knob that is offline for a day sends it when it is back.

### Sending a report

When the knob is idle and online, the oldest pending report goes:

1. **Stage it in PSRAM, never streaming the card into TLS.** The SDMMC DMA
   and TLS both need internal DMA RAM, and the knob has been short of it
   (see the SD card notes in the memory file).
   - Read the window's lines from the card.
   - Apply **Hide numbers and names**.
   - Compress them with the ROM's deflate (miniz, `tdefl`).
   - 512 kB of log comes to about 60-100 kB.
2. **Close the card, then open one HTTPS connection** to the server (the
   certificate bundle the updates already use) and POST the report.
3. **On success,** delete the outbox file and remember the report's number
   for the page.
4. **On failure,** retry later: 1 min, 5 min, 30 min, then hourly. A report
   older than 7 days is dropped.

A call or an over starting mid-upload does not wait for it. The upload keeps
going on core 0 at low priority; the audio's tasks are higher.

### Its API, on the knob

Behind the page's login, like the rest:

| Request | |
|---|---|
| `GET /api/logs` | the settings and state, as JSON |
| `POST /api/logs` | `email`, `on`, `hide`, `radio_detail` |
| `POST /api/logs/send` | `text`, and optionally `minutes` |
| `GET /api/logs/download?minutes=60` | bench builds only, for us at the desk: the log as text. Not on the page. |
| `POST /api/logs/delete` | empty the log on the card |
| `POST /api/logs/forget` | off, and ask the server to forget this knob |

## On the server

On vfoknob.com, beside the site and its other APIs, under
`https://vfoknob.com/api/logs/v1/...`: the knob's firmware only ever talks to
these four endpoints, so how they are built behind that address is the site's
business.

### Endpoints

| Request | |
|---|---|
| `POST register` | `{ "email", "knob": <random id>, "firmware" }`. Sends the confirmation email; answers a **token** for this knob, kept in its NVS. |
| `GET confirm?c=<code>` | the link in that email; confirms the address |
| `POST report` | `Authorization: Bearer <token>`. Multipart: `meta.json`, `log.gz`, optionally `core.bin`. Answers `{ "id": "R-4F7K" }`. Refused (403) until the address is confirmed; the knob keeps the report and tries again later. |
| `POST forget` | `Authorization: Bearer <token>`. Deletes the knob's reports, its address and its token. |

**`meta.json`** holds:
- the knob's random id (never its MAC address);
- the firmware and version, and the radio model;
- the reason and the signature;
- the window;
- the reset reason;
- the owner's text;
- whether names and numbers were hidden.

### What the server does with a report

1. Checks the token, the size (at most 2 MB in all) and the rate (at most 20
   reports a day per knob).
2. Stores the three parts under the knob and the date.
3. Emails us a notice: the reason, the firmware, the radio, and a link to
   the report.
4. Emails the owner a short receipt with the report's number, so that a
   reply can refer to it.

### For us

- A page, behind a login, listing the reports. The log opens as text, and
  the core dump downloads, to decode with the ELF of that firmware version.
  The release process must therefore keep every released version's ELF.
- `tools/logfetch.py R-4F7K` downloads a report to read here, like the live
  log on port 3333.

## Safety

- **Credentials never reach the log.** That is a rule for every log line
  already. A test should grep a long log for the configured passwords and
  fail if one is found.
- **The token** is per knob, random, and only lets that knob add reports and
  forget itself.
- **Everything goes over TLS.** The server accepts nothing else.
- **Nothing is executed from a report.** A log is text, and a core dump is
  only ever read by the decoder.
- **A knob with the service off** sends nothing and writes nothing to the
  card.

## In steps

1. **The log on the card, the address and Send log now.** Reports by hand,
   through the server: the first thing an owner can use, with nothing to
   download or upload. This is enough for the first sessions on the
   IC-7300MK2, the IC-905 and the IC-7760, with **Detailed radio log** on:
   the tester presses **Send log now** after the session.
2. **The automatic reasons.** Their thresholds come from what step 1 shows
   is worth sending.

## Decisions taken, and why

- **No log files for the owner.** Downloading a log and mailing it is an IT
  job, and most hams are not IT people. The knob sends its own reports; the
  owner switches it on, gives an address and, at most, presses a button.
- **Not on a timer.** A report on a schedule mostly carries nothing, and
  every report is personal data leaving someone's house. With a reason
  attached, each one is worth reading, and the owner can see why it went.
- **The SD card, not flash.** Flash writes stop the audio's interrupts (a
  97 ms hole in a call was an NVS write), and flash wears. The card has room
  for days, and its writes do not touch the flash cache.
- **Staged in PSRAM, then sent.** The card's DMA and TLS side by side have
  run the knob out of internal DMA memory before.
- **An email address, confirmed.** It lets us answer, and it stops a knob
  from being signed up with someone else's address.
