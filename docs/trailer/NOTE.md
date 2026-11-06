# Lewd Vault trailer: "One key" (13.5 s, 1920x1080, 30 fps, silent)

## The idea
The emergency key. It is the one moment only this app owns: you're enjoying the vault, someone's at the door, you press
Ctrl+Shift+Space, and the screen is a dull blue clock. Press it again and you're back. Cover the logo and the film is still
about this app's key, not about "a gallery app". The ending pushes into the app's own sidebar heart and wordmark, and the
README's own voice finishes it: "100% private, and all yours."

## Beats and what each buys
| frames | beat | buys |
|---|---|---|
| 0-62 | Library, magnified, gliding over the five pictures | the viewer sees a lovely vault of spicy art |
| 62-110 | Add screen, push into the tag chips | you file it like a booru: category, tags, rating |
| 110-152 | Slideshow, slow push | you enjoy it full screen |
| 152-199 | the room dims, "Someone at the door?", the three keys crash in and go down together | the danger, and the cause |
| 199-265 | the app's screensaver, a 2.2 s breath (the joke) with drifting light and a creeping camera; the keys come back and go down | one key hides everything, convincingly (the point) |
| 265-296 | Library snaps back, wide | and it all comes back |
| 296-340 | Untraceable page, push | and on a work PC, it can leave no trace |
| 340-405 | camera into the sidebar heart + "Lewd Vault"; the vault closes around it; the line rises | whose app this is |

## What is real and what is built
- Real: every UI frame is the real page with the real Rust backend (tools/screenshots.mjs, `--dpr 3`), the five demo pictures
  added through the real Add screen; the lockup is a crop of the real sidebar; the words are the app's ("Make it untraceable"
  is on screen as the UI's own title) and the README's ("100% private, and all yours.").
- Rebuilt from the app's own code: the screensaver (ui/desktop.css `.pv-screen`: aurora colours, clock), so its light can drift.
  The clock reads 10:10 and the date is fixed (illustrative).
- Built for the film: the three keycaps (the app's kbd keycap, made physical), the room dimming, the line "Someone at the door?".
- No sound: no ElevenLabs key was available, so there is no voice, music or SFX. The cuts are placed for sound (the two presses
  at 6.63 s and 8.83 s are where hits would go).

## Gates (numbers printed by the scripts)
- look.py (look-final/): G0 PASS, duration 13.50 s matches --expect 13.50 s, audio none (not required). G2 PASS, no run of
  more than 3 flat frames. G3 PASS, end hold 0.30 s (ends mid-motion, intended: the last word is still settling), final shot
  1.30 s = 10 % of the film. 8 cuts, all explained by events; mean shot 1.50 s (0.70-2.97 s). Motion: mean frame-to-frame luma
  change 4.9/255, inside the human band (4.4-9.8), on the calm side. Longest static-ish stretch 6.67-8.13 s (1.47 s): the
  screensaver, on purpose (the joke's breath; the aurora and camera keep it alive).
- lint.mjs: GATE G5 PASS. One WARN (D1): the screensaver's "10:10" reads as a left-anchored headline; it is the app's own
  screensaver layout, held 2.2 s. The first version failed C1 (the date as a small corner label); fixed by filming the
  screen closer (camera at 1.25-1.31x on the clock), not by hiding it.
- Motion.selfTest: ok, 24 random seeks, 73 elements, 0 mismatches.
- G1 (by eye): tag chips about 0.04 H after the push was raised to 2.8-3.25x; the Untraceable page's title and fields read at
  2.35-2.7x. The Library glide shows pictures, not text, at 1.6-1.7x.
- G4 (by eye): one hero per frame; the turn stacks the question over the keys as one moment.

## One sentence per sheet frame (look-final/sheet.jpg)
1-2 Library magnified, five pin-ups and the tag column, gliding left. 3-4 the tag chips of the bear picture, pushing in.
5-6 the slideshow's beach picture, pushing in. 7 the room dims and "Someone at the" rises. 8 the question and the three keys.
9-10 the blue screensaver, "10:10". 11-12 the Library wide, snapping back. 13 the Untraceable page with its password fields.
14 the sidebar heart and wordmark filling the frame over the Library tile. 15-16 the heart and "Lewd Vault" alone, then the line.

## What I would still improve
- Sound: a soft room tone that cuts dead on the first press, and a tiny click on each key, would double the joke.
- The Library glide is a camera on a still; a scroll of a bigger demo library would feel more alive.
- The keys could cast a shadow onto the picture they cross.
- The trailer is 16:9 only; a 9:16 cut for phones would need its own framing.

## Rebuilding it
`index.html` is the editable film. Next to it, it needs:
- `runtime/`: the motion runtime from the motionmaxxing skill (`~/.claude/skills/motionmaxxing/runtime`);
- `fonts/`: `nunito-latin.woff2` and `dancing-latin.woff2` from `src/vendor/` (after `npm run sync`);
- `plates/`: sharp frames of the real app, made with
  `node tools/screenshots.mjs <the 5 demo pictures> --dpr 3 --frames <dir>`, saved as `library.jpg`, `add.jpg`,
  `slideshow.jpg`, `untraceable.jpg`, plus `lockup.png` = `library.png` cropped to (0, 0, 705, 189).
Then `node <skill>/scripts/render.mjs index.html final.mp4` and `python3 <skill>/scripts/look.py final.mp4 --expect 13.5`.
