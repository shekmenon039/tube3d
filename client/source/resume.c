#include "resume.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef RESUME_FILE
#define RESUME_FILE "sdmc:/3ds/Tube3D/resume.txt"
#endif

#define MIN_POS_MS    30000   // a point in the first 30 s isn't worth resuming from
#define END_MARGIN_MS 60000   // nor one in the last minute

typedef struct {
	ResumeInfo info;
	char path[400];
} Entry;

static Entry list[RESUME_MAX];   // newest first
static int count = 0;
static bool loaded = false;

// The file has one line per video, with tab-separated fields: pos_ms, dur_ms, audio, sub
// and finally the path.
#define NUM_FIELDS 4   // the numbers before the path
static void load(void)
{
	if (loaded) return;
	loaded = true;
	count = 0;
	FILE* f = fopen(RESUME_FILE, "r");
	if (!f) return;
	char line[480];
	while (count < RESUME_MAX && fgets(line, sizeof(line), f))
	{
		char* cursor = line;
		long fields[NUM_FIELDS];
		bool ok = true;
		for (int i = 0; i < NUM_FIELDS && ok; i++)
		{
			char* end;
			fields[i] = strtol(cursor, &end, 10);
			ok = end != cursor && *end == '\t';
			cursor = end + 1;
		}
		if (!ok) continue;
		char* path = cursor;
		path[strcspn(path, "\r\n")] = 0;
		const size_t len = strlen(path);
		if (!len || len >= sizeof(list[0].path)) continue;   // resume_set never writes a path this long
		Entry* entry = &list[count++];
		entry->info.pos_ms = (uint32_t)fields[0];
		entry->info.dur_ms = (uint32_t)fields[1];
		entry->info.audio = (int)fields[2];
		entry->info.sub = (int)fields[3];
		memcpy(entry->path, path, len + 1);
	}
	fclose(f);
}

static void save(void)
{
	FILE* f = fopen(RESUME_FILE, "w");
	if (!f) return;
	for (int i = 0; i < count; i++)
		fprintf(f, "%lu\t%lu\t%d\t%d\t%s\n", (unsigned long)list[i].info.pos_ms, (unsigned long)list[i].info.dur_ms,
		        list[i].info.audio, list[i].info.sub, list[i].path);
	fclose(f);
}

static int find(const char* path)
{
	for (int i = 0; i < count; i++)
		if (!strcmp(list[i].path, path)) return i;
	return -1;
}

bool resume_worth(uint32_t pos_ms, uint32_t dur_ms)
{
	if (pos_ms < MIN_POS_MS) return false;
	if (!dur_ms) return true;
	return pos_ms + END_MARGIN_MS < dur_ms && pos_ms < dur_ms / 20 * 19;   // not in the last minute or 5%
}

bool resume_get(const char* path, ResumeInfo* out)
{
	load();
	const int i = find(path);
	if (i < 0) return false;
	*out = list[i].info;
	return true;
}

void resume_set(const char* path, const ResumeInfo* info)
{
	load();
	if (strlen(path) >= sizeof(list[0].path)) return;
	const int i = find(path);
	// Track choices are worth keeping even near the start, so a movie restarts in the
	// languages picked for it. Only a finished file, or one where nothing was chosen, is
	// dropped.
	const bool keep = resume_worth(info->pos_ms, info->dur_ms) ||
	                  (info->pos_ms < MIN_POS_MS && (info->audio >= 0 || info->sub != -2));
	if (!keep)
	{
		if (i < 0) return;
		memmove(&list[i], &list[i + 1], sizeof(Entry) * (size_t)(count - i - 1));
		count--;
		save();
		return;
	}
	// Move it to the front, where the newest entry goes.
	const int from = i >= 0 ? i : (count < RESUME_MAX ? count++ : RESUME_MAX - 1);
	memmove(&list[1], &list[0], sizeof(Entry) * (size_t)from);
	list[0].info = *info;
	if (!resume_worth(info->pos_ms, info->dur_ms)) list[0].info.pos_ms = 0;
	snprintf(list[0].path, sizeof(list[0].path), "%s", path);
	save();
}
