#ifndef WIIU_NETWORK_H
#define WIIU_NETWORK_H

#include <stdbool.h>

void wiiu_network_init(void);
void wiiu_network_shutdown(void);
bool wiiu_network_is_online(void);

#endif // WIIU_NETWORK_H
