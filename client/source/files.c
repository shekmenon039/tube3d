#include "files.h"
#include "app.h"
#include "tlog.h"
#include "netfile.h"
#include "servers.h"
#include "resume.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

// Whether the player should try this file, judged by its extension. It opens Matroska and
// MP4 files with H.264 video.
static bool files_is_video(const char* name)
{
	static const char* exts[] = { ".mkv", ".mk3d", ".webm", ".mp4", ".m4v", ".mov" };
	const char* dot = strrchr(name, '.');
	if (!dot) return false;
	for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++)
		if (!strcasecmp(dot, exts[i])) return true;
	return false;
}

bool files_is_network(const char* path) { return !strncmp(path, "smb://", 6) || !strncmp(path, "sftp://", 7); }

void files_parent(const char* dir, char* out, int size)
{
	if (files_is_network(dir))
	{
		// "smb://srv/a/b/" goes up to "smb://srv/a/", and a server's top ("smb://srv/") goes
		// back to the server list ("smb:").
		SrvKind kind;
		char name[32];
		const char* rest;
		str_copy(out, (size_t)size, dir);
		size_t len = strlen(out);
		if (len && out[len - 1] == '/') out[--len] = 0;
		if (!servers_parse_url(dir, &kind, name, sizeof(name), &rest) || !rest[0])
		{
			str_copy(out, (size_t)size, !strncmp(dir, "smb", 3) ? "smb:" : "sftp:");
			return;
		}
		char* slash = strrchr(out, '/');
		if (slash) slash[1] = 0;
		return;
	}
	if (!strcmp(dir, "smb:") || !strcmp(dir, "sftp:") || !dir[0] || !strcmp(dir, FILES_SD_ROOT) ||
	    strncmp(dir, FILES_SD_ROOT, strlen(FILES_SD_ROOT)) != 0)
	{
		out[0] = 0;   // the sources
		return;
	}
	str_copy(out, (size_t)size, dir);
	char* slash = strrchr(out, '/');
	if (slash && slash > out + strlen(FILES_SD_ROOT) - 1) *slash = 0;
	else str_copy(out, (size_t)size, FILES_SD_ROOT);
}

#define SEPARATOR     " \xE2\x80\xBA "   // " › " between the parts of a location
#define SEPARATOR_LEN (sizeof(SEPARATOR) - 1)

void files_location(const char* dir, char* out, size_t size)
{
	if (!dir[0])
	{
		str_copy(out, size, "Videos");
		return;
	}
	if (!strcmp(dir, "smb:"))
	{
		str_copy(out, size, "SMB shares");
		return;
	}
	if (!strcmp(dir, "sftp:"))
	{
		str_copy(out, size, "SFTP servers");
		return;
	}
	char label[300];
	const char* part;
	if (files_is_network(dir))
	{
		const bool smb = !strncmp(dir, "smb", 3);
		snprintf(label, sizeof(label), smb ? "SMB" : "SFTP");
		part = dir + (smb ? 6 : 7);   // skip the scheme, leaving "<server>/<share>/..."
	}
	else
	{
		snprintf(label, sizeof(label), "SD card");
		part = dir + strlen(FILES_SD_ROOT);
	}
	while (*part)
	{
		const char* slash = strchr(part, '/');
		size_t len = slash ? (size_t)(slash - part) : strlen(part);
		size_t end = strlen(label);
		if (len && end + len + SEPARATOR_LEN + 1 < sizeof(label))
		{
			memcpy(label + end, SEPARATOR, SEPARATOR_LEN);
			memcpy(label + end + SEPARATOR_LEN, part, len);
			label[end + SEPARATOR_LEN + len] = 0;
		}
		part += len;
		if (*part == '/') part++;
	}
	str_copy(out, size, label);
}

static void human_size(long long bytes, char* out, size_t len)
{
	if (bytes >= 1024LL * 1024 * 1024) snprintf(out, len, "%.1f GB", (double)bytes / (1024.0 * 1024 * 1024));
	else if (bytes >= 1024 * 1024) snprintf(out, len, "%.0f MB", (double)bytes / (1024.0 * 1024));
	else snprintf(out, len, "%lld KB", bytes / 1024);
}

// A file's row text: its size and extension, e.g. "1.2 GB · mkv".
static void file_info(long long bytes, const char* name, char* out, size_t len)
{
	char size_text[24];
	human_size(bytes, size_text, sizeof(size_text));
	const char* dot = strrchr(name, '.');
	snprintf(out, len, "%s  \xC2\xB7  %s", size_text, dot ? dot + 1 : "");
}

static int by_kind_then_name(const void* a, const void* b)
{
	const VideoEntry* x = (const VideoEntry*)a;
	const VideoEntry* y = (const VideoEntry*)b;
	if (x->folder != y->folder) return x->folder ? -1 : 1;
	return strcasecmp(x->title, y->title);
}

static void files_scan_sd(const char* dir);

static VideoEntry* add(const char* title, const char* info, const char* path, bool folder)
{
	if (app.nfiles >= MAX_FILES) return NULL;
	VideoEntry* entry = &app.files[app.nfiles++];
	memset(entry, 0, sizeof(*entry));
	entry->folder = folder;
	str_copy(entry->title, sizeof(entry->title), title);
	str_copy(entry->channel, sizeof(entry->channel), info);
	str_copy(entry->path, sizeof(entry->path), path);
	return entry;
}

