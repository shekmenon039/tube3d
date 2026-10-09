#include "netfile.h"
#include "servers.h"
#include "tlog.h"

#include <3ds.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-share-enum.h>
#include <smb2/libsmb2-raw.h>
#include <libssh2.h>
#include <libssh2_sftp.h>

#define CHUNK_BYTES        (128 * 1024)   // size of one network read
#define NET_TIMEOUT_S      15
#define CONNECT_TIMEOUT_MS 8000   // how long to wait for an SFTP server that doesn't answer
#define SHA256_BYTES       32     // a host key fingerprint, before it's written as hex
#define FETCH_STACK_BYTES  (64 * 1024)
#define FETCH_IDLE_NS      (5 * 1000000LL)   // the reader thread's nap when the window is full or done
#define READ_WAIT_NS       (2 * 1000000LL)   // nf_read's nap while it waits for data
#define SHARE_TYPE_MASK    3                 // the share-type bits of an SMB share's type (disk, printer, ...)

// ---------------------------------------------------------------- connections

typedef struct {
	SrvKind kind;
	char server[32];
	// SMB
	struct smb2_context* smb2;
	struct smb2fh* fh;
	// SFTP
	int sock;
	LIBSSH2_SESSION* ssh;
	LIBSSH2_SFTP* sftp;
	LIBSSH2_SFTP_HANDLE* sh;
	int64_t sftp_pos;   // where the handle reads next; we only seek when needed (see conn_pread)
} Conn;

static void conn_close(Conn* c)
{
	if (c->smb2)
	{
		if (c->fh) smb2_close(c->smb2, c->fh);
		smb2_disconnect_share(c->smb2);
		smb2_destroy_context(c->smb2);
	}
	// For SFTP we skip the polite goodbye: its three round trips added about 0.3 s to leaving a
	// video. Shutting the socket first makes libssh2's close calls fail at once, so they only
	// free memory.
	if (c->ssh && c->sock >= 0) shutdown(c->sock, SHUT_RDWR);
	if (c->sh) libssh2_sftp_close(c->sh);
	if (c->sftp) libssh2_sftp_shutdown(c->sftp);
	if (c->ssh)
	{
		libssh2_session_disconnect(c->ssh, "bye");
		libssh2_session_free(c->ssh);
	}
	if (c->sock >= 0) close(c->sock);
	memset(c, 0, sizeof(*c));
	c->sock = -1;
}

// For SMB, splits "share/dir/file" into the share and the path within it.
static void split_share(const char* rest, char* share, size_t share_size, const char** path)
{
	const char* slash = strchr(rest, '/');
	size_t len = slash ? (size_t)(slash - rest) : strlen(rest);
	if (len >= share_size) len = share_size - 1;
	memcpy(share, rest, len);
	share[len] = 0;
	*path = slash ? slash + 1 : rest + len;
}

static bool smb_connect(Conn* c, const Server* s, const char* share, char* err, size_t errlen)
{
	c->smb2 = smb2_init_context();
	if (!c->smb2)
	{
		snprintf(err, errlen, "Out of memory for SMB.");
		return false;
	}
	smb2_set_security_mode(c->smb2, SMB2_NEGOTIATE_SIGNING_ENABLED);
	smb2_set_timeout(c->smb2, NET_TIMEOUT_S);
	if (s->user[0]) smb2_set_user(c->smb2, s->user);
	smb2_set_password(c->smb2, s->pass);
	char host[110];
	if (s->port) snprintf(host, sizeof(host), "%s:%d", s->host, s->port);
	else snprintf(host, sizeof(host), "%s", s->host);
	if (smb2_connect_share(c->smb2, host, share, s->user[0] ? s->user : NULL) < 0)
	{
		snprintf(err, errlen, "%s", smb2_get_error(c->smb2));
		tlog("netfile: smb %s/%s: %s", s->host, share, err);
		return false;
	}
	return true;
}

