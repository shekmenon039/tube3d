#include "servers.h"
#include "app.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SERVERS_FILE
#define SERVERS_FILE "sdmc:/3ds/Tube3D/servers.txt"
#endif

static Server list[MAX_SERVERS];
static int count = 0;
// The main thread edits the list, but the connecting threads also read it (servers_lookup)
// and write to it (servers_set_hostkey). So every access holds this lock, and so does
// saving, since two threads writing servers.txt at once would garble it. It's recursive
// because the functions that change the list also save it.
static RecursiveLock lock;
static bool lock_ready = false;
static void lock_list(void)
{
	if (!lock_ready)
	{
		RecursiveLock_Init(&lock);
		lock_ready = true;
	}   // the first call is servers_load on the main thread
	RecursiveLock_Lock(&lock);
}
static void unlock_list(void) { RecursiveLock_Unlock(&lock); }

// The file has one server per line with tab-separated fields, in this order:
//   kind name host port user pass start hostkey
// Tabs, newlines and backslashes inside a field are escaped as \t, \n and \\.
static void put_field(FILE* f, const char* s, bool last)
{
	for (; *s; s++)
	{
		if (*s == '\t') fputs("\\t", f);
		else if (*s == '\n') fputs("\\n", f);
		else if (*s == '\\') fputs("\\\\", f);
		else fputc(*s, f);
	}
	fputc(last ? '\n' : '\t', f);
}

// Copies the next field of *line into out, unescaping it, and moves *line past it.
static void get_field(char** line, char* out, size_t size)
{
	size_t len = 0;
	char* cursor = *line;
	while (*cursor && *cursor != '\t' && *cursor != '\n')
	{
		char c = *cursor++;
		if (c == '\\' && *cursor)
		{
			c = *cursor == 't' ? '\t' : *cursor == 'n' ? '\n' : *cursor;
			cursor++;
		}
		if (len + 1 < size) out[len++] = c;
	}
	out[len] = 0;
	if (*cursor == '\t') cursor++;
	*line = cursor;
}

void servers_load(void)
{
	lock_list();
	count = 0;
	FILE* f = fopen(SERVERS_FILE, "r");
	if (!f)
	{
		unlock_list();
		return;
	}
	char line[1024];
	while (count < MAX_SERVERS && fgets(line, sizeof(line), f))
	{
		char* cursor = line;
		char kind[8], port[8];
		Server s;
		memset(&s, 0, sizeof(s));
		get_field(&cursor, kind, sizeof(kind));
		if (strcmp(kind, "smb") != 0 && strcmp(kind, "sftp") != 0) continue;
		s.kind = !strcmp(kind, "smb") ? SRV_SMB : SRV_SFTP;
		get_field(&cursor, s.name, sizeof(s.name));
		get_field(&cursor, s.host, sizeof(s.host));
		get_field(&cursor, port, sizeof(port));
		s.port = (int)strtol(port, NULL, 10);
		get_field(&cursor, s.user, sizeof(s.user));
		get_field(&cursor, s.pass, sizeof(s.pass));
		get_field(&cursor, s.start, sizeof(s.start));
		get_field(&cursor, s.hostkey, sizeof(s.hostkey));
		if (s.name[0] && s.host[0]) list[count++] = s;
	}
	fclose(f);
	unlock_list();
}

static bool servers_save(void)
{
	lock_list();
	FILE* f = fopen(SERVERS_FILE, "w");
	if (!f)
	{
		unlock_list();
		return false;
	}
	for (int i = 0; i < count; i++)
	{
		const Server* s = &list[i];
		char port[8];
		snprintf(port, sizeof(port), "%d", s->port);
		put_field(f, s->kind == SRV_SMB ? "smb" : "sftp", false);
		put_field(f, s->name, false);
		put_field(f, s->host, false);
		put_field(f, port, false);
		put_field(f, s->user, false);
		put_field(f, s->pass, false);
		put_field(f, s->start, false);
		put_field(f, s->hostkey, true);
	}
	const bool ok = fclose(f) == 0;
	unlock_list();
	return ok;
}

int servers_count(void) { return count; }
const Server* servers_get(int i) { return i >= 0 && i < count ? &list[i] : NULL; }

// The caller holds the lock. NULL if there's no such server.
static Server* find(SrvKind kind, const char* name)
{
	for (int i = 0; i < count; i++)
		if (list[i].kind == kind && !strcmp(list[i].name, name)) return &list[i];
	return NULL;
}

bool servers_lookup(SrvKind kind, const char* name, Server* out)
{
	lock_list();
	const Server* s = find(kind, name);
	if (s) *out = *s;
	unlock_list();
	return s != NULL;
}

bool servers_add(const Server* s)
{
	lock_list();
	bool ok = false;
	Server* slot = find(s->kind, s->name);
	if (!slot && count < MAX_SERVERS) slot = &list[count++];
	if (slot)
	{
		*slot = *s;
		ok = servers_save();
	}
	unlock_list();
	return ok;
}

void servers_remove(SrvKind kind, const char* name)
{
	lock_list();
	Server* s = find(kind, name);
	if (s)
	{
		const int i = (int)(s - list);
		memmove(&list[i], &list[i + 1], sizeof(Server) * (size_t)(count - i - 1));
		count--;
		servers_save();
	}
	unlock_list();
}

bool servers_set_hostkey(const char* name, const char* fingerprint)
{
	bool ok = false;
	lock_list();
	Server* s = find(SRV_SFTP, name);
	if (s)
	{
		str_copy(s->hostkey, sizeof(s->hostkey), fingerprint);
		ok = servers_save();
	}
	unlock_list();
	return ok;
}

bool servers_parse_url(const char* url, SrvKind* kind, char* name, size_t name_size, const char** rest)
{
	const char* server;   // just past the scheme
	if (!strncmp(url, "smb://", 6))
	{
		*kind = SRV_SMB;
		server = url + 6;
	}
	else if (!strncmp(url, "sftp://", 7))
	{
		*kind = SRV_SFTP;
		server = url + 7;
	}
	else return false;
	const char* slash = strchr(server, '/');
	size_t len = slash ? (size_t)(slash - server) : strlen(server);
	if (len == 0 || len >= name_size) return false;
	memcpy(name, server, len);
	name[len] = 0;
	*rest = slash ? slash + 1 : server + len;
	return true;
}
