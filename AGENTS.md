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

When you change a control or a screen, update the README's Controls section and the footer
hints in `src/ui.c`. When you change the engine, run `make test` and keep its checks passing
(CI greps for them in `.github/workflows/build.yml`).

The app updates itself from the latest GitHub release (`src/update.c`, HTTPS through Mbed TLS,
fetched at a pinned release like the samples). A release is built as the version in its tag (CI
passes `VERSION=` to make, which overrides `CS_VERSION` in `src/engine.h`); keep `CS_VERSION` at
the latest release for untagged builds. When you change the updater or the
package layout, run `make update-test`.