// Opens the TCP connection for SFTP.
static int tcp_connect(const char* host, int port, char* err, size_t errlen)
{
	char port_str[8];
	snprintf(port_str, sizeof(port_str), "%d", port);
	struct addrinfo hints, *res = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
	{
		snprintf(err, errlen, "Can't find %s on the network.", host);
		return -1;
	}
	// Connect without blocking, so a host that never answers costs CONNECT_TIMEOUT_MS rather
	// than minutes; a blocking connect to a missing server freezes the app. To tell whether we
	// connected we ask getpeername, because the 3DS's SO_ERROR still says "in progress" after
	// poll reports the socket writable (the same quirk tools/libsmb2-3ds-connect.patch handles).
	int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (sock >= 0)
	{
		const int flags = fcntl(sock, F_GETFL, 0);
		fcntl(sock, F_SETFL, flags | O_NONBLOCK);
		bool connected = connect(sock, res->ai_addr, res->ai_addrlen) == 0;
		if (!connected && (errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EALREADY))
		{
			struct pollfd pfd = { .fd = sock, .events = POLLOUT };
			if (poll(&pfd, 1, CONNECT_TIMEOUT_MS) > 0 && !(pfd.revents & (POLLERR | POLLHUP | POLLNVAL)))
			{
				struct sockaddr_in peer;
				socklen_t peer_len = sizeof(peer);
				connected = getpeername(sock, (struct sockaddr*)&peer, &peer_len) == 0;
			}
		}
		fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);   // libssh2 wants a blocking socket and has its own timeout
		if (!connected)
		{
			close(sock);
			sock = -1;
		}
	}
	freeaddrinfo(res);
	if (sock < 0) snprintf(err, errlen, "%s doesn't answer on port %d.", host, port);
	return sock;
}

// Keyboard-interactive login, for servers that insist on it. We only answer a prompt with the
// password if it's hidden (no echo) and actually asks for a password, never a one-time code
// or anything else. The password is per thread because two threads can log in at once.
static __thread const char* g_kbd_pass = "";
static void kbd_cb(const char* name, int name_len, const char* instruction, int instruction_len, int num_prompts,
                   const LIBSSH2_USERAUTH_KBDINT_PROMPT* prompts, LIBSSH2_USERAUTH_KBDINT_RESPONSE* responses,
                   void** abstract)
{
	(void)name;
	(void)name_len;
	(void)instruction;
	(void)instruction_len;
	(void)abstract;
	for (int i = 0; i < num_prompts; i++)
	{
		bool asks_password = !prompts[i].echo && prompts[i].text && prompts[i].length < 200;
		if (asks_password)
		{
			char lower[200];
			for (unsigned j = 0; j < prompts[i].length; j++) lower[j] = (char)tolower(prompts[i].text[j]);
			lower[prompts[i].length] = 0;
			asks_password = strstr(lower, "password") != NULL;
		}
		const char* answer = asks_password ? g_kbd_pass : "";
		responses[i].text = strdup(answer);
		responses[i].length = responses[i].text ? (unsigned int)strlen(answer) : 0;
	}
}

// Connects to an SFTP server and does the SSH handshake, putting the server key's SHA-256
// fingerprint (hex) in fp. Fails with err set if anything goes wrong, including a server that
// offers no fingerprint: we never trust one without it.
static bool ssh_handshake(Conn* c, const Server* s, char* fp, size_t fplen, char* err, size_t errlen)
{
	static bool ssh_ready = false;
	if (!ssh_ready)
	{
		libssh2_init(0);
		ssh_ready = true;
	}
	c->sock = tcp_connect(s->host, s->port ? s->port : 22, err, errlen);
	if (c->sock < 0) return false;
	c->ssh = libssh2_session_init();
	if (!c->ssh)
	{
		snprintf(err, errlen, "Out of memory for SFTP.");
		return false;
	}
	libssh2_session_set_blocking(c->ssh, 1);
	libssh2_session_set_timeout(c->ssh, NET_TIMEOUT_S * 1000L);
	if (libssh2_session_handshake(c->ssh, c->sock) != 0)
	{
		snprintf(err, errlen, "The SSH handshake with %s failed.", s->host);
		return false;
	}
	const unsigned char* hash = (const unsigned char*)libssh2_hostkey_hash(c->ssh, LIBSSH2_HOSTKEY_HASH_SHA256);
	if (!hash || fplen < SHA256_BYTES * 2 + 1)
	{
		snprintf(err, errlen, "%s didn't show its key: refused.", s->host);
		return false;
	}
	for (int i = 0; i < SHA256_BYTES; i++) snprintf(fp + i * 2, 3, "%02x", hash[i]);
	return true;
}

