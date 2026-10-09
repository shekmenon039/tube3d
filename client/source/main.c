// Tube3D: YouTube with stereoscopic 3D on the New 3DS.
#include <3ds.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "app.h"
#include "direct_player.h"
#include "files.h"
#include "filethumb.h"
#include "netfile.h"
#include "servers.h"
#include "net.h"
#include "playerui.h"
#include "thumbs.h"
#include "ui.h"
#include "yt.h"
#include "exitguard.h"
#include "power.h"
#include "tlog.h"

// The menu only redraws when something has changed. Set this after anything that took over
// the screens (playback, the keyboard, a message) and when coming back from the HOME menu.
static volatile bool redraw_requested = true;

#define CONFIG_DIR   "sdmc:/3ds/Tube3D"
#define CONFIG_FILE  CONFIG_DIR "/config.txt"
#define HISTORY_FILE CONFIG_DIR "/history.txt"

// ---------------------------------------------------------------- config and history

// Parses a settings or history field as a whole decimal number, or returns fallback.
static int parse_int(const char* s, int fallback)
{
	char* end = NULL;
	long v = strtol(s, &end, 10);
	return end != s && *end == 0 && v >= INT_MIN && v <= INT_MAX ? (int)v : fallback;
}

static void load_config(void)
{
	FILE* f = fopen(CONFIG_FILE, "r");
	if (!f) return;
	char line[256];
	while (fgets(line, sizeof(line), f))
	{
		line[strcspn(line, "\r\n")] = 0;   // NOLINT(clang-analyzer-security.ArrayBound): strcspn stays in the string
		// Unknown lines, such as server= from the old PC-server mode, are ignored.
		if (!strncmp(line, "mode=", 5))
		{
			int mode = parse_int(line + 5, -1);
			if (mode >= 0 && mode < MODE_COUNT) app.mode = (PlayMode)mode;
		}
		else if (!strncmp(line, "menu3d=", 7))
		{
			int depth = parse_int(line + 7, -1);
			if (depth >= 0 && depth <= 3) app.menu_depth = depth;
		}
		else if (!strncmp(line, "subtitles=", 10)) app.subtitles = line[10] == '1';
		else if (!strncmp(line, "nerdstats=", 10)) app.nerd_stats = line[10] == '1';
		else if (!strncmp(line, "motion=", 7))
		{
			int motion = parse_int(line + 7, -1);
			if (motion >= 0 && motion < MOTION_COUNT) app.motion = motion;
		}
	}
	fclose(f);
}

static void save_config(void)
{
	mkdir("sdmc:/3ds", 0777);
	mkdir(CONFIG_DIR, 0777);
	FILE* f = fopen(CONFIG_FILE, "w");
	if (!f) return;
	fprintf(f, "mode=%d\nmenu3d=%d\nsubtitles=%d\nmotion=%d\nnerdstats=%d\n", (int)app.mode, app.menu_depth,
	        app.subtitles ? 1 : 0, app.motion, app.nerd_stats ? 1 : 0);
	fclose(f);
}

static int parse_entries(char* body, VideoEntry* out, int max)
{
	int count = 0;
	char* strtok_state = NULL;
	for (char* line = strtok_r(body, "\n", &strtok_state); line && count < max;
	     line = strtok_r(NULL, "\n", &strtok_state))
	{
		// Tab-separated: id, duration, channel, title and views. Older files lack the views.
		char* fields[5] = { 0 };
		char* cursor = line;
		for (int i = 0; i < 5 && cursor; i++)
		{
			fields[i] = cursor;
			cursor = (i < 4) ? strchr(cursor, '\t') : NULL;
			if (cursor) *cursor++ = 0;
		}
		if (!fields[0] || !fields[3] || !fields[0][0]) continue;
		VideoEntry* entry = &out[count++];
		str_copy(entry->id, sizeof(entry->id), fields[0]);
		entry->duration = parse_int(fields[1], 0);
		str_copy(entry->channel, sizeof(entry->channel), fields[2]);
		str_copy(entry->title, sizeof(entry->title), fields[3]);
		str_copy(entry->views, sizeof(entry->views), fields[4] ? fields[4] : "");
	}
	return count;
}

static void load_history(void)
{
	FILE* f = fopen(HISTORY_FILE, "r");
	if (!f) return;
	static char body[MAX_HISTORY * 400];
	size_t len = fread(body, 1, sizeof(body) - 1, f);
	body[len] = 0;
	fclose(f);
	app.nhistory = parse_entries(body, app.history, MAX_HISTORY);
}

static void add_history(const VideoEntry* e)
{
	int count = 1;
	VideoEntry merged[MAX_HISTORY];
	merged[0] = *e;
	for (int i = 0; i < app.nhistory && count < MAX_HISTORY; i++)
		if (strcmp(app.history[i].id, e->id) != 0) merged[count++] = app.history[i];
	memcpy(app.history, merged, sizeof(VideoEntry) * count);
	app.nhistory = count;
	app.sel[TAB_HISTORY] = 0;
	app.anim[TAB_HISTORY] = 0;

	mkdir(CONFIG_DIR, 0777);
	FILE* f = fopen(HISTORY_FILE, "w");
	if (!f) return;
	for (int i = 0; i < app.nhistory; i++)
		fprintf(f, "%s\t%d\t%s\t%s\t%s\n", app.history[i].id, app.history[i].duration, app.history[i].channel,
		        app.history[i].title, app.history[i].views);
	fclose(f);
}

// ---------------------------------------------------------------- background search

typedef struct {
	char query[160];   // the text to search for
	bool three_d;      // a 3D search (the 3D button or the 3D picks tab)
	Tab tab;
	volatile bool done;
	bool ok;                  // the search or page succeeded (error says why not)
	bool more;                // fetch the next page of tab's list (via cursor) rather than search anew
	volatile bool discard;    // the list was cleared while this ran, so throw the result away
	int want;                 // how many videos to fetch
	YtSearchCursor* cursor;   // for a new search, where to continue (NULL: no more pages);
	// for "more", the cursor being used
	VideoEntry found[MAX_RESULTS];
	int count;
	char error[128];
} SearchJob;

