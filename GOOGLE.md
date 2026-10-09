# Google Contacts on the Telephone knob

The Telephone firmware (`vfo-knob-phone`) can take your **starred** Google
contacts and make them the knob's favourites. Those are the names the dial steps
through and the slab calls. The knob signs in to Google once, then keeps the
list up to date by itself. It only ever reads your contacts.

This page covers:
- setting it up: your own Google Cloud client, the small return page, and the knob;
- what the knob does with your contacts and what it stores;
- every message the page can show, and what to do about each;
- the page's API.

The Telephone guide itself is [docs/phone.md](docs/phone.md).

## At a glance

| | |
|---|---|
| What it reads | the contacts in your **Starred** group, with their names and phone numbers |
| What it writes to Google | nothing: the permission is read-only (`contacts.readonly`) |
| What it becomes | the favourites list: up to 40 entries, sorted by name, one per number |
| When it syncs | when the knob starts (if signed in), every six hours, right after you sign in, and when you press **Sync now** |
| What a sync needs | the knob on WiFi and no call up. A SIP account is not needed |
| What you need | a free Google Cloud project of your own. The return page it needs is on the VFO-Knob website, at `https://vfoknob.com/google.html` |

## Why it needs two things of yours

**Your own Google Cloud client.** Google lets a device read contacts only through
an app registered with Google, an *OAuth client*. Nothing of Google's is built
into the knob. Each owner makes a client in their own free Google Cloud
project, and the knob signs in with that.

**A return page at an `https` address.** After you sign in, Google sends the
browser back to an address registered with the client. Google only accepts
`https` addresses there. Your knob is `http`, on your own network, and Google
cannot reach it. So Google returns to a small static page, `google.html`, and
that page hands the sign-in on to your knob. The page has no server code and
holds no secrets. It works from any `https` host.

## 1. The return page

The knob expects the page at:

```
https://vfoknob.com/google.html
```

That is the repository's `docs/google.html`, served by the VFO-Knob website.
An owner has nothing to do here but put that address in their Google client
(step 4 below). It is the firmware's default, so the knob's page has it
already.

**For the website.** Serve `docs/google.html` as it is, at exactly that
address:
- **The exact address.** No redirect to another path: not `/google`, and no
  added slash. Google returns to the address registered in each owner's client,
  character for character, and the firmware has this one built in
  (`DEFAULT_RELAY` in `components/phone_client/contacts.c`). If the site cannot
  serve it there, change the firmware and every page that names it, together.
- **The query string intact.** Google appends `?code=…&state=…&scope=…` (or
  `?error=…&state=…`), and the page reads `code`, `state` and `error` from it.
- **Its script runs.** The page is its inline script. A Content-Security-Policy
  must allow that script, or the script moves to a `.js` file on the same site.
- **Nothing third-party on it.** No analytics, tag manager, chat widget or font
  service. Its address carries a sign-in code; the code is useless without the
  owner's client secret, but it has no place in anyone's logs.
  `Referrer-Policy: no-referrer` suits it.
- **Restyle it freely.** It may wear the site's look, as long as the script and
  its LAN check stay as they are.

**Or host a copy yourself.** If you would rather not depend on the website, copy
`docs/google.html` to any `https` address you control, for example
`https://example.org/vfo/google.html`. Then enter that same address in two
places:
- in your Google client's **Authorized redirect URIs** (step 4 below);
- on the knob's page, in **Sign-in returns through (the client's redirect URI)**.

The two must match exactly, character for character.

## 2. Your Google Cloud client

This takes about five minutes, once. The console names below are Google's as of
2026. Since April 2025 the OAuth pages have their own section, the **Google Auth
Platform**, which was formerly the *OAuth consent screen* under *APIs &
Services*.

