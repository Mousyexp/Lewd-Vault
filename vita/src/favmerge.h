// Two-way Favourites: see favmerge.c.
#pragma once

// mine, theirs, base: favorites.tsv-style files (a missing one is an empty list); the merged list is
// written to out (through out.part). added: how many came from theirs; removed: how many of mine went.
// 0, or -1 (out of memory, or out couldn't be written).
int lv_favs_merge(const char *mine, const char *theirs, const char *base, const char *out, int *added, int *removed);