static SearchJob job;
static Thread search_thread = NULL;
static YtSearchCursor* cursors[TAB_COUNT];   // where each tab's list continues (NULL: no more pages)
static struct {
	bool on;
	char query[160];
	bool three_d;
	Tab tab;
} pending;   // a search waiting for the running job
static struct {
	char query[160];
	bool three_d;
} last_search;   // what the Search button runs again

static void search_main(void* arg)
{
	static YtItem items[MAX_RESULTS];
	int got = job.more
	              ? yt_search_more(job.cursor, items, job.want, job.error, sizeof(job.error))
	              : yt_search_paged(job.query, job.three_d, items, job.want, &job.cursor, job.error, sizeof(job.error));
	if (got >= 0)
	{
		for (int i = 0; i < got; i++)
		{
			VideoEntry* entry = &job.found[i];
			str_copy(entry->id, sizeof(entry->id), items[i].id);
			entry->duration = items[i].duration;
			str_copy(entry->channel, sizeof(entry->channel), items[i].channel);
			str_copy(entry->title, sizeof(entry->title), items[i].title);
			str_copy(entry->views, sizeof(entry->views), items[i].views);
		}
		job.count = got;
	}
	job.ok = got >= 0;
	__sync_synchronize();   // make the results visible before done is set
	job.done = true;
}

static void run_job(void)
{
	app.busy = true;
	// 64 KB of stack, because a search runs curl, mbedTLS and a JSON parser on this thread.
	search_thread = threadCreate(search_main, NULL, 64 * 1024, 0x30, -2, false);
	if (!search_thread)
	{
		app.busy = false;
		app.loading_more = false;
		snprintf(app.status, sizeof(app.status), "Couldn't start the search.");
	}
}

static void start_search(const char* query, bool three_d, Tab tab)
{
	snprintf(app.busy_text, sizeof(app.busy_text), tab == TAB_PICKS ? "Finding 3D videos..." : "Searching...");
	if (app.busy)
	{
		// A search or a next page is still running, so this search starts right after it. Only
		// the newest request waits; if the running search was for the same list, its result
		// is dropped when it finishes.
		pending.on = true;
		snprintf(pending.query, sizeof(pending.query), "%s", query);
		pending.three_d = three_d;
		pending.tab = tab;
		if (!job.more && job.tab == tab) job.discard = true;
		app.loading_more = false;   // show the overlay of the search that's waiting
		return;
	}
	yt_search_free(cursors[tab]);   // forget the old list's remaining pages
	cursors[tab] = NULL;
	memset(&job, 0, sizeof(job));
	job.three_d = three_d;                                 // yt_search_paged handles the 3D phrasing and filtering
	snprintf(job.query, sizeof(job.query), "%s", query);   // queries are at most 100 bytes, so this never truncates
	job.tab = tab;
	job.want = SEARCH_FIRST;
	app.status[0] = 0;
	run_job();
}

// Fetches the next page of a search list in the background when the user nears its end. The
// list stays usable meanwhile.
static void start_more(Tab tab)
{
	int count;
	app_list(tab, &count);
	if (app.busy || !cursors[tab] || count >= MAX_RESULTS) return;
	memset(&job, 0, sizeof(job));
	job.more = true;
	job.tab = tab;
	job.cursor = cursors[tab];
	job.want = MAX_RESULTS - count < SEARCH_FIRST ? MAX_RESULTS - count : SEARCH_FIRST;
	app.loading_more = true;
	run_job();
}

// Puts the finished job's result into its list, unless the job was discarded.
static void take_result(void)
{
	if (job.discard)
	{
		// The list was cleared or replaced by another search. Add nothing and stop paging it.
		if (!job.more) yt_search_free(job.cursor);
		else if (cursors[job.tab] == job.cursor)
		{
			yt_search_free(cursors[job.tab]);
			cursors[job.tab] = NULL;
		}
		return;
	}
	VideoEntry* dst = job.tab == TAB_PICKS ? app.picks : app.results;
	int* count = job.tab == TAB_PICKS ? &app.npicks : &app.nresults;
	if (job.more)
	{
		// Append the next page to its list. On an error, or when there are no more pages, the
		// list stays as it is and stops asking for more.
		int appended = job.ok ? job.count : 0;
		if (appended > MAX_RESULTS - *count) appended = MAX_RESULTS - *count;
		memcpy(dst + *count, job.found, sizeof(VideoEntry) * appended);
		*count += appended;
		if (appended == 0)
		{
			yt_search_free(cursors[job.tab]);
			cursors[job.tab] = NULL;
		}
		return;
	}
	yt_search_free(cursors[job.tab]);   // safe: no page of the old list can be loading, since this job was running
	cursors[job.tab] = job.cursor;
	if (!job.ok)
	{
		snprintf(app.status, sizeof(app.status), "Search failed: %s", job.error);
		return;
	}
	memcpy(dst, job.found, sizeof(VideoEntry) * job.count);
	*count = job.count;
	app.sel[job.tab] = 0;
	app.anim[job.tab] = 0;
	snprintf(app.status, sizeof(app.status), job.count ? "" : "No results. Try other words.");
}

static void poll_search(void)
{
	if (!app.busy || !job.done) return;
	__sync_synchronize();
	threadJoin(search_thread, U64_MAX);
	threadFree(search_thread);
	search_thread = NULL;
	app.busy = false;
	app.loading_more = false;
	take_result();
	if (pending.on)
	{
		// Start the search that was requested while this one ran.
		pending.on = false;
		start_search(pending.query, pending.three_d, pending.tab);
	}
}

// ---------------------------------------------------------------- actions

