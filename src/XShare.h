#ifndef _XSHARE_H_
#define _XSHARE_H_

#include "lua.h"
#include "GC.h"
#include "shared_table.h"
#include "stored_object.h"

#ifdef __cplusplus
extern "C" {
#endif


int luaopen_XShare(lua_State* L);

#ifdef __cplusplus
}
#endif

#endif