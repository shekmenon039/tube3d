#pragma once
// The network places in the Videos tab: SMB shares and SFTP servers the user has added. They
// are kept one per line in sdmc:/3ds/Tube3D/servers.txt, passwords included in plain text.
// The SD card is not safe, so users should set up an account just for this. URLs refer to a
// server by its name and never contain credentials:
//   smb://<name>/<share>/<path>      sftp://<name>/<path>
#include <stdbool.h>
#include <stddef.h>

typedef enum { SRV_SMB, SRV_SFTP } SrvKind;

#define MAX_SERVERS 16

typedef struct {
	SrvKind kind;
	char name[32];   // shown in the list, and used as the host part of URLs
	char host[96];   // the computer's address or name
	int port;        // 0 means the protocol's default (445 or 22)
	char user[64];
	char pass[64];
	char start[128];     // SMB: the share (empty lists the shares). SFTP: the folder to start in
	char hostkey[100];   // SFTP: the server's key fingerprint, pinned on the first connection
} Server;

void servers_load(void);
int servers_count(void);
const Server* servers_get(int i);
// Copies a server out for the threads that connect. The main thread may edit the list while
// they run, and a pointer into it could end up pointing at a different server. Returns false
// if there's no such server.
bool servers_lookup(SrvKind kind, const char* name, Server* out);
bool servers_add(const Server* s);   // replaces an existing one of the same kind and name
void servers_remove(SrvKind kind, const char* name);
bool servers_set_hostkey(const char* name, const char* fingerprint);   // SFTP only

// Splits "smb://name/rest" into the kind, the name and the rest ("" or "share/dir/file").
// Returns false if the URL isn't one of ours.
bool servers_parse_url(const char* url, SrvKind* kind, char* name, size_t name_size, const char** rest);