// What the ui_*_hit functions return (ui.h): 0 is nothing.
enum {
	HIT_CANCEL = 1,   // the form or menu's Cancel
	HIT_SAVE = 2,     // the form's Save
	HIT_ITEM = 100,   // + i: form field or menu choice i
	HIT_BACK = 1,     // the Wi-Fi test's Back
	HIT_AGAIN = 2,    // the Wi-Fi test's Connect / Test again
};

// Asks for text on the keyboard. Optional text may be left empty, and secret text
// (passwords) is typed hidden.
static bool ask_text(const char* hint, const char* initial, char* out, size_t outlen, int maxlen, bool optional,
                     bool secret)
{
	SwkbdState kb;
	swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, maxlen);
	swkbdSetHintText(&kb, hint);
	if (initial && *initial) swkbdSetInitialText(&kb, initial);
	swkbdSetValidation(&kb, optional ? SWKBD_ANYTHING : SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
	if (secret) swkbdSetPasswordMode(&kb, SWKBD_PASSWORD_HIDE_DELAY);
	char buf[512] = "";
	if (swkbdInputText(&kb, buf, sizeof(buf)) != SWKBD_BUTTON_CONFIRM) return false;
	snprintf(out, outlen, "%s", buf);
	return true;
}

// ---- network work off the main thread
// Talking to a network place can take seconds, and some servers never answer at all. So the
// request runs on a thread while the main loop keeps drawing "Connecting", HOME keeps working,
// and Cancel or B stops waiting. A cancelled request still finishes in the background (its
// connection times out eventually), and the next request waits for it first.
typedef enum { NJ_LIST, NJ_TEST, NJ_FINGERPRINT } NetJobKind;
static struct {
	NetJobKind kind;
	char arg[300];          // the folder, URL or place name
	int count;              // NJ_LIST: number of entries, < 0 on failure
	bool ok;                // result of NJ_TEST and NJ_FINGERPRINT
	char fingerprint[80];   // NJ_FINGERPRINT: the key fingerprint
	char err[200];
	volatile bool done;
} netjob;
static Thread netjob_thread = NULL;

static void netjob_main(void* arg)
{
	(void)arg;
	switch (netjob.kind)
	{
	case NJ_LIST: netjob.count = files_net_fetch(netjob.arg, netjob.err, sizeof(netjob.err)); break;
	case NJ_TEST: netjob.ok = nf_test_server(netjob.arg, netjob.err, sizeof(netjob.err)); break;
	default:
		netjob.ok = nf_sftp_fingerprint(netjob.arg, netjob.fingerprint, sizeof(netjob.fingerprint), netjob.err,
		                                sizeof(netjob.err));
	}
	__sync_synchronize();   // make the results visible before done is set
	netjob.done = true;
}

// Waits for the background request, if there is one, showing "Connecting <where>". Returns
// false if the user cancelled.
static bool netjob_wait(const char* where, bool cancellable)
{
	bool cancel = false;
	while (netjob_thread && !netjob.done)
	{
		if (!aptMainLoop())
		{
			cancel = true;
			break;
		}
		hidScanInput();
		const bool tapped = ui_connecting(where);
		if (cancellable && (tapped || (hidKeysDown() & KEY_B)))
		{
			cancel = true;
			break;
		}
		svcSleepThread(16 * 1000000ULL);
	}
	if (netjob_thread && netjob.done)
	{
		threadJoin(netjob_thread, U64_MAX);
		threadFree(netjob_thread);
		netjob_thread = NULL;
	}
	redraw_requested = true;
	return !cancel;
}

// Runs one request on the thread and waits for it. Returns false if it was cancelled.
static bool netjob_run(NetJobKind kind, const char* arg, const char* where)
{
	if (netjob_thread && !netjob_wait("the last request", false))
		return false;   // an earlier cancelled request is still finishing
	memset(&netjob, 0, sizeof(netjob));
	netjob.kind = kind;
	str_copy(netjob.arg, sizeof(netjob.arg), arg);
	netjob_thread = threadCreate(netjob_main, NULL, 64 * 1024, 0x30, -2, false);
	if (!netjob_thread)
	{
		snprintf(netjob.err, sizeof(netjob.err), "Couldn't start the network request.");
		netjob.count = -1;
		return true;   // finished, but failed; err says why
	}
	return netjob_wait(where, true);
}

// Opens a folder in the Videos tab. Network folders are read on a thread, and if that fails
// or is cancelled the list stays as it was, with the reason shown.
static void open_folder(const char* path)
{
	app.status[0] = 0;
	if (!files_is_network(path))
	{
		files_scan(path);
		return;
	}
	char location[300];
	files_location(path, location, sizeof(location));
	if (!netjob_run(NJ_LIST, path, location))
	{
		snprintf(app.status, sizeof(app.status), "Cancelled.");
		return;
	}
	if (netjob.count < 0)
	{
		// Tell the user, naming the place. In a location like "SMB > Living room PC > Movies"
		// the place is the second part.
		char title[80];
		const char* place = strstr(location, "\xE2\x80\xBA ");
		snprintf(title, sizeof(title), "Can't connect to %.40s", place ? place + 4 : location);
		char* end = strstr(title, " \xE2\x80\xBA");
		if (end) *end = 0;
		pui_message(title, netjob.err[0] ? netjob.err : "The server didn't answer.", NULL, "OK");
		redraw_requested = true;
		return;
	}
	files_net_apply(path, netjob.count, netjob.err);
}