1. **A project.** At [console.cloud.google.com](https://console.cloud.google.com/),
   create a project, for example *VFO-Knob*.
2. **The People API.** Under **APIs & Services → Library**, find the
   **Google People API** and press **Enable**.
3. **The app.** Open **Google Auth Platform** and press **Get started**:
   - give the app a name (*VFO-Knob* will do) and a support email address;
   - choose **External** as the audience;
   - give a contact email address, and finish.

   Then open **Audience** and press **Publish app**, so its status reads
   *In production*. Two reasons:
   - An app left in *Testing* can only be used by the test users listed there.
   - In *Testing*, Google expires every sign-in seven days after consent.
     The knob would lose access every week.

   You do not need Google's verification for this. See
   [the unverified-app warning](#the-unverified-app-warning).
4. **The client.** Under **Clients**, press **Create client**:
   - choose **Web application**;
   - under **Authorized redirect URIs**, add the return page's address exactly:
     `https://vfoknob.com/google.html`, or your own copy's;
   - press **Create**;
   - copy the **Client ID** and the **Client secret**.

   **Copy the secret now.** Since June 2025, Google shows a new client's secret
   only on this screen. Afterwards the console shows only its last four
   characters. If you lose it, add a new secret to the same client (a client can
   have two), then disable and delete the old one.
5. **On the knob**: see the next section.

Optionally, under **Data Access**, add the scope
`https://www.googleapis.com/auth/contacts.readonly` ("See and download your
contacts"). The knob asks for it at sign-in either way.

One client can serve all your knobs. They share the same redirect address, and
each knob signs in for itself.

## 3. On the knob

Open the knob's configuration page:
- hold a finger on the meter arc until the knob clicks, and browse to the address
  on the card;
- the user is `admin`, and the password is `admin` until you change it.

Use a computer or phone **on the same network as the knob**. At the end of the
sign-in, the browser itself goes back to the knob's local address. Open the page
by the knob's IP address (for example `http://192.168.1.40`) or by
`http://vfo-knob.local`. The address you use is the one the sign-in returns to.

Under **Google Contacts**:

1. Paste the **OAuth client ID** and the **Client secret**.
2. If you host the return page yourself, put its address in **Sign-in returns
   through**. Otherwise leave the default, `https://vfoknob.com/google.html`.
3. Press **Save the client**. The page says *Google client saved.*
4. Press **Sign in with Google**. The button stays greyed out until a client ID
   and a secret are saved.
5. Choose your Google account. Google then warns that it *hasn't verified this
   app* (see below). Go on, and **allow** the app to see your contacts.
6. Google returns to the return page, which takes you back to the knob's page,
   at its Google Contacts section. The **Sync** row says *signed in, reading the
   contacts*. A few seconds later it says, for example, *12 favourites from
   Google*, and the **Favourites** list above it fills in.

### The unverified-app warning

Reading contacts is what Google calls a *sensitive* permission. An app asking
for one without Google's review shows a warning screen first: *Google hasn't
verified this app*. The app is your own, so this is expected:
1. choose **Advanced**;
2. choose **Go to VFO-Knob (unsafe)**, which carries the name you gave the app;
3. continue, and allow access to your contacts.

Google allows an unverified app up to 100 users in total. That is no limit for a
client only you use.

## What the knob does with your contacts

- **Which contacts.** Your **Starred** group: the contacts you starred in
  Google Contacts or on your Android phone. Up to 200 are read.
- **Which numbers.** Every phone number of each starred contact. The knob uses
  Google's international form when Google has one (`+32475123456`), otherwise
  the number as it was typed. It keeps only the digits, and a `+` at the start.
- **The names.**
  - A contact with one number keeps its name.
  - A contact with several numbers gets one favourite per number. Each is named
    by up to 14 characters of the name and up to 8 of the number's kind:
    *Jan Peeters Mobile*, *Jan Peeters Work*.
  - A contact without a name is named by its number.
- **The list.** Sorted by name, at most 40 favourites. Each sync **replaces the
  whole list**, entries added by hand included. To keep a list of your own,
  sign out of Google and edit the favourites on the page.
- **Starred contacts without a number** are skipped. If none of the starred
  contacts has a number, the knob's log says so.

## What the knob keeps

| What | Where | |
|---|---|---|
| The client ID | the knob's settings: its SD card, or its own memory without one (namespace `phone`, key `gcid`) | shown on the page, as every sign-in link carries it anyway |
| The client secret | the settings (`gcsec`) | never shown again, and never returned by the API (only whether one is set) |
| Google's refresh token | the settings (`grtok`) | what lets the knob sync without you |
| The return page's address | the settings (`grelay`) | only when it is not the default |
| An access token | RAM, during a sync | gone when the sync is done |

**Sign out** on the page deletes the refresh token. The client stays saved for
the next sign-in, and the favourites stay as they are.

**To cut the knob off from Google's side**, open
[myaccount.google.com/connections](https://myaccount.google.com/connections)
("Your connections to third-party apps & services"), choose the app, and remove
its access. The knob's next sync then fails with *Google's sign-in expired: sign
in again*, and it deletes its token.

## How it works

```
 browser           knob (http, LAN)          Google                 return page (https)
    |  Sign in with Google  |                    |                           |
    |---------------------->|                    |                           |
    |  302 to Google, state = "<knob address>/<nonce>"                       |
    |<----------------------|                    |                           |
    |  sign in, allow contacts                   |                           |
    |------------------------------------------->|                           |
    |  302 to the return page, with code and state                           |
    |<-------------------------------------------|                           |
    |------------------------------------------------------------------------>|
    |  the page checks the knob address is on the LAN,                       |
    |  and sends the browser on to http://<knob>/api/phone/google/callback   |
    |<------------------------------------------------------------------------|
    |---------------------->|  nonce checked, used once                     |
    |  302 to the page, #phonegoogle             |                           |
    |<----------------------|                    |                           |
    |                       |  then, by itself:  |                           |
    |                       |  code -> refresh token (HTTPS, with the secret)|
    |                       |------------------->|                           |
    |                       |  the starred group, then its people            |
    |                       |------------------->|                           |
    |                       |  favourites saved  |                           |
```

In words:

1. **Sign in with Google** sends the browser to Google, with the knob's address
   (as your browser had it) and a fresh nonce in `state`.
2. After you allow access, Google sends the browser to the return page with a
   one-time code.
3. The return page sends the browser on to the knob, and only to a LAN address:
   - `10.x.x.x`, `172.16-31.x.x` or `192.168.x.x`;
   - a name without dots (`vfo-knob`);
   - a name ending in `.local`, `.lan`, `.home`, `.internal` or `.home.arpa`;
   - optionally with a `:port`.

   For anything else it says there is no knob to return to.
4. The knob checks the nonce: it must be the one from its own pending sign-in,
   and it is used once. The nonce lives in RAM, so a knob that restarted in
   between asks you to sign in again. The browser goes straight back to the
   knob's page, which shows the sync as it runs.
5. In the background, the knob trades the code for a refresh token, directly
   with Google over HTTPS, using the client secret it holds. Then it reads the
   starred group (the People API's `contactGroups/starred`), fetches those
   people's names and numbers in batches of 20 (`people:batchGet`), and saves
   the favourites.

The code that passes through the browser is useless without the client secret,
which only the knob holds. The knob's page and its Google endpoints are behind
the page's login.

## The page and its API

The **Google Contacts** section shows on the Telephone firmware's page only:

| Row or control | |
|---|---|
| **Account** | *signed in* or *not signed in* |
| **Sync** | the state, why, and how many minutes ago the last good sync was: *ok · 12 favourites from Google · 3 min ago* |
| **OAuth client ID**, **Client secret** (with **Show**) | your client's |
| **Sign-in returns through (the client's redirect URI)** | the return page's address; it must start with `https://` |
| **Save the client** | stores the three fields. An empty secret field keeps the secret already saved |
| **Sign in with Google** | greyed out until a client ID and a secret are saved |
| **Sync now** | greyed out until signed in |
| **Sign out** | shown only while signed in. It asks first: *Sign out of Google? The favourites stay as they are.* |

The endpoints are all behind the page's login (HTTP basic auth, `admin` and your
password):

| Request | |
|---|---|
| `GET /api/phone/google` | the state, as JSON (below) |
| `POST /api/phone/google` | form fields `client_id`, `client_secret`, `relay`. Quotes, backslashes, spaces and control characters are stripped; a `relay` must start with `https://` |
| `GET /api/phone/google/signin` | a 302 to Google's sign-in, or `{"ok":false,"why":"set the Google client first"}` |
| `GET /api/phone/google/callback` | where the return page brings `code` (or `error`) and `state`; answers with a 302 to `/#phonegoogle` |
| `POST /api/phone/google/sync` | a sync, now (once the knob is on WiFi with no call up) |
| `POST /api/phone/google/forget` | signs out: the refresh token is deleted, the favourites stay |

`GET /api/phone/google` returns:

```json
{"client":true,"client_id":"1234-abc.apps.googleusercontent.com","secret":true,
 "signed_in":true,"relay":"https://vfoknob.com/google.html",
 "state":"ok","why":"12 favourites from Google","count":12,"minutes_ago":3}
```

| Field | |
|---|---|
| `client`, `secret` | whether a client ID and a secret are saved. The secret itself is never returned |
| `client_id`, `relay` | as saved |
| `signed_in` | the knob holds a refresh token |
| `state` | `idle`, `syncing`, `ok` or `failed` |
| `why` | the message for the state (see below) |
| `count` | favourites from the last good sync |
| `minutes_ago` | since the last good sync; `-1` if none since the knob started |

The favourites themselves are at `GET /api/phone/favourites`.

## Messages

What the **Sync** row can say:

| State | Message | Meaning, and what to do |
|---|---|---|
| idle | *not signed in* | no sign-in yet: press **Sign in with Google** |
| idle | *signed out* | after **Sign out** |
| syncing | *signed in, reading the contacts* | back from Google; the first sync is running |
| syncing | (empty) | a sync is running |
| ok | *12 favourites from Google* | done; the list on the page is the new one |
| failed | *the sign-in was cancelled* | you chose **Cancel** at Google, or did not allow access: sign in again |
| failed | *not this knob's sign-in: sign in again* | the return carried no nonce of this knob's: an old Google tab, a second attempt, or the knob restarted in between. Start again from the page |
| failed | *Google refused the sign-in*, or Google's own words (*Unauthorized*, *Bad Request*, ...) | the code could not be exchanged: usually a wrong client secret (`invalid_client`), or a return address that differs from the client's redirect URI. Check both and sign in again |
| failed | *Google's sign-in expired: sign in again* | Google no longer accepts the token. Either the app is still in *Testing* (seven days), you removed its access, or the token went unused for six months. The knob has deleted it: publish the app if needed, and sign in again |
| failed | *Google refused the token* | Google refused to renew access for another reason: check the client in Google Cloud |
| failed | *Google not reached* | no answer from Google, after three tries: check the knob's internet connection |
| failed | *Google not reached: sign in again* | the sign-in came back, but the knob could not reach Google to finish it (three tries). The code it brought is spent: sign in again |
| failed | *the starred contacts were refused* | usually the **People API is not enabled** in the project (Google answers 403, `SERVICE_DISABLED`). Enable it, wait a few minutes, press **Sync now** |
| failed | *the contacts could not be read* | a list came back incomplete or unreadable; the favourites were left as they were. Press **Sync now** |
| failed | *no memory for the sync just now* | the knob is short of memory for a moment; it tries again by itself half a minute later |
| failed | another word such as `invalid_request` | Google's error code, passed on as it is |

The knob's log, on TCP port 3333, has the details under the tag `contacts`. For
example: *sync: free internal 26435, largest 11776* as a sync starts, *signed in
to Google*, *sign-in refused: 401 Unauthorized*, *people.googleapis.com not
reached (...): again*, *12 favourites from Google*, *5 starred contacts, none
with a number*.

A sync keeps one connection to each of Google's servers for all its requests,
and runs on a stack in PSRAM: a TLS handshake costs the knob seconds of
software cryptography and some of the internal RAM its WiFi needs, so it makes
as few as it can. A whole sync takes two or three seconds.

## Troubleshooting

| What you see | Why, and the fix |
|---|---|
| Google: **Error 400: redirect_uri_mismatch** | The knob's **Sign-in returns through** differs from the client's **Authorized redirect URIs**. Make them identical, then sign in again |
| Google: **Access blocked**, the app *has not completed the Google verification process* or *is currently being tested* | The app is still in *Testing*, and your account is not a test user. Under **Google Auth Platform → Audience**, press **Publish app** |
| The return page: *there is no knob to return to* | The knob's page was opened by an address the return page does not accept: a public name, an IPv6 address, or a name in a router's own domain such as `.fritz.box`. Open the knob's page by its IP address, or by `vfo-knob.local`, and sign in again |
| The browser cannot open the knob after Google | The browser is not on the knob's network (a phone on mobile data, say), or it is set to HTTPS only. Sign in from a device on the same network, and allow plain `http` for the knob's address when the browser asks |
| **404** at the return page's address | The website does not serve `google.html` there (yet). Host a copy yourself meanwhile, and enter its address in both places (see [the return page](#1-the-return-page)) |
| *not this knob's sign-in: sign in again* | Start the sign-in again from the knob's page, and finish it in the same tab |
| Signed out every week | The app is in *Testing*: publish it, then sign in once more |
| *Google refused the sign-in* right after Google | Usually the client secret was mistyped. Paste it again and **Save the client**. If it is lost, add a new secret to the client in Google Cloud |
| *the starred contacts were refused* | Enable the **Google People API** in the same project as the client, wait a few minutes, press **Sync now** |
| *0 favourites from Google* | No starred contact has a phone number. Star some in [contacts.google.com](https://contacts.google.com), then press **Sync now** |
| Favourites added by hand have gone | A sync replaces the list. Star those contacts in Google instead, or sign out to keep a list of your own |
| The client disappeared from Google Cloud | Google deletes OAuth clients unused for six months. A knob that syncs every six hours keeps its client in use, but one left switched off for that long does not. Restore it within 30 days under **Deleted credentials**, or make a new one and sign in again |

## Limits worth knowing

- **Starred group:** up to 200 members are read; up to 40 favourites are kept.
- **Batches:** names and numbers are fetched 20 people at a time. A reply too
  long to read in full fails the sync, and the old list stays.
- **Google's limits:**
  - 100 live refresh tokens per Google account per client. Signing in a 101st
    time quietly retires the oldest.
  - An unverified app may serve 100 users in total.
  - Neither matters for one owner's knobs.

## For website builders

The sources for pages about this feature:

| File | |
|---|---|
| `GOOGLE.md` | this page: the setup, its reasons, and every message |
| `docs/phone.md` | the Telephone guide: the account, the face, calling, the keypad, the headset or speaker, the knob's battery, the API, and a short Google Contacts section |
| `docs/google.html` | the return page itself, to be served as it is at exactly `https://vfoknob.com/google.html`: see [the return page](#1-the-return-page) for what the site must and must not do with it |
| `docs/phone/*.svg` | the guide's sixteen pictures, drawn from the firmware's own layout by `tools/mkdocs.py phone`: the face with its parts named, turning, the keypad, calling, a call coming in, a call with its parts named, DTMF, ended, NO SERVICE, mute, a headset, the call history, a missed call, a call coming in with a headset and with a speaker, and the knob's own battery |
| `docs/display-phone.svg` | the face alone on its glass, in a call, for the README (`tools/mkrender.py --glass --radio phone`) |
| `docs/marketing/phone/` | product renders, SVG and PNG: the knob angled with room for text left or right, and upright on a desk, each in the blue and the black finish (`tools/mkrender.py --radio phone`). The folder is kept out of git (`.gitignore`): it exists where that command ran |

The numbers in the pictures are Ofcom's, kept for drama (`+44 7700 900xxx`,
`+44 1632 960xxx`): no one answers them.

Facts to keep consistent across pages:
- the permission is **read-only**;
- **starred** contacts only;
- up to **40 favourites**;
- a sync **every six hours**, and only with **no call up**;
- each owner uses **their own** Google Cloud client;
- the return page's address is `https://vfoknob.com/google.html`, built into the firmware.
