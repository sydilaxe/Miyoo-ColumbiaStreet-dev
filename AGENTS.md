# AGENTS.md

Columbia Street is a five-channel jungle sequencer for the Miyoo Mini Plus running OnionOS:
four break channels that play slices of sampled breaks and one bass channel, each sequenced by
three Euclidean cells (HIT, ROLL, ALT). `make` cross-compiles `dist/ColumbiaStreet/columbia`
with zig (ARMv7 hard-float, glibc 2.28) against Onion's own SDL 1.2 and fetches the curated
samples; `make test` renders screenshots and audio on the host into `build/test` and prints the
engine checks. The platform layer (gfx, font, SDL front-end, Wi-Fi sync) is shared in spirit
with the sibling HI5 and Jaffedrone projects; the engine is its own.

Without zig, the Miyoo binary also builds with the OnionOS toolchain image
(`aemiii91/miyoomini-toolchain`) via `make package CROSS=...`; CI does this.

Samples are never committed: `tools/fetch_samples.sh` fetches them from Dirt-Samples at a pinned
commit into `dist/ColumbiaStreet/breaks` and `bass` (both gitignored apart from their
README.txt). The breaks are real recordings sliced at load time (`src/breaks.c`), not
synthesised.

`src/link.c` and `src/midi.c` let this unit follow a TrimUI Brick over USB, ported from the TrimUI
version (sydilaxe/Trimui-Columbiastreet-dev): the USB-C port as a USB gadget through configfs and
FunctionFS, with the ids a following Brick uses, and the clock it receives (MIDI clock, START,
STOP and the leader's position SysEx). `midi.c` is the follower's part of the TrimUI's `midi.c`,
under the same names; keep the two protocols the same. Whether the Miyoo's kernel has USB device
mode is not known: the app checks at start-up and logs what it finds (`link:` in log.txt).
`make test` checks the clock following and the descriptors.

When you change a control or a screen, update the README's Controls section and the footer
hints in `src/ui.c`. When you change the engine, run `make test` and keep its checks passing
(CI greps for them in `.github/workflows/build.yml`).

The app updates itself from the latest GitHub release (`src/update.c`, HTTPS through Mbed TLS,
fetched at a pinned release like the samples). A release is built as the version in its tag (CI
passes `VERSION=` to make, which overrides `CS_VERSION` in `src/engine.h`); keep `CS_VERSION` at
the latest release for untagged builds. When you change the updater or the
package layout, run `make update-test`.