// The form for a network place, new (sv empty) or being edited. It shows every field; tapping
// one brings up the keyboard, and Save finishes. Returns false if it was cancelled.
static bool server_form(SrvKind kind, Server* sv, const char* title)
{
	const bool smb = kind == SRV_SMB;
	char port[8] = "22";
	if (sv->port) snprintf(port, sizeof(port), "%d", sv->port);
	enum { F_HOST, F_PORT, F_USER, F_PASS, F_NAME };
	// The fields shown, in order. SMB doesn't ask for a port.
	int order[UI_FORM_MAX], num_fields = 0;
	order[num_fields++] = F_HOST;
	if (!smb) order[num_fields++] = F_PORT;
	order[num_fields++] = F_USER;
	order[num_fields++] = F_PASS;
	order[num_fields++] = F_NAME;   // no start share or folder: like a file manager, a place opens at its top
	int sel = 0;
	while (aptMainLoop())
	{
		UiField fields[UI_FORM_MAX];
		for (int i = 0; i < num_fields; i++)
		{
			switch (order[i])
			{
			case F_HOST: fields[i] = (UiField){ "Address", sv->host, "required", false }; break;
			case F_PORT: fields[i] = (UiField){ "Port", port, "22", false }; break;
			case F_USER: fields[i] = (UiField){ "User name", sv->user, smb ? "guest" : "required", false }; break;
			case F_PASS: fields[i] = (UiField){ "Password", sv->pass, "none", true }; break;
			default:
				fields[i] = (UiField){ "Name", sv->name, sv->host[0] ? sv->host : "same as the address", false };
				break;
			}
		}
		const bool can_save = sv->host[0] && (smb || sv->user[0]);
		ui_form(title, fields, num_fields, sel, can_save);

		hidScanInput();
		const u32 keys = hidKeysDown();
		int hit = 0;
		if (keys & KEY_TOUCH)
		{
			touchPosition tp;
			hidTouchRead(&tp);
			hit = ui_form_hit(tp.px, tp.py, num_fields);
		}
		if (keys & KEY_DUP) sel = sel > 0 ? sel - 1 : 0;
		if (keys & KEY_DDOWN) sel = sel < num_fields - 1 ? sel + 1 : num_fields - 1;
		if (keys & KEY_A) hit = HIT_ITEM + sel;
		if (keys & KEY_START) hit = HIT_SAVE;
		if (keys & KEY_B) hit = HIT_CANCEL;
		if (hit == HIT_CANCEL) return false;
		if (hit == HIT_SAVE && can_save) break;
		if (hit < HIT_ITEM) continue;
		sel = hit - HIT_ITEM;
		switch (order[sel])
		{
		case F_HOST:
			ask_text(smb ? "The computer: its address (192.168.1.20) or name" : "The server's address (192.168.1.20)",
			         sv->host, sv->host, sizeof(sv->host), 95, false, false);
			break;
		case F_PORT:
			ask_text("Port (22 unless the server says otherwise)", port, port, sizeof(port), 5, true, false);
			break;
		case F_USER:
			ask_text(smb ? "User name (empty for guest access)" : "User name", sv->user, sv->user, sizeof(sv->user), 63,
			         smb, false);
			break;
		case F_PASS: ask_text("Password", "", sv->pass, sizeof(sv->pass), 63, true, true); break;
		default:
			ask_text("A name for this place", sv->name[0] ? sv->name : sv->host, sv->name, sizeof(sv->name), 31, true,
			         false);
		}
		if (sel < num_fields - 1) sel++;   // move on to the next field
	}
	if (!aptMainLoop()) return false;
	const int port_num = (int)strtol(port, NULL, 10);
	sv->port = smb || port_num == 22 || port_num <= 0 || port_num > 65535 ? 0 : port_num;
	if (!sv->name[0]) str_copy(sv->name, sizeof(sv->name), sv->host);
	for (char* c = sv->name; *c; c++)
		if (*c == '/' || *c == '\t') *c = '-';   // the name becomes part of URLs
	return true;
}

// Saves a place from the form and then connects to it. old_name is the place it replaces
// when editing, NULL otherwise. For an SFTP server without a pinned key (a new one, or one
// whose address changed) the user is asked to trust its key first.
static void save_server(Server sv, const char* old_name)
{
	const SrvKind kind = sv.kind;
	const bool smb = kind == SRV_SMB;
	sv.start[0] = 0;   // places always open at their top, so clear any old start folder
	servers_load();
	if (old_name && strcmp(old_name, sv.name) != 0) servers_remove(kind, old_name);
	if (!servers_add(&sv))
	{
		pui_message("Not saved", "The list of places couldn't be written to the SD card.", NULL, "OK");
		return;
	}
	char url[200], err[200] = "";
	snprintf(url, sizeof(url), smb ? "smb://%s/" : "sftp://%s/~/", sv.name);   // the place's top, as files.c lists it
	if (!smb && !sv.hostkey[0])
	{
		// Show the server's key once so the user can trust it. After that, any other key is refused.
		char fingerprint[80];
		const bool got = netjob_run(NJ_FINGERPRINT, sv.name, sv.host) && netjob.ok;
		str_copy(fingerprint, sizeof(fingerprint), netjob.fingerprint);
		if (!got) str_copy(err, sizeof(err), netjob.err[0] ? netjob.err : "Cancelled.");
		else
		{
			char msg[300];
			snprintf(msg, sizeof(msg), "%s's key (SHA-256):\n%.32s\n%.32s\n\nTrust it? Only if it is your server.",
			         sv.host, fingerprint, fingerprint + 32);
			if (!pui_message("Trust this server?", msg, "Trust", "Remove"))
			{
				servers_remove(kind, sv.name);
				files_scan(app.files_dir);
				return;
			}
			servers_set_hostkey(sv.name, fingerprint);
		}
	}
	const bool reached = netjob_run(NJ_TEST, url, sv.host) && netjob.ok;
	if (!reached)
	{
		str_copy(err, sizeof(err), netjob.err[0] ? netjob.err : "Cancelled.");
		char msg[300];
		snprintf(msg, sizeof(msg), "%.200s\n\nKeep it in the list anyway?", err);
		if (!pui_message("Couldn't connect", msg, "Keep", "Remove")) servers_remove(kind, sv.name);
		files_scan(app.files_dir);
		return;
	}
	open_folder(url);   // go straight in
	app.sel[TAB_FILES] = 0;
}

