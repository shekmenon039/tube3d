#pragma once
// Reading files from SMB shares (libsmb2) and SFTP servers (libssh2) for the player. Like
// hstream.c does for HTTP, a read-ahead thread keeps a window of the file filled while FFmpeg
// reads from it, so seeking within the window costs nothing. URLs follow servers.h:
// smb://<server>/<share>/<path> and sftp://<server>/<path>.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct NetFile NetFile;

// Connects and opens the file. This blocks for a second or two. Returns NULL on failure, with
// the reason in err.
NetFile* nf_open(const char* url, uint32_t window_bytes, char* err, size_t errlen);
// Reads up to size bytes at the current position, waiting for the network if needed. Returns
// 0 at the end of the file, and -1 on an error (nf_error describes it) or once *stop is set.
int nf_read(NetFile* f, uint8_t* buf, int size, volatile bool* stop);
int64_t nf_seek(NetFile* f, int64_t pos);   // returns the new position, or -1 if out of range
int64_t nf_tell(NetFile* f);
int64_t nf_size(NetFile* f);
bool nf_error(NetFile* f, char* out, size_t len);
void nf_close(NetFile* f);

typedef struct {
	char name[256];
	bool dir;
	long long size;
} NfEntry;

// Lists up to max entries of a folder, leaving out "." and "..". For "smb://<server>/" on a
// server with no share set, it lists the shares instead. Returns -1 on failure, with the
// reason in err.
int nf_list(const char* url, NfEntry* out, int max, char* err, size_t errlen);

// Tests a server that is being added by logging in and listing where it starts. True if
// that works.
bool nf_test_server(const char* url, char* err, size_t errlen);

// Fetches the key fingerprint (SHA-256, in hex) an SFTP server presents right now, without
// trusting it yet. It's shown to the user when the place is added, and if they accept,
// servers_set_hostkey pins it.
bool nf_sftp_fingerprint(const char* name, char* fp, size_t fplen, char* err, size_t errlen);
