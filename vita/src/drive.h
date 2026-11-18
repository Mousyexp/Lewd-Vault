// Google Drive on the Vita: the vault kept in step with the PC's "Lewd Vault" folder in Drive, the
// way src-tauri/src/gdrive.rs does it (the same folders, the same rules), signed in with a QR code.
#pragma once
#include <stdint.h>

enum { DRV_IDLE, DRV_CODE, DRV_SYNCING, DRV_DONE, DRV_FAILED };
typedef struct {
  volatile int state;
  int signing; // (1: signing in, 0: a sync)
  // signing in: the code to type at the address (the QR code takes the phone there)
  char user_code[32], url[128];
  volatile uint64_t code_until; // (sceKernelGetProcessTimeWide: when the code runs out)
  // a sync: how far, and what it did
  volatile int total, done, up, down, here, there, conflicts, failed;
  volatile int64_t bytes_left; // (downloads still to come)
  char message[300];
  char file[200]; // (the one now)
} DriveStatus;
extern DriveStatus drive;

int drive_has_key(void);
int drive_signed_in(void);
// The "TVs and Limited Input" client of the same Google Cloud project as the PC's key (its files are
// only seen by the same project). Kept in ux0:data/LWDV00001/drive/account.txt; a build can carry one.
void drive_set_key(const char *id, const char *secret);
const char *drive_key_id(void);
void drive_load(void);
void drive_sign_in(void);  // (a thread: the code, then waits for the phone)
void drive_sign_out(void);
void drive_cancel(void);
// A sync of the vault at root with Drive (a thread). folder: Drive's folder ("Lewd Vault", or
// "Lewd Vault (encrypted)" for an Untraceable vault; Drive's must then have the same password).
// make: the folder is made in Drive if it isn't there (0: fetching a vault: it must be)
void drive_sync(const char *root, const char *folder, int make);
// What's in Drive: 1 if the folder exists (signed in; asks Drive, so on a thread or once).
int drive_folder_exists(const char *folder);
const char *drive_last_sync(void);
// 1 when the vault at root has been synced with Drive from this Vita.
int drive_has_manifest(const char *root);
// A finished job's thread joined (its stack given back); called now and then from the main loop.
void drive_reap(void);

// The Vita's own files in Drive, in "Lewd Vault (Vita)" beside the vault's folder (signed in; a
// thread each; -1 when busy or signed out). Two-way Favourites: drive_favs_sync (also done after
// every vault sync) merges this Vita's favorites.tsv with Drive's; drive_favs_state goes 1 (busy),
// then 2 (DRIVE_FAV_MERGED ready: the drawing thread folds it in with lv_favs_merge, mine = the
// favourites now, base = DRIVE_FAV_START, and keeps DRIVE_FAV_MERGED as DRIVE_FAV_BASE) or 3
// (drive_favs_msg says why).
#define DRIVE_FAV_LOCAL "ux0:data/LWDV00001/favorites.tsv"
#define DRIVE_FAV_START "ux0:data/LWDV00001/drive/fav_start.tsv"
#define DRIVE_FAV_BASE "ux0:data/LWDV00001/drive/fav_base.tsv"
#define DRIVE_FAV_MERGED "ux0:data/LWDV00001/drive/fav_merged.tsv"
int drive_favs_sync(void);
extern volatile int drive_favs_state, drive_favs_added, drive_favs_removed, drive_favs_runs, drive_favs_fails;
extern char drive_favs_msg[160];
// A test kit report (the file at path) into "Lewd Vault (Vita)/Test reports" as name
int drive_send_report(const char *path, const char *name);
extern volatile int drive_report_state; // (0, 1 sending, 2 sent, 3 failed)
extern volatile int drive_side_busy;
