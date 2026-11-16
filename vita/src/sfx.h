// The Vita's own interface sounds (sounds/*.wav, from the system's common resources), mixed on a
// voice port of their own so they never wait for a video's sound.
#pragma once
enum { SFX_TICK, SFX_SELECT, SFX_BACK, SFX_TOGGLE, SFX_OPEN, SFX_CLOSE, SFX_TAB, SFX_ERROR, SFX_SAVED, SFX_DONE,
       SFX_DELETE, SFX_SLIDESHOW, SFX_MORE, SFX_START, SFX_COUNT };
void sfx_init(void);
// Plays a sound (at the volume set: 0 is off). A sound already playing that recently isn't stacked.
void sfx(int id);
void sfx_volume(int percent);