static void add_server(SrvKind kind)
{
	Server sv;
	memset(&sv, 0, sizeof(sv));
	sv.kind = kind;
	if (server_form(kind, &sv, kind == SRV_SMB ? "Add an SMB share" : "Add an SFTP server")) save_server(sv, NULL);
}

// The menu behind a saved place's ⋮ button (or X): edit it in the form, or remove it after
// asking.
static void place_menu(int idx)
{
	if (idx < 0 || idx >= app.nfiles || !files_is_network(app.files[idx].path)) return;
	const SrvKind kind = app.files_dir[1] == 'm' ? SRV_SMB : SRV_SFTP;
	Server sv;
	if (!servers_lookup(kind, app.files[idx].title, &sv)) return;
	static const char* items[2] = { "Edit", "Remove" };
	char subtitle[140];
	snprintf(subtitle, sizeof(subtitle), "%.60s", sv.host);
	int sel = 0, choice = -1;
	while (aptMainLoop() && choice < 0)
	{
		ui_menu(sv.name, subtitle, items, 2, sel, 1);
		hidScanInput();
		const u32 keys = hidKeysDown();
		int hit = 0;
		if (keys & KEY_TOUCH)
		{
			touchPosition tp;
			hidTouchRead(&tp);
			hit = ui_menu_hit(tp.px, tp.py, 2);
		}
		if (keys & KEY_DUP) sel = 0;
		if (keys & KEY_DDOWN) sel = 1;
		if (keys & KEY_A) hit = HIT_ITEM + sel;
		if (keys & KEY_B) hit = HIT_CANCEL;
		if (hit == HIT_CANCEL) break;
		if (hit >= HIT_ITEM) choice = hit - HIT_ITEM;
	}
	if (choice == 0)
	{
		Server edited = sv;
		if (server_form(kind, &edited, kind == SRV_SMB ? "Edit SMB share" : "Edit SFTP server"))
		{
			// A different server, so ask about its key again.
			if (strcmp(edited.host, sv.host) != 0 || edited.port != sv.port) edited.hostkey[0] = 0;
			save_server(edited, sv.name);
		}
	}
	else if (choice == 1)
	{
		char msg[160];
		snprintf(msg, sizeof(msg), "\"%.40s\" will be removed from the list. Nothing on it is changed.", sv.name);
		if (pui_message("Are you sure?", msg, "Remove", "Cancel"))
		{
			servers_remove(kind, sv.name);
			files_scan(app.files_dir);
			if (app.sel[TAB_FILES] >= app.nfiles) app.sel[TAB_FILES] = app.nfiles > 0 ? app.nfiles - 1 : 0;
		}
	}
	redraw_requested = true;
}

static void play_video(const VideoEntry* video)
{
	app.now_playing = video;
	PlayMode mode = app.mode;   // a copy: pressing X during playback changes this video only
	ui_playback(true);   // screen-size render targets, leaving VRAM for the video and skipping the 4x supersampling
	if (video->path[0])
	{
		thumbs_release();   // free linear memory: 720p and 1080p decoders need up to ~18 MB
		direct_play_file(video->path, video->title, &mode, app.audio_ok);
		files_update_resume();   // so the list shows where the file was left
	}
	else
	{
		direct_set_meta(video->channel,
		                video->views);   // shown under the title (for a pasted link, YouTube fills in the channel)
		direct_play(video->id, video->title, &mode, app.audio_ok);
	}
	if (app.settings_changed)   // Smooth motion or nerd stats switched on the player's settings page
	{
		save_config();
		app.settings_changed = false;
	}
	const u64 start_ms = osGetTime();
	if (!aptShouldClose()) ui_playback(false);
	tlog("main: menu screens back in %llu ms", (unsigned long long)(osGetTime() - start_ms));
	app.now_playing = NULL;
}

static void play_selected(void)
{
	int count;
	VideoEntry* list = app_list(app.tab, &count);
	if (count == 0 || app.tab == TAB_SETTINGS) return;
	VideoEntry picked = list[app.sel[app.tab] < count ? app.sel[app.tab] : count - 1];
	if (picked.folder)
	{
		if (!strcmp(picked.path, "smb:+") || !strcmp(picked.path, "sftp:+"))
		{
			add_server(picked.path[1] == 'm' ? SRV_SMB : SRV_SFTP);
			redraw_requested = true;
			return;
		}
		open_folder(picked.path);
		app.sel[TAB_FILES] = 0;
		app.anim[TAB_FILES] = 0;
		app.status[0] = 0;
		redraw_requested = true;
		return;
	}
	if (picked.other)
	{
		snprintf(app.status, sizeof(app.status), "Not a video: Tube3D plays MKV and MP4 files.");
		redraw_requested = true;
		return;
	}
	if (picked.path[0])
	{
		static VideoEntry file;   // files stay out of the YouTube history
		file = picked;
		play_video(&file);
		return;
	}
	add_history(&picked);
	play_video(&app.history[0]);
}

// Finds the 11-character video id in a YouTube link (watch?v=, youtu.be/, shorts/, ...).
static bool youtube_id(const char* url, char* id)
{
	static const char* keys[] = { "?v=", "&v=", "youtu.be/", "/shorts/", "/embed/", "/live/" };
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
	{
		const char* start = strstr(url, keys[i]);
		if (!start) continue;
		start += strlen(keys[i]);
		size_t len = 0;
		while (start[len] && (isalnum((unsigned char)start[len]) || start[len] == '-' || start[len] == '_')) len++;
		if (len == 11)
		{
			memcpy(id, start, 11);
			id[11] = 0;
			return true;
		}
	}
	return false;
}