static bool sftp_connect(Conn* c, const Server* s, char* err, size_t errlen)
{
	// The user confirmed the server's key when adding it (a place added some other way gets
	// its key pinned on the first connection). Any different key after that is refused.
	char fp[80];
	if (!ssh_handshake(c, s, fp, sizeof(fp), err, errlen)) return false;
	if (s->hostkey[0] && strcmp(s->hostkey, fp) != 0)
	{
		snprintf(err, errlen,
		         "%s's key changed since it was added: refused (remove and add the server again "
		         "if that was expected).",
		         s->name);
		tlog("netfile: sftp %s: host key %s, expected %s", s->host, fp, s->hostkey);
		return false;
	}
	if (!s->hostkey[0]) servers_set_hostkey(s->name, fp);
	int rc = libssh2_userauth_password(c->ssh, s->user, s->pass);
	if (rc != 0)
	{
		g_kbd_pass = s->pass;
		rc = libssh2_userauth_keyboard_interactive(c->ssh, s->user, kbd_cb);
		g_kbd_pass = "";   // don't keep pointing into the caller's copy of the server
	}
	if (rc != 0)
	{
		snprintf(err, errlen, "%s refused the user name or password.", s->host);
		return false;
	}
	c->sftp = libssh2_sftp_init(c->ssh);
	if (!c->sftp)
	{
		snprintf(err, errlen, "%s doesn't offer SFTP.", s->host);
		return false;
	}
	return true;
}

// Connects to the server named in url (and, for SMB, to the share) and points *path at the
// path on that server.
static bool conn_open(Conn* c, const char* url, const char** path, char* err, size_t errlen)
{
	memset(c, 0, sizeof(*c));
	c->sock = -1;
	const char* rest;
	if (!servers_parse_url(url, &c->kind, c->server, sizeof(c->server), &rest))
	{
		snprintf(err, errlen, "Not a network address.");
		return false;
	}
	Server s;   // a copy, since the list may change while we connect
	if (!servers_lookup(c->kind, c->server, &s))
	{
		snprintf(err, errlen, "The server \"%s\" isn't in the list any more.", c->server);
		return false;
	}
	if (c->kind == SRV_SMB)
	{
		char share[132];
		split_share(rest, share, sizeof(share), path);
		return smb_connect(c, &s, share[0] ? share : "IPC$", err, errlen);
	}
	*path = rest;
	return sftp_connect(c, &s, err, errlen);
}

// Turns a URL path into the path to give libssh2.
static void sftp_path(const char* path, char* out, size_t size)
{
	// URL paths are absolute ("sftp://srv/" is "/"), except when they start with "~", which
	// means the home folder. libssh2 treats a path without a leading '/' as relative to home.
	if (path[0] == '~' && (path[1] == 0 || path[1] == '/'))
		snprintf(out, size, "%s", path[1] && path[2] ? path + 2 : ".");
	else snprintf(out, size, "/%s", path);
}

static bool conn_open_file(Conn* c, const char* path, int64_t* size, char* err, size_t errlen)
{
	if (c->kind == SRV_SMB)
	{
		c->fh = smb2_open(c->smb2, path, O_RDONLY);
		// The open reply already carried the size, so seeking to the end costs no round trip.
		// Fall back to fstat if that gives nothing.
		int64_t end = c->fh ? smb2_lseek(c->smb2, c->fh, 0, SEEK_END, NULL) : -1;
		if (end > 0)
		{
			smb2_lseek(c->smb2, c->fh, 0, SEEK_SET, NULL);
			*size = end;
			return true;
		}
		struct smb2_stat_64 st;
		int rc = c->fh ? smb2_fstat(c->smb2, c->fh, &st) : -1;
		if (rc < 0)
		{
			const char* smb_err = smb2_get_error(c->smb2);
			snprintf(err, errlen, "%s (%s)", smb_err && smb_err[0] ? smb_err : "the file couldn't be opened",
			         c->fh ? "stat" : "open");
			tlog("netfile: smb open %s: %s", path, err);
			return false;
		}
		*size = (int64_t)st.smb2_size;
		return true;
	}
	char remote_path[400];
	sftp_path(path, remote_path, sizeof(remote_path));
	c->sh = libssh2_sftp_open(c->sftp, remote_path, LIBSSH2_FXF_READ, 0);
	c->sftp_pos = 0;
	LIBSSH2_SFTP_ATTRIBUTES attrs;
	if (!c->sh || libssh2_sftp_fstat_ex(c->sh, &attrs, 0) != 0)
	{
		snprintf(err, errlen, "The file couldn't be opened on the server.");
		return false;
	}
	*size = (int64_t)attrs.filesize;
	return true;
}

