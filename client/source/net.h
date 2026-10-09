#pragma once
// Starts and stops the soc:u socket service and owns its buffer. All HTTP(S) traffic goes
// through curl (yt.cpp, hstream.c), and curl needs this up first.
#include <3ds.h>
#include <stdbool.h>

bool net_init(void);
void net_exit(void);