// The search bar. Words search YouTube, while a YouTube link (pasted or typed) plays that video.
static void do_search(void)
{
	char text[400];
	if (!ask_text("Search YouTube, or paste a link", app.query, text, sizeof(text), 380, false, false)) return;
	static VideoEntry adhoc;
	memset(&adhoc, 0, sizeof(adhoc));
	if (youtube_id(text, adhoc.id))
	{
		str_copy(adhoc.title, sizeof(adhoc.title), text);
		play_video(&adhoc);
		return;
	}
	snprintf(app.query, sizeof(app.query), "%.100s", text);
	snprintf(last_search.query, sizeof(last_search.query), "%s", app.query);
	last_search.three_d = false;   // a plain search; typing "3D" is how to look for 3D videos
	app.tab = TAB_HOME;
	start_search(app.query, false, TAB_HOME);
}

// Tapping Home while already on Home goes back to the start page, with the big search field
// and the quick picks.
static void home_reset(void)
{
	if (pending.on && pending.tab == TAB_HOME) pending.on = false;   // cancel a search waiting to start
	if (app.busy && job.tab == TAB_HOME) job.discard = true;         // take_result frees its pages
	else
	{
		yt_search_free(cursors[TAB_HOME]);
		cursors[TAB_HOME] = NULL;
	}
	app.nresults = 0;
	app.sel[TAB_HOME] = 0;
	app.anim[TAB_HOME] = 0;
	app.query[0] = 0;
	last_search.query[0] = 0;
	app.status[0] = 0;
}

static void set_tab(Tab tab)
{
	app.tab = tab;
	app.status[0] = 0;
	if (tab == TAB_PICKS && app.npicks == 0 && !app.busy) start_search("3D SBS", true, TAB_PICKS);
	if (tab == TAB_FILES)
		open_folder(app.files_dir);   // re-read on every visit in case files were copied; "" is the sources
}

static void move_sel(int delta)
{
	int count;
	app_list(app.tab, &count);
	if (count == 0 || app.tab == TAB_SETTINGS) return;
	int sel = app.sel[app.tab] + delta;
	if (sel < 0) sel = 0;
	if (sel > count - 1) sel = count - 1;
	app.sel[app.tab] = sel;
}

// ---------------------------------------------------------------- Settings > Wi-Fi test

static volatile bool connect_done;
static void connect_main(void* arg)
{
	wifi_connect(20000);
	connect_done = true;
}

static void wifi_screen(void)
{
	static WifiTest test;
	if (!wifi_test_start(&test)) return;
	Thread connect_thread = NULL;   // the running connect request, if any
	while (true)
	{
		if (!aptMainLoop())
		{
			exit_requested("wifi test");   // cuts the test's network requests short
			break;
		}
		hidScanInput();
		u32 keys = hidKeysDown();
		int hit = 0;
		if (keys & KEY_TOUCH)
		{
			touchPosition tp;
			hidTouchRead(&tp);
			hit = ui_wifi_hit(tp.px, tp.py);
		}
		if (keys & KEY_B) hit = HIT_BACK;
		if (keys & KEY_A) hit = HIT_AGAIN;

		if (connect_thread && connect_done)
		{
			// Connected, or gave up. Run the test again to show where things stand.
			threadJoin(connect_thread, U64_MAX);
			threadFree(connect_thread);
			connect_thread = NULL;
			wifi_test_end(&test);
			wifi_test_start(&test);
		}
		bool busy = !test.done || connect_thread;
		if (!busy && hit == HIT_BACK) break;
		if (!busy && hit == HIT_AGAIN)
		{
			bool need_connect = test.link != WT_OK;
			wifi_test_end(&test);
			if (need_connect)
			{
				connect_done = false;
				connect_thread = threadCreate(connect_main, NULL, 16 * 1024, 0x30, -2, false);
			}
			if (!connect_thread) wifi_test_start(&test);
		}
		ui_wifi(&test, connect_thread != NULL);
	}
	if (connect_thread)
	{
		threadJoin(connect_thread, U64_MAX);
		threadFree(connect_thread);
	}
	wifi_test_end(&test);
}

