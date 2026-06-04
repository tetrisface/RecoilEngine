/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef NET_COMMANDS_H
#define NET_COMMANDS_H

#include <cstdint>

#ifdef SYNCCHECK
void ResetLocalSyncChecksumsForReplayCheckpoint(int32_t frameNum, uint32_t checksum);
#endif

#endif // NET_COMMANDS_H