void files_scan(const char* dir)
{
	str_copy(app.files_dir, sizeof(app.files_dir), dir);
	app.nfiles = 0;
	if (!dir[0])
	{
		add("SD card", "This 3DS", FILES_SD_ROOT, true);
		add("SMB share", "A Windows or NAS shared folder", "smb:", true);
		add("SFTP server", "An SSH server", "sftp:", true);
		return;
	}
	if (!strcmp(dir, "smb:") || !strcmp(dir, "sftp:"))
	{
		// The servers added so far, followed by an entry for adding another.
		const SrvKind kind = dir[1] == 'm' ? SRV_SMB : SRV_SFTP;
		servers_load();
		for (int i = 0; i < servers_count(); i++)
		{
			const Server* s = servers_get(i);
			if (s->kind != kind) continue;
			// As in a file manager, an SMB place opens on its list of shares and an SFTP place in
			// the user's home folder ("~", where Up leads on to "/"). From there you can go anywhere.
			char url[200], info[128];
			snprintf(url, sizeof(url), kind == SRV_SMB ? "smb://%s/" : "sftp://%s/~/", s->name);
			snprintf(info, sizeof(info), "%.60s", s->host);
			add(s->name, info, url, true);
		}
		add(kind == SRV_SMB ? "Add an SMB share" : "Add an SFTP server", "", kind == SRV_SMB ? "smb:+" : "sftp:+",
		    true);
		return;
	}
	if (files_is_network(dir))
	{
		char err[160] = "";
		const int count = files_net_fetch(dir, err, sizeof(err));
		files_net_apply(dir, count, err);
		return;
	}
	if (strncmp(dir, FILES_SD_ROOT, strlen(FILES_SD_ROOT)) != 0) return;
	files_scan_sd(dir);
}

// The entries of a network folder as read from the server (main.c reads on a thread).
static NfEntry net_entries[MAX_FILES];
int files_net_fetch(const char* dir, char* err, size_t errlen)
{
	return nf_list(dir, net_entries, MAX_FILES, err, errlen);
}

void files_net_apply(const char* dir, int count, const char* err)
{
	str_copy(app.files_dir, sizeof(app.files_dir), dir);
	app.nfiles = 0;
	if (count < 0)
	{
		snprintf(app.status, sizeof(app.status), "%.150s", err);
		tlog("files: %s: %s", dir, err);
		return;
	}
	// As in a file manager: folders first, then every file. Files that aren't videos are
	// listed greyed out.
	for (int i = 0; i < count && app.nfiles < MAX_FILES; i++)
	{
		const NfEntry* entry = &net_entries[i];
		if (entry->name[0] == '.') continue;   // hide dot files, as file managers do
		size_t name_len = strlen(entry->name);
		if (entry->dir && name_len && entry->name[name_len - 1] == '$') continue;   // hidden shares (IPC$, C$...)
		char path[sizeof(app.files[0].path)];
		if (snprintf(path, sizeof(path), "%s%s%s", dir, entry->name, entry->dir ? "/" : "") >= (int)sizeof(path))
			continue;
		char info[64] = "Folder";
		if (!entry->dir) file_info(entry->size, entry->name, info, sizeof(info));
		VideoEntry* added = add(entry->name, info, path, entry->dir);
		if (added) added->other = !entry->dir && !files_is_video(entry->name);
	}
	qsort(app.files, (size_t)app.nfiles, sizeof(VideoEntry), by_kind_then_name);
	files_update_resume();
	tlog("files: %s: %d entries", dir, app.nfiles);
}

static void files_scan_sd(const char* dir)
{
	mkdir("sdmc:/videos", 0777);   // the folder the on-screen hint tells users to put videos in
	DIR* listing = opendir(dir);
	if (!listing)
	{
		tlog("files: can't open %s", dir);
		return;
	}
	const bool at_root = !strcmp(dir, FILES_SD_ROOT);
	struct dirent* entry;
	while ((entry = readdir(listing)) && app.nfiles < MAX_FILES)
	{
		const char* name = entry->d_name;
		if (name[0] == '.') continue;
		if (at_root && (!strcasecmp(name, "Nintendo 3DS") || !strcasecmp(name, "luma"))) continue;   // system folders
		char path[sizeof(app.files[0].path)];
		if (snprintf(path, sizeof(path), "%s%s%s", dir, at_root ? "" : "/", name) >= (int)sizeof(path)) continue;
		struct stat st;
		if (stat(path, &st) != 0) continue;
		bool folder = S_ISDIR(st.st_mode);
		char info[64] = "Folder";
		if (!folder) file_info((long long)st.st_size, name, info, sizeof(info));
		VideoEntry* added = add(name, info, path, folder);
		if (added) added->other = !folder && !files_is_video(name);
	}
	closedir(listing);
	qsort(app.files, (size_t)app.nfiles, sizeof(VideoEntry), by_kind_then_name);
	files_update_resume();
	tlog("files: %s: %d entries", dir, app.nfiles);
}

void files_update_resume(void)
{
	for (int i = 0; i < app.nfiles; i++)
	{
		VideoEntry* entry = &app.files[i];
		ResumeInfo info;
		const bool has_point = !entry->folder && !entry->other && resume_get(entry->path, &info) && info.pos_ms;
		entry->resume_ms = has_point ? info.pos_ms : 0;
		entry->resume_dur = has_point ? info.dur_ms : 0;
	}
}