static void handle(UiAction action)
{
	switch (action)
	{
	case UA_SEARCH: do_search(); break;
	case UA_SEARCH_AGAIN:   // search the same words again without the keyboard, from the top
		if (!last_search.query[0]) do_search();
		else
		{
			app.tab = TAB_HOME;
			start_search(last_search.query, last_search.three_d, TAB_HOME);
		}
		break;
	case UA_FILES_ROW_0:
	case UA_FILES_ROW_1:
	case UA_FILES_ROW_2:
	case UA_FILES_ROW_3:
	case UA_FILES_ROW_4: {
		// Tapping a row selects it, and tapping the selected row opens or plays it. The source
		// tiles open on the first tap.
		int idx = ui_files_first() + (int)(action - UA_FILES_ROW_0);
		if (idx >= app.nfiles) break;
		if (idx == app.sel[TAB_FILES] || !app.files_dir[0])
		{
			app.sel[TAB_FILES] = idx;
			play_selected();
		}
		else app.sel[TAB_FILES] = idx;
		break;
	}
	case UA_FILES_MORE_0:
	case UA_FILES_MORE_1:
	case UA_FILES_MORE_2:
	case UA_FILES_MORE_3:
	case UA_FILES_MORE_4: place_menu(ui_files_first() + (int)(action - UA_FILES_MORE_0)); break;
	case UA_FILES_PGUP:   move_sel(-5); break;
	case UA_FILES_PGDN:   move_sel(5); break;
	case UA_FILES_UP:     {
		// Go up a folder and select the one we just came out of.
		char from[sizeof(app.files_dir)], up[sizeof(app.files_dir)];
		str_copy(from, sizeof(from), app.files_dir);
		files_parent(from, up, sizeof(up));
		open_folder(up);
		app.sel[TAB_FILES] = 0;
		for (int i = 0; i < app.nfiles; i++)
			if (!strcmp(app.files[i].path, from)) app.sel[TAB_FILES] = i;
		app.anim[TAB_FILES] = (float)app.sel[TAB_FILES];
		app.status[0] = 0;
		break;
	}
	case UA_PREV: move_sel(-1); break;
	case UA_NEXT: move_sel(1); break;
	case UA_PLAY: play_selected(); break;
	case UA_TAB_HOME:
	case UA_TAB_PICKS:
	case UA_TAB_HISTORY:
	case UA_TAB_FILES:
	case UA_TAB_SETTINGS:
		if (action == UA_TAB_HOME && app.tab == TAB_HOME) home_reset();   // tapped again: back to the start page
		else set_tab((Tab)(action - UA_TAB_HOME));
		break;
	case UA_WIFI: wifi_screen(); break;
	case UA_MENU_DEPTH:
		app.menu_depth = (app.menu_depth + 1) & 3;
		save_config();
		break;
	case UA_DEPTH_0:
	case UA_DEPTH_1:
	case UA_DEPTH_2:
	case UA_DEPTH_3:
		app.menu_depth = action - UA_DEPTH_0;
		save_config();
		break;
	case UA_PICK_0:
	case UA_PICK_1:
	case UA_PICK_2:
	case UA_PICK_3:
	case UA_PICK_4:
	case UA_PICK_5: {
		const QuickPick* pick = &quick_picks[action - UA_PICK_0];
		snprintf(app.query, sizeof(app.query), "%s%s", pick->three_d ? "3D " : "", pick->query);
		snprintf(last_search.query, sizeof(last_search.query), "%s", pick->query);
		last_search.three_d = pick->three_d;
		app.tab = TAB_HOME;
		start_search(pick->query, pick->three_d, TAB_HOME);
		break;
	}
	case UA_SET_PAGE_0:
	case UA_SET_PAGE_1:
	case UA_SET_PAGE_2: app.set_page = action - UA_SET_PAGE_0; break;
	case UA_SUBTITLES:
		app.subtitles = !app.subtitles;
		save_config();
		break;
	case UA_NERD_STATS:
		app.nerd_stats = !app.nerd_stats;
		save_config();
		break;
	case UA_MOTION:
		app.motion = (app.motion + 1) % MOTION_COUNT;
		save_config();
		break;
	default:
		if (action >= UA_MODE_0 && action < UA_MODE_0 + MODE_COUNT)
		{
			app.mode = (PlayMode)(action - UA_MODE_0);
			save_config();
		}
		break;
	}
	redraw_requested = true;   // whatever ran may have drawn over the screens
}

// ---------------------------------------------------------------- main

// Everything the menu shows apart from the list itself. When any of it changes, the menu
// has to redraw.
typedef struct {
	int minute;
	float slider;
	int battery;
	bool charging;
	u8 bars;
	bool online;
} UiState;

static bool ui_state_same(const UiState* a, const UiState* b)
{
	return a->minute == b->minute && a->slider == b->slider && a->battery == b->battery && a->charging == b->charging &&
	       a->bars == b->bars && a->online == b->online;
}

static void on_apt_event(APT_HookType type, void* param)
{
	// Logged so that a hang around the HOME menu or sleep shows how far the transition got.
	static const char* names[] = { "suspend (to HOME)", "restore (back from HOME)", "sleep", "wakeup", "exit" };
	tlog("apt: %s", type < APTHOOK_COUNT ? names[type] : "?");
	if (type == APTHOOK_ONRESTORE || type == APTHOOK_ONWAKEUP)
		redraw_requested = true;   // back from the HOME menu or sleep
}

