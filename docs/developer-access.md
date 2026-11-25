# The developer shell, logs, and a state area that repairs itself

Three things that make the unit reachable and self-sufficient once it is in
the car, where there is no console.

What still needs a bench: a change to the MCU's firmware (SWD) and a
damaged U-Boot or U-Boot environment. Everything else is reached by the
shell, an app update over Wi-Fi or the rescue stick.

## The shell: "Developer access"

- **Who gets in.** Whoever holds the private half of a key whose public half
  was built into the image (`local/keys/dev-authorized_keys`, from `make keys`, copied to
  `/opt/autotop/sys/authorized_keys`). An image built without that file has
  no developer access and its launcher shows no row for it. The private
  half stays in `~/.config/bm168-mb-v04/`, outside the repository.
- **Off unless switched on.** Settings → System → "Developer access"
  (between "System info" and "Reboot"; a choice like the others, a push
  switches it). The choice is kept on the state partition by the system's
  own script and holds across starts and reinstalls.
- **Part of the system, not of the app.** `init-b` starts it at boot
  (`/opt/autotop/dev-access start`), whatever an app update does to the
  launcher. It still needs the unit's Wi-Fi, which the app's session script
  brings up: a broken app can take the Wi-Fi away, and then the rescue stick
  is the way back.
- **The server** is Dropbear 2025.89 (pinned in `sources.lock.json`, static,
  383 KB), limited at build time (`recovery/plan-a/dropbear-options.h`):
  keys only (no password code at all), one key type (Ed25519), nothing
  forwarded, no file-transfer subsystem. Its own key is made on the unit the
  first time access is switched on and kept on the state partition, so the
  Mac knows the unit by the same key afterwards.
- **Never a login shell.** Every session goes through
  `/opt/autotop/sys/dev-shell` (the server's forced command). The image's
  `/etc/profile` is stock's own start-up; a login shell would run it and
  start stock's drivers and Launcher over the running system.
- **Root's home in RAM.** The server takes a key list only from a directory
  that is root's all the way up or up to root's home, and nothing in the
  image is (stock's directories belong to user 1000, `/` is group-writable).
  So `dev-access` mounts a tmpfs on `/root` and puts the list there.

From the Mac:

    tools/unit-wifi-join                   onto the unit's Wi-Fi (macOS; after every restart of the unit)
    tools/unit-ssh                         a shell
    tools/unit-ssh 'tail -n 20 /tmp/launcherd.log'
    tools/unit-ssh --get /tmp/launcherd.log .
    tools/unit-ssh --put dist/x /tmp/x

The unit's access point hands out itself as router and name server (that
keeps a phone on it during an update). A Mac whose only network is that
Wi-Fi has no internet while it is joined; it needs a second connection
(Ethernet, or a phone on USB) ordered above Wi-Fi.

### The Wi-Fi passphrase

- **Words.** Four words of the EFF's short list with a digit between, like
  `acorn4river7tulip2mango` (`launcher/ap_words.h`; about 50 bits). Before,
  24 random characters, which are a pain to type; three words
  (37 bits) could be tried exhaustively in about a day by someone who had
  recorded a phone joining, so it is four.
- **It stays while developer access is on.**
  Otherwise the unit makes a new passphrase at every start and every phone
  switch, which is what keeps a phone that was left behind off its Wi-Fi;
  a developer's computer would then need the new one typed from the unit's
  screen after every start. So, with developer access on, one passphrase is
  made when it is switched on, kept with the unit's state
  (`/data/autotop-ap-kept`) and put back at every change; switching
  developer access off removes it and makes a new one at once. The cost:
  for as long as it is on, a phone that once had the passphrase can rejoin.

## The logs as a file

`/opt/autotop/bin/logs-pack FILE` (part of the app) puts the kernel's
messages, every log under `/tmp` and under the stock root's `/tmp`, and a
few facts about the running system into one tar (the last 2 MB of each).
The Software update page has a link, "Save this session's logs"
(`GET /logs`), so they can be taken with a phone and handed on;
over the shell, `tools/unit-ssh '/opt/autotop/bin/logs-pack /tmp/l.tar'`
and `--get`. They name the phones the unit knows; the unit sends them
nowhere by itself. They are this session's only: logs live in RAM and are
gone at power-off.

## A state partition that repairs itself

`/opt/autotop/state-mount` (called by `init-b`) arms the driver's window,
attaches stock's `userdata` and mounts it read-only, as before. New: if that
fails twice in the same start, the partition is made anew (erased, attached,
the volume created, an empty filesystem, a note `autotop/made-anew`). What
was kept is lost then (pairings, settings, a kept app update; the image's
own copies stand in), but the unit keeps things again from that start on.
Nothing is erased unless the driver took the arming and both attempts
failed.

The attach after the erase is by `sys/ubi-attach` (a few lines of C), not
by busybox: UBI holds back blocks for ones that go bad, by the kernel's
default 20 in every 1,024 of the whole chip, which is 20 of this
partition's 48; busybox 1.25 cannot say otherwise. Stock's own holds back 2.

## Tests (offline)

| Test | What |
| --- | --- |
| `make system-test`, first part (58 checks) | `dev-access` and `state-mount` run by the unit's own shell (stock busybox under qemu-arm) with stand-ins for the server and the flash commands: no key, on, twice on, restart, off, a server that fails, no state partition; a good partition, one failure, two failures of attach or of mount (made anew: the right partition erased, volume, note), an erase that fails (disarmed), no partition |
| `make system-test`, second part (11 checks) | the real server as built for the unit (ARM, under qemu-arm) on the container's loopback and Dropbear's own client: the listed key gets in, another does not, a command's output and status, a shell that is not a login shell, files there and back by scp |
| `make ui-test` | 40 scenarios (three new: no row without a key; with one, the row and both switches; on from an earlier start), 247 navigation checks |
| `launcher/test_update.sh` (24 checks) | the page offers the logs, `/logs` gives the file the command made as a download and removes it, by a name it is only the phone's "yes" |
