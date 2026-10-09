#pragma once
// The Videos tab, a small file manager. Its top level lists the sources: the SD card, SMB
// shares and SFTP servers. Each list is made of VideoEntry items with a path; for a folder,
// the path is where it leads. app.files_dir says what is being shown:
//   ""                        the sources
//   "sdmc:/..."               a folder on the SD card
//   "smb:", "sftp:"           the servers added so far, plus "Add server" ("smb:+", "sftp:+")
//   "smb://<server>/<share>/<dir>/", "sftp://<server>/<dir>/"
//                             a network folder (see netfile.c); folder URLs end with '/'
#include <stdbool.h>
#include <stddef.h>

#define FILES_SD_ROOT "sdmc:/"

// Lists dir into app.files and app.nfiles, and makes it app.files_dir.
void files_scan(const char* dir);
// Listing a network folder takes two steps. files_net_fetch talks to the server, which is
// slow; it touches nothing in App, so it can run on another thread. files_net_apply then
// builds the list from what was read, on the main thread. If n < 0, err says what went wrong
// and is shown as the status. files_scan does both steps in one go.
int files_net_fetch(const char* dir, char* err, size_t errlen);
void files_net_apply(const char* dir, int n, const char* err);

// Refreshes where each listed file was left (VideoEntry.resume_ms, see resume.h). Called
// again after a file has been played.
void files_update_resume(void);

// The folder above dir. "sdmc:/a/b" gives "sdmc:/a", "sdmc:/a" gives "sdmc:/", and the SD
// card's root goes back to the sources (""), which have no parent.
void files_parent(const char* dir, char* out, int size);

// dir as the user should read it, e.g. "SD card > videos > Movies", or "Videos" for the sources.
void files_location(const char* dir, char* out, size_t size);

// A network folder or file. These are slow to list, so the caller shows that it's connecting.
bool files_is_network(const char* path);