// Reads up to n bytes at off. Returns the count read, 0 at the end of the file, or < 0 on error.
static int conn_pread(Conn* c, uint8_t* buf, uint32_t n, int64_t off)
{
	if (c->kind == SRV_SMB)
	{
		uint32_t max = smb2_get_max_read_size(c->smb2);
		if (max && n > max) n = max;
		return smb2_pread(c->smb2, c->fh, buf, n, (uint64_t)off);
	}
	// Only seek when the offset actually jumps. libssh2 pipelines sequential reads and a seek
	// throws that read-ahead away; seeking before every chunk made SFTP too slow for even a
	// 2.4 Mbit/s video on the console.
	if (off != c->sftp_pos) libssh2_sftp_seek64(c->sh, (libssh2_uint64_t)off);
	c->sftp_pos = -1;   // unknown until the reads below succeed
	uint32_t got = 0;
	while (got < n)
	{
		ssize_t read_now = libssh2_sftp_read(c->sh, (char*)buf + got, n - got);
		if (read_now < 0) return got ? (int)got : -1;
		if (read_now == 0) break;
		got += (uint32_t)read_now;
	}
	c->sftp_pos = off + got;
	return (int)got;
}

// ---------------------------------------------------------------- the read-ahead file

struct NetFile {
	char url[400];
	Conn conn;
	LightLock lock;
	Thread thread;
	volatile bool quit;
	uint8_t* buf;
	uint32_t cap;
	int64_t base, fill;   // buf holds the file's bytes [base, base + fill)
	int64_t pos, size;
	uint32_t gen;   // bumped by a seek outside the window, so a read in flight is discarded
	bool eof;
	char error[160];
};

static void fetch_main(void* arg)
{
	NetFile* f = (NetFile*)arg;
	uint8_t* chunk = (uint8_t*)malloc(CHUNK_BYTES);
	int retries = 0;
	LightLock_Lock(&f->lock);
	while (!f->quit && chunk)
	{
		// Discard old data, keeping an eighth of the window behind the reader.
		int64_t keep_from = f->pos - f->cap / 8;
		if (keep_from > f->base && f->pos - f->base > (int64_t)f->cap / 2)
		{
			int64_t drop = keep_from - f->base;
			if (drop > f->fill) drop = f->fill;
			memmove(f->buf, f->buf + drop, (size_t)(f->fill - drop));
			f->base += drop;
			f->fill -= drop;
		}
		const int64_t off = f->base + f->fill;
		if (f->error[0] || off >= f->size || f->fill >= f->cap)
		{
			f->eof = off >= f->size;
			LightLock_Unlock(&f->lock);
			svcSleepThread(FETCH_IDLE_NS);
			LightLock_Lock(&f->lock);
			continue;
		}
		uint32_t want = CHUNK_BYTES;
		if ((int64_t)want > (int64_t)f->cap - f->fill) want = (uint32_t)(f->cap - f->fill);
		if ((int64_t)want > f->size - off) want = (uint32_t)(f->size - off);
		const uint32_t gen = f->gen;
		LightLock_Unlock(&f->lock);
		int got = conn_pread(&f->conn, chunk, want, off);
		if (got < 0 && !f->quit && retries++ < 2)
		{
			// The connection probably dropped. Reconnect and try once more.
			tlog("netfile: read at %lld failed, reconnecting", (long long)off);
			char err[160];
			const char* path;
			int64_t size;
			conn_close(&f->conn);
			if (conn_open(&f->conn, f->url, &path, err, sizeof(err)) &&
			    conn_open_file(&f->conn, path, &size, err, sizeof(err)))
				got = conn_pread(&f->conn, chunk, want, off);
		}
		LightLock_Lock(&f->lock);
		if (gen != f->gen) continue;   // a seek moved the window while we were reading
		if (got > 0)
		{
			retries = 0;
			memcpy(f->buf + f->fill, chunk, (size_t)got);
			f->fill += got;
		}
		else if (got == 0) f->size = off;   // the file is shorter than the server claimed
		else snprintf(f->error, sizeof(f->error), "The network connection to the server was lost.");
	}
	LightLock_Unlock(&f->lock);
	free(chunk);
}