int main(void)
{
	gfxInitDefault();           // BGR8 framebuffers for citro2d; playback switches the top screen to RGB565
	osSetSpeedupEnable(true);   // 804 MHz and the L2 cache on the New 3DS, for the whole run (see power.h)
	tlog_open();                // sdmc:/3ds/Tube3D/log.txt, for chasing problems like a slow shutdown
	if (!ui_init())
	{
		gfxExit();
		return 0;
	}

	tlog("ui: %s targets, %s font", ui_supersampled() ? "2x2 supersampled" : "screen-size",
	     ui_font_ok() ? "crisp (uifont)" : "system");
	bool is_new3ds = false;
	APT_CheckNew3DS(&is_new3ds);
	if (!is_new3ds)
	{
		ui_fatal("New 3DS needed", "Tube3D uses the hardware video decoder that only the New 3DS and New 2DS XL have.");
		ui_exit();
		gfxExit();
		return 0;
	}

	app.audio_ok = R_SUCCEEDED(ndspInit());   // fails if the DSP firmware hasn't been dumped
	if (!net_init())
	{
		ui_fatal("No network", "Couldn't start networking. Turn Wi-Fi on and open Tube3D again.");
		if (app.audio_ok) ndspExit();
		ui_exit();
		gfxExit();
		return 0;
	}

	yt_init();      // curl for YouTube; if it fails, searches and playback report the error
	wifi_init();    // Wi-Fi state for the header icon and Settings > Wi-Fi test
	power_init();   // battery icon, backlight and CPU speed
	tlog("started: build %s", app_build_id());

	app.menu_depth = 2;           // Normal, unless the config says otherwise
	app.motion = MOTION_SMOOTH;   // Smooth by default; it falls back to whole frames by itself when it can't keep up
	load_config();
	load_history();
	servers_load();   // load network places now, since a saved path can play before the Videos tab is opened
	thumbs_init();
	ui_draw();
	aptHookCookie restore_hook;
	aptHook(&restore_hook, on_apt_event, NULL);   // redraw after the HOME menu
	UiState shown;
	memset(&shown, 0, sizeof(shown));
	u64 redraw_until = 0;

	while (aptMainLoop())
	{
		// Track Wi-Fi coming and going. The header icon and the offline line both read this
		// state, and a change triggers a redraw through UiState.
		wifi_poll();

		hidScanInput();
		u32 keys = hidKeysDown();
		u32 repeat_keys = hidKeysDownRepeat();

		if (keys & KEY_START) break;
		if ((keys & KEY_Y) && app.tab != TAB_FILES) handle(UA_SEARCH);
		if ((keys & KEY_X) && app.tab == TAB_FILES &&
		    (!strcmp(app.files_dir, "smb:") || !strcmp(app.files_dir, "sftp:")) && app.nfiles > 0)
		{
			// In a server list of the Videos tab, X opens the selected place's menu (edit, remove).
			place_menu(app.sel[TAB_FILES] < app.nfiles ? app.sel[TAB_FILES] : app.nfiles - 1);
		}
		if (keys & KEY_A) handle(UA_PLAY);
		if (keys & KEY_B)
		{
			if (app.tab == TAB_FILES && app.files_dir[0]) handle(UA_FILES_UP);   // go up a folder
			else set_tab(TAB_HOME);
		}
		if (keys & KEY_L) set_tab((Tab)((app.tab + TAB_COUNT - 1) % TAB_COUNT));
		if (keys & KEY_R) set_tab((Tab)((app.tab + 1) % TAB_COUNT));
		if (keys & KEY_SELECT)
		{
			app.mode = (PlayMode)((app.mode + 1) % MODE_COUNT);
			save_config();
		}
		if (repeat_keys & (KEY_LEFT | KEY_UP)) move_sel(-1);
		if (repeat_keys & (KEY_RIGHT | KEY_DOWN)) move_sel(1);
		if (keys & KEY_TOUCH)
		{
			touchPosition tp;
			hidTouchRead(&tp);
			handle(ui_hit(tp.px, tp.py));
		}

		bool was_busy = app.busy;
		poll_search();
		// Within a few videos of the end of a search list, start loading its next page.
		if (app.tab == TAB_HOME || app.tab == TAB_PICKS)
		{
			int len;
			app_list(app.tab, &len);
			if (len > 0 && app.sel[app.tab] >= len - 4) start_more(app.tab);
		}

		// Animate the carousel towards the selection and request thumbnails for nearby entries.
		int count;
		VideoEntry* list = app_list(app.tab, &count);
		bool moving = false;
		for (int tab = 0; tab < TAB_COUNT; tab++)
		{
			float gap = app.sel[tab] - app.anim[tab];
			app.anim[tab] = fabsf(gap) < 0.002f ? app.sel[tab] : app.anim[tab] + gap * 0.22f;
			moving |= app.anim[tab] != app.sel[tab];
		}
		if (app.tab != TAB_SETTINGS)
			for (int offset = -3; offset <= 3; offset++)
			{
				int i = app.sel[app.tab] + offset;
				if (i < 0 || i >= count) continue;
				const int priority = 100 - abs(offset);   // the selected entry first
				if (strlen(list[i].id) == 11)             // a YouTube video gets its thumbnail JPEG
					thumbs_request(list[i].id, app_layout(&list[i]), priority);
				else if (list[i].path[0] && !list[i].folder && !list[i].other && offset >= -2 && offset <= 2)
				{
					// A file gets a frame from inside it (filethumb.c).
					char id[24];
					filethumb_id(list[i].path, id, sizeof(id));
					filethumb_register(id, list[i].path);
					thumbs_request(id, app_layout(&list[i]), priority);
				}
			}
		bool thumbs_active = thumbs_update();

		// To save battery, only draw when something has changed, and keep drawing for about
		// 1.5 s afterwards so animations and the background colour fade can finish. Otherwise
		// just wait for the next vblank.
		power_poll();
		u64 now = osGetTime();
		time_t wall = time(NULL);
		float slider = osGet3DSliderState();
		UiState state = { (int)(wall / 60), slider, power_percent(), power_charging(), wifi_bars(), wifi_connected() };
		bool changed = keys || repeat_keys || (hidKeysHeld() & KEY_TOUCH) || moving || was_busy != app.busy ||
		               !ui_state_same(&state, &shown) || redraw_requested;
		if (changed) redraw_until = now + 1500;
		redraw_requested = false;
		shown = state;
		if (now < redraw_until || app.busy || thumbs_active) ui_draw();
		else gspWaitForVBlank();   // nothing new to show, so the GPU stays idle
	}

	// Shutting down, after START, Close in the HOME menu or the power button. Network waits end
	// at once, a watchdog makes sure the app is gone within a few seconds, and every step is
	// logged.
	exit_requested("menu");
	exit_step("search thread");
	for (int i = 0; i < 300 && app.busy && !job.done; i++) svcSleepThread(10 * 1000000ULL);
	pending.on = false;   // don't start another search on the way out
	if (job.done) poll_search();
	// A search may still be stuck inside curl, in a DNS lookup for example, which can't be
	// aborted. Its handle must not be freed under it, and since the process is ending anyway
	// we skip the curl and socket cleanup in that case.
	const bool search_running = app.busy && !job.done;
	if (search_running) tlog("exit: search still running, leaving curl and sockets to the system");
	exit_step("thumbnails");
	thumbs_exit();
	exit_step("wifi");
	wifi_exit();
	exit_step("power");
	power_exit();
	if (!search_running)
	{
		exit_step("curl");
		yt_exit();
		exit_step("sockets");
		net_exit();
	}
	if (app.audio_ok)
	{
		exit_step("audio");
		ndspExit();
	}
	// When the app is closed from the HOME menu it never gets the GPU back, so C3D_Fini would
	// wait forever and "Closing software..." would never end. The process is about to end
	// anyway, so in that case we leave the graphics to the system.
	if (!aptShouldClose())
	{
		exit_step("graphics");
		ui_exit();
		gfxExit();
		exit_done();
		return 0;
	}
	// With the graphics still running, returning from main() would let libctru free the heap
	// while its GPU event thread (gspEventThreadMain, whose stack lives on the heap) can still
	// wake up. That crashes with a data abort when the app is closed from HOME during a video.
	// Instead, end the process the way aptExit() would, taking all threads down at once.
	exit_step("graphics left to the system (closed from HOME), ending the process through APT");
	exit_done();
	exit_through_apt();
}
