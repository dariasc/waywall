#ifndef WAYWALL_CONFIG_INTERNAL_H
#define WAYWALL_CONFIG_INTERNAL_H

#include "config/config.h"
#include <luajit-2.1/lua.h>
#include <stdint.h>

#define BIND_BUFLEN 17

int config_api_init(struct config_vm *vm);

void config_dump_stack(lua_State *L);
void config_encode_bind(char buf[static BIND_BUFLEN], const struct config_action *action);
int config_parse_hex(uint8_t rgba[static 4], const char *raw);

#endif