NetFile* nf_open(const char* url, uint32_t window_bytes, char* err, size_t errlen)
{
	NetFile* f = (NetFile*)calloc(1, sizeof(NetFile));
	if (!f)
	{
		snprintf(err, errlen, "Out of memory.");
		return NULL;
	}
	snprintf(f->url, sizeof(f->url), "%s", url);
	LightLock_Init(&f->lock);
	f->cap = window_bytes;
	f->buf = (uint8_t*)malloc(window_bytes);
	const char* path;
	if (!f->buf || !conn_open(&f->conn, url, &path, err, errlen) ||
	    !conn_open_file(&f->conn, path, &f->size, err, errlen))
	{
		if (f->buf && !err[0]) snprintf(err, errlen, "Out of memory.");
		nf_close(f);
		return NULL;
	}
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	// Run on the New 3DS's extra core like the HTTP downloads (hstream.c). If that fails, run
	// at a lower priority than the playback loop.
	f->thread = threadCreate(fetch_main, f, FETCH_STACK_BYTES, prio - 1, 2, false);
	if (!f->thread) f->thread = threadCreate(fetch_main, f, FETCH_STACK_BYTES, prio + 1, -1, false);
	if (!f->thread)
	{
		snprintf(err, errlen, "Couldn't start the network thread.");
		nf_close(f);
		return NULL;
	}
	tlog("netfile: %s: %lld bytes", url, (long long)f->size);
	return f;
}

int nf_read(NetFile* f, uint8_t* buf, int size, volatile bool* stop)
{
	LightLock_Lock(&f->lock);
	while (f->pos >= f->base + f->fill)
	{
		if (f->pos >= f->size)
		{
			LightLock_Unlock(&f->lock);
			return 0;
		}
		if (f->error[0] || (stop && *stop))
		{
			LightLock_Unlock(&f->lock);
			return -1;
		}
		LightLock_Unlock(&f->lock);
		svcSleepThread(READ_WAIT_NS);
		LightLock_Lock(&f->lock);
	}
	int64_t avail = f->base + f->fill - f->pos;
	int count = avail < size ? (int)avail : size;
	memcpy(buf, f->buf + (f->pos - f->base), (size_t)count);
	f->pos += count;
	LightLock_Unlock(&f->lock);
	return count;
}

int64_t nf_seek(NetFile* f, int64_t pos)
{
	if (pos < 0 || pos > f->size) return -1;
	LightLock_Lock(&f->lock);
	if (pos < f->base || pos > f->base + f->fill)
	{
		// Outside the window, so start a new window here.
		f->base = pos;
		f->fill = 0;
		f->gen++;
	}
	f->pos = pos;
	LightLock_Unlock(&f->lock);
	return pos;
}

int64_t nf_tell(NetFile* f) { return f->pos; }
int64_t nf_size(NetFile* f) { return f->size; }

bool nf_error(NetFile* f, char* out, size_t len)
{
	LightLock_Lock(&f->lock);
	bool failed = f->error[0] != 0;
	if (failed && out) snprintf(out, len, "%s", f->error);
	LightLock_Unlock(&f->lock);
	return failed;
}

void nf_close(NetFile* f)
{
	if (!f) return;
	const u64 start_ms = osGetTime();
	f->quit = true;
	if (f->thread)
	{
		threadJoin(f->thread, U64_MAX);
		threadFree(f->thread);
	}
	const u64 joined_ms = osGetTime();
	conn_close(&f->conn);
	tlog("netfile: closed in %llu ms (reader %llu, connection %llu)", (unsigned long long)(osGetTime() - start_ms),
	     (unsigned long long)(joined_ms - start_ms), (unsigned long long)(osGetTime() - joined_ms));
	free(f->buf);
	free(f);
}

// ---------------------------------------------------------------- folders

