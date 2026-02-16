#include "lua.h"
#include "lauxlib.h"
#include "shared_table.h"
#include "GC.h"

// GC 相关 Lua 函数
static int l_gc_collect(lua_State* L) {
    GC* gc = gc_instance();
    gc_collect(gc);
    return 0;
}

static int l_gc_count(lua_State* L) {
    GC* gc = gc_instance();
    lua_pushinteger(L, gc_count(gc));
    return 1;
}

static int l_gc_step(lua_State* L) {
    GC* gc = gc_instance();
    double old = gc_get_step(gc);
    if (lua_gettop(L) >= 1) {
        double new_step = luaL_checknumber(L, 1);
        gc_set_step(gc, new_step);
    }
    lua_pushnumber(L, old);
    return 1;
}

static int l_gc_pause(lua_State* L) {
    GC* gc = gc_instance();
    gc_pause(gc);
    return 0;
}

static int l_gc_resume(lua_State* L) {
    GC* gc = gc_instance();
    gc_resume(gc);
    return 0;
}

static int l_gc_enabled(lua_State* L) {
    GC* gc = gc_instance();
    lua_pushboolean(L, gc_enabled(gc));
    return 1;
}

// 注册模块
int luaopen_XShare(lua_State* L) {
    // 创建metatable
    luaL_newmetatable(L, SHARED_TABLE_MT);
    static const luaL_Reg mt[] = {
        {"__index", l_shared_table_index},
        {"__newindex", l_shared_table_newindex},
        {"__len", l_shared_table_len},
        {"__pairs", l_shared_table_pairs},
        {"__ipairs", l_shared_table_ipairs},
        {"__gc", l_shared_table_gc},
        {"__tostring", l_shared_table_tostring},
        {NULL, NULL}
    };
    luaL_setfuncs(L, mt, 0);
    lua_pop(L, 1);

    // 创建xshare.table构造函数和其他全局函数
    lua_newtable(L);
    lua_pushcfunction(L, l_shared_table_new);
    lua_setfield(L, -2, "table");

    lua_pushcfunction(L, l_shared_table_setmetatable);
    lua_setfield(L, -2, "setmetatable");

    lua_pushcfunction(L, l_shared_table_getmetatable);
    lua_setfield(L, -2, "getmetatable");

    lua_pushcfunction(L, l_shared_table_rawset);
    lua_setfield(L, -2, "rawset");

    lua_pushcfunction(L, l_shared_table_rawget);
    lua_setfield(L, -2, "rawget");

    lua_pushcfunction(L, l_shared_table_size);
    lua_setfield(L, -2, "size");

    lua_newtable(L);  // 压入 gc 表
    lua_pushcfunction(L, l_gc_collect); lua_setfield(L, -2, "collect");
    lua_pushcfunction(L, l_gc_count);   lua_setfield(L, -2, "count");
    lua_pushcfunction(L, l_gc_step);    lua_setfield(L, -2, "step");
    lua_pushcfunction(L, l_gc_pause);   lua_setfield(L, -2, "pause");
    lua_pushcfunction(L, l_gc_resume);  lua_setfield(L, -2, "resume");
    lua_pushcfunction(L, l_gc_enabled); lua_setfield(L, -2, "enabled");
    lua_setfield(L, -2, "gc");  // 将 gc 表设置到主表中

    return 1;
}