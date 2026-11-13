# The iOS port

This directory holds everything that is **only** about running Lewd Vault on iOS (as a sideloaded
IPA, iOS 26+). The app is one Tauri crate: iOS is a second target of the same Windows code, not a
fork. So this directory is a *layer*, and the shared code it builds on stays where it was.

**The one rule that outranks everything:** a vault folder must stay fully interoperable
Windows ↔ iOS — the same `index.csv`, `views.csv` and `_app/` layout, byte for byte. Any change here
that would alter the on-disk vault format is wrong. See [docs/PORT.md](docs/PORT.md) §5 and the
vault-format tests in `src-tauri/tests/`.

## Layout

```
mobile/
  ui/                 the iOS front-end layer (loaded only under html.ios)
    ios.js            structure + behaviour: bottom tab bar, large collapsing title, viewer chrome
    ios.css           the iOS skin (Liquid Glass, Apple-Photos layout); appended after desktop.css
  docs/
    PORT.md           the full port research: feasibility, compatibility matrix, CI, decisions
    PHASE-1.md        the Phase 1 plan (foundation, format compatibility, Liquid Glass) + exit criteria
  native/
    README.md         the platform-native pieces that must stay in src-tauri (tool-fixed paths)
```

Nothing else lives here. The shared page (`ui/`), the Rust backend (`src-tauri/src/`) and the build
(`sync-ui.mjs`) are shared with Windows and are edited in place, with `#[cfg(target_os = "ios")]`
or `html.ios` guards where the two platforms differ.

## How the pieces fit

The iOS front-end is assembled by `sync-ui.mjs` into the built `src/`, exactly like the desktop one:

| Source | Becomes | Loaded |
| --- | --- | --- |
| `ui/desktop.css` + `mobile/ui/ios.css` (concatenated, iOS last) | `src/mobile.css` | `<link href="/mobile.css">` |
| `mobile/ui/ios.js` | `src/ios.js` | `<script src="/ios.js">` |

`ios.css` is appended **after** `desktop.css` on purpose: it overrides the desktop rules, so it must
stay last. On Windows none of it applies — every rule is gated on `html.ios` (or `html.desktop`, which
iOS also carries) — and `ios.js` early-returns because `lib.rs` never adds the `ios` class there.

The Rust side turns the layer on with one line in `src-tauri/src/lib.rs`: an initialization script that
adds `ios` to `<html>` before the page's own scripts run, so nothing flashes in desktop shape. The rest
of the Rust branching is `#[cfg(target_os = "ios")]` (storage roots, the loopback media server, the
recycle-bin shim); those files live in `src-tauri/src/`, not here, because they are the same modules the
desktop build compiles.

## Building and previewing

- **The IPA** is built on a macOS runner by [`.github/workflows/ios.yml`](../.github/workflows/ios.yml)
  (Actions → Build iOS → Run workflow). It runs `npm test`, then `tauri ios init` / `tauri ios build`.
  The result is unsigned by default (`--no-sign`) and is signed on the device by your sideloading tool
  (AltStore/SideStore, TrollStore, or your own certificate). See [docs/PORT.md](docs/PORT.md) §6.
- **The UI without a Mac.** `npm test` runs the guards; to *see* the iOS layout, build `src/`, add
  `document.documentElement.classList.add('ios')` to a copy of `src/index.html`, and open it in a
  WebKit-based browser (or Safari's responsive mode). The iOS CSS is pure `html.ios` CSS, so it renders
  anywhere. `mobile/docs/PORT.md` §12 records the layout checks and the bugs they caught.

## Where to start reading

1. [docs/PORT.md](docs/PORT.md) — why iOS 26, what is shared vs adapted, the compatibility contract.
2. [docs/PHASE-1.md](docs/PHASE-1.md) — the concrete Phase 1 tasks and their exit criteria.
3. `mobile/ui/ios.js` then `mobile/ui/ios.css` — the actual iOS layer, ~260 lines together.
