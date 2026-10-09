// The version shown in Settings > More: the release number and the commit it was built from,
// e.g. "1.0.0 (e206264)".
#include <stdio.h>
#include "app.h"

#ifndef TUBE3D_VERSION
#define TUBE3D_VERSION "dev"
#endif
#ifndef TUBE3D_COMMIT
#define TUBE3D_COMMIT ""
#endif
#ifndef TUBE3D_BUILT
#define TUBE3D_BUILT ""
#endif

const char* app_build_id(void)
{
	static char id[48];
	if (!id[0])
		snprintf(id, sizeof(id), "%s%s%s%s%s%s", TUBE3D_VERSION, TUBE3D_COMMIT[0] ? " (" : "", TUBE3D_COMMIT,
		         TUBE3D_COMMIT[0] ? ")" : "", TUBE3D_BUILT[0] ? " " : "", TUBE3D_BUILT);
	return id;
}
