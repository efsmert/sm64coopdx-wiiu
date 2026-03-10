#ifndef PC_DIAG_H
#define PC_DIAG_H

#include <stdint.h>

void pc_diag_watchdog_init(void);
void pc_diag_mark_stage(const char *stage);
void pc_diag_mark_frame(uint32_t frame_index);

#endif // PC_DIAG_H
