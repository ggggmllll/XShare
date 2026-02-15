#ifndef SHARED_TABLE_H
#define SHARED_TABLE_H

#include <lua.h>
#include <pthread.h>
#include "GC.h"
#include "stored_object.h"

typedef struct SharedTable {
    GCObject header;
    pthread_rwlock_t lock;
    struct {
        StoredObject** keys;
        StoredObject** vals;
        size_t cap;
        size_t size;
    } entries;
    StoredObject* metatable;   // 元表（可能为NULL或指向另一个SharedTable的StoredObject）
} SharedTable;

// 创建新的空SharedTable
SharedTable* shared_table_create(GC* gc);

// 设置键值对（增加键和值的引用，若键已存在则替换并释放旧值）
int shared_table_set(SharedTable* tbl, StoredObject* key, StoredObject* val);

// 获取键对应的值（返回的StoredObject*未增加引用，调用者不应释放，因为值仍属于表）
StoredObject* shared_table_get(SharedTable* tbl, StoredObject* key);

// 删除键（释放键和值的引用）
void shared_table_delete(SharedTable* tbl, StoredObject* key);

// 返回元素个数
size_t shared_table_size(SharedTable* tbl);

// 返回表的长度（#操作，整数键连续段）
size_t shared_table_length(SharedTable* tbl);

// 迭代器：获取第一个/下一个键值对。若key为NULL，从头开始；否则从key之后开始。
// 返回的key和val均为StoredObject*（属于表内部，调用者不应释放）。
typedef struct { StoredObject* key; StoredObject* val; } SharedTablePair;
SharedTablePair shared_table_next(SharedTable* tbl, StoredObject* key);

// 设置元表（mt应为NULL或指向SharedTable的StoredObject）
void shared_table_set_metatable(SharedTable* tbl, StoredObject* mt);

// 获取元表（返回的StoredObject*可能为NULL）
StoredObject* shared_table_get_metatable(SharedTable* tbl);

// 以下为Lua绑定函数
int l_shared_table_new(lua_State* L);
int l_shared_table_index(lua_State* L);
int l_shared_table_newindex(lua_State* L);
int l_shared_table_len(lua_State* L);
int l_shared_table_pairs(lua_State* L);
int l_shared_table_ipairs(lua_State* L);
int l_shared_table_tostring(lua_State* L);
int l_shared_table_setmetatable(lua_State* L);
int l_shared_table_getmetatable(lua_State* L);
int l_shared_table_rawset(lua_State* L);
int l_shared_table_rawget(lua_State* L);
int l_shared_table_size(lua_State* L);
int l_shared_table_gc(lua_State* L);

#endif