// Cleans a name from a server's listing by turning control characters such as tabs and line
// breaks into spaces. Left alone, they would break the lines of resume.txt and the UI.
static void clean_name(char* s)
{
	for (; *s; s++)
		if ((unsigned char)*s < 0x20 || *s == 0x7F) *s = ' ';
}

int nf_list(const char* url, NfEntry* out, int max, char* err, size_t errlen)
{
	Conn c;
	const char* path;
	if (!conn_open(&c, url, &path, err, errlen))
	{
		conn_close(&c);
		return -1;
	}
	int count = 0;
	if (c.kind == SRV_SMB)
	{
		const char* rest;
		SrvKind kind;
		char name[32];
		servers_parse_url(url, &kind, name, sizeof(name), &rest);
		if (!rest[0])
		{
			// List the server's shares, through the IPC$ connection.
			struct smb2_share_enum_reply* shares = smb2_share_enum_sync(c.smb2, SMB2_SHARE_INFO_1);
			if (!shares)
			{
				snprintf(err, errlen, "%s", smb2_get_error(c.smb2));
				conn_close(&c);
				return -1;
			}
			for (uint32_t i = 0; i < shares->entries_read && count < max; i++)
			{
				const struct smb2_share_info_1* share = &shares->share_info.info_1[i];
				// only plain folder shares, no hidden ones like C$
				if ((share->type & SHARE_TYPE_MASK) != SMB2_SHARE_TYPE_DISKTREE ||
				    (share->type & SMB2_SHARE_TYPE_HIDDEN))
					continue;
				snprintf(out[count].name, sizeof(out[count].name), "%s", share->netname);
				clean_name(out[count].name);
				out[count].dir = true;
				out[count].size = 0;
				count++;
			}
			smb2_free_data(c.smb2, shares);
		}
		else
		{
			struct smb2dir* dir = smb2_opendir(c.smb2, path);
			if (!dir)
			{
				snprintf(err, errlen, "%s", smb2_get_error(c.smb2));
				conn_close(&c);
				return -1;
			}
			struct smb2dirent* entry;
			while ((entry = smb2_readdir(c.smb2, dir)) && count < max)
			{
				if (!strcmp(entry->name, ".") || !strcmp(entry->name, "..")) continue;
				snprintf(out[count].name, sizeof(out[count].name), "%s", entry->name);
				clean_name(out[count].name);
				out[count].dir = entry->st.smb2_type == SMB2_TYPE_DIRECTORY;
				out[count].size = (long long)entry->st.smb2_size;
				count++;
			}
			smb2_closedir(c.smb2, dir);
		}
	}
	else
	{
		char remote_path[400];
		sftp_path(path, remote_path, sizeof(remote_path));
		LIBSSH2_SFTP_HANDLE* dir = libssh2_sftp_opendir(c.sftp, remote_path);
		if (!dir)
		{
			snprintf(err, errlen, "The folder couldn't be opened on the server.");
			conn_close(&c);
			return -1;
		}
		char name[256];
		LIBSSH2_SFTP_ATTRIBUTES attrs;
		while (count < max && libssh2_sftp_readdir(dir, name, sizeof(name), &attrs) > 0)
		{
			if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
			snprintf(out[count].name, sizeof(out[count].name), "%s", name);
			clean_name(out[count].name);
			out[count].dir = (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && LIBSSH2_SFTP_S_ISDIR(attrs.permissions);
			out[count].size = (long long)attrs.filesize;
			count++;
		}
		libssh2_sftp_closedir(dir);
	}
	conn_close(&c);
	return count;
}

bool nf_test_server(const char* url, char* err, size_t errlen)
{
	static NfEntry probe[4];
	return nf_list(url, probe, 4, err, errlen) >= 0;
}

bool nf_sftp_fingerprint(const char* name, char* fp, size_t fplen, char* err, size_t errlen)
{
	Server s;
	if (!servers_lookup(SRV_SFTP, name, &s))
	{
		snprintf(err, errlen, "The server \"%s\" isn't in the list any more.", name);
		return false;
	}
	Conn c;
	memset(&c, 0, sizeof(c));
	c.sock = -1;
	c.kind = SRV_SFTP;
	const bool ok = ssh_handshake(&c, &s, fp, fplen, err, errlen);
	conn_close(&c);
	return ok;
}
