#ifndef MAC_SWIM2_H
#define MAC_SWIM2_H

#include <stdint.h>

struct fileTYPE;

bool swim2_can_mount(int disk, bool eject = false);
void swim2_poll();
bool swim2_retry();
int swim2_sd_service(int disk, fileTYPE *file, int op, uint64_t lba,
                     uint32_t size, int ack, int wide, uint64_t *cache_lba);

#endif
