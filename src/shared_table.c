#include "shared_table.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "GC.h"
#include "lauxlib.h"  // 用于luaL_checkudata

// 内部：查找键的索引，返回-1表示未找到
static int find_key_index(SharedTable* tbl, StoredObject* key) {
    for (size_t i = 0; i < tbl->entries.size; i++) {
        if (stored_compare(tbl->entries.keys[i], key) == 0)
            return (int)i;
    }
    return -1;
}

// 内部：确保数组容量
static int ensure_capacity(SharedTable* tbl, size_t needed) {
    if (needed <= tbl->entries.cap) return 1;
    size_t newcap = tbl->entries.cap ? tbl->entries.cap * 2 : 4;
    while (newcap < needed) newcap *= 2;
    StoredObject** newkeys = realloc(tbl->entries.keys, newcap * sizeof(StoredObject*));
    if (!newkeys) return 0;
    StoredObject** newvals = realloc(tbl->entries.vals, newcap * sizeof(StoredObject*));
    if (!newvals) {
        free(newkeys);
        return 0;
    }
    tbl->entries.keys = newkeys;
    tbl->entries.vals = newvals;
    tbl->entries.cap = newcap;
    return 1;
}

// 析构函数
static void shared_table_dtor(GCObject* obj) {
    SharedTable* tbl = (SharedTable*)obj;
    // 释放所有键值对的引用
    for (size_t i = 0; i < tbl->entries.size; i++) {
        gc_release((GCObject*)tbl->entries.keys[i]);
        gc_release((GCObject*)tbl->entries.vals[i]);
    }
    free(tbl->entries.keys);
    free(tbl->entries.vals);
    if (tbl->metatable)
        gc_release((GCObject*)tbl->metatable);
    pthread_rwlock_destroy(&tbl->lock);
}

SharedTable* shared_table_create(GC* gc) {
    SharedTable* tbl = (SharedTable*)gc_create(gc, sizeof(SharedTable) - sizeof(GCObject));
    if (!tbl) return NULL;
    tbl->header.dtor = shared_table_dtor;
    pthread_rwlock_init(&tbl->lock, NULL);
    tbl->entries.keys = NULL;
    tbl->entries.vals = NULL;
    tbl->entries.cap = 0;
    tbl->entries.size = 0;
    tbl->metatable = NULL;
    return tbl;
}

int shared_table_set(SharedTable* tbl, StoredObject* key, StoredObject* val) {
    pthread_rwlock_wrlock(&tbl->lock);
    int idx = find_key_index(tbl, key);
    if (idx >= 0) {
        gc_release((GCObject*)tbl->entries.vals[idx]);
        tbl->entries.vals[idx] = val;
        gc_add_reference((GCObject*)tbl, (GCObject*)val);
    } else {
        if (!ensure_capacity(tbl, tbl->entries.size + 1)) {
            pthread_rwlock_unlock(&tbl->lock);
            return 0;
        }
        tbl->entries.keys[tbl->entries.size] = key;
        tbl->entries.vals[tbl->entries.size] = val;
        tbl->entries.size++;
        gc_add_reference((GCObject*)tbl, (GCObject*)key);
        gc_add_reference((GCObject*)tbl, (GCObject*)val);
    }
    pthread_rwlock_unlock(&tbl->lock);
    return 1;
}

StoredObject* shared_table_get(SharedTable* tbl, StoredObject* key) {
    pthread_rwlock_rdlock(&tbl->lock);
    int idx = find_key_index(tbl, key);
    StoredObject* result = (idx >= 0) ? tbl->entries.vals[idx] : NULL;
    pthread_rwlock_unlock(&tbl->lock);
    return result;
}

void shared_table_delete(SharedTable* tbl, StoredObject* key) {
    pthread_rwlock_wrlock(&tbl->lock);
    int idx = find_key_index(tbl, key);
    if (idx >= 0) {
        // 释放键和值的引用
        gc_release((GCObject*)tbl->entries.keys[idx]);
        gc_release((GCObject*)tbl->entries.vals[idx]);
        // 将最后一个元素移到当前位置
        tbl->entries.keys[idx] = tbl->entries.keys[tbl->entries.size - 1];
        tbl->entries.vals[idx] = tbl->entries.vals[tbl->entries.size - 1];
        tbl->entries.size--;
    }
    pthread_rwlock_unlock(&tbl->lock);
}

size_t shared_table_size(SharedTable* tbl) {
    pthread_rwlock_rdlock(&tbl->lock);
    size_t sz = tbl->entries.size;
    pthread_rwlock_unlock(&tbl->lock);
    return sz;
}

size_t shared_table_length(SharedTable* tbl) {
    pthread_rwlock_rdlock(&tbl->lock);
    // 找出所有整数键
    size_t max = 0;
    for (size_t i = 0; i < tbl->entries.size; i++) {
        StoredObject* key = tbl->entries.keys[i];
        if (key->type == STORED_INTEGER) {
            lua_Integer n = key->data.integer_val;
            if (n > 0) {
                if ((size_t)n > max) max = (size_t)n;
            }
        }
    }
    // 检查连续段
    size_t len = 0;
    for (size_t i = 1; i <= max; i++) {
        StoredObject tmp;
        tmp.type = STORED_INTEGER;
        tmp.data.integer_val = i;
        int found = 0;
        for (size_t j = 0; j < tbl->entries.size; j++) {
            if (stored_compare(tbl->entries.keys[j], &tmp) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) break;
        len++;
    }
    pthread_rwlock_unlock(&tbl->lock);
    return len;
}

SharedTablePair shared_table_next(SharedTable* tbl, StoredObject* key) {
    SharedTablePair result = {NULL, NULL};
    pthread_rwlock_rdlock(&tbl->lock);
    if (tbl->entries.size == 0) goto out;

    if (key == NULL) {
        // 从头开始
        result.key = tbl->entries.keys[0];
        result.val = tbl->entries.vals[0];
    } else {
        // 找到key之后的下一个（假设key存在于表中，否则返回第一个？简化：线性查找）
        int start = find_key_index(tbl, key);
        if (start >= 0 && start + 1 < (int)tbl->entries.size) {
            result.key = tbl->entries.keys[start + 1];
            result.val = tbl->entries.vals[start + 1];
        }
    }
out:
    pthread_rwlock_unlock(&tbl->lock);
    return result;
}

void shared_table_set_metatable(SharedTable* tbl, StoredObject* mt) {
    pthread_rwlock_wrlock(&tbl->lock);
    if (tbl->metatable)
        gc_release((GCObject*)tbl->metatable);
    tbl->metatable = mt;
    if (mt)
        gc_add_reference((GCObject*)tbl, (GCObject*)mt);
    pthread_rwlock_unlock(&tbl->lock);
}

StoredObject* shared_table_get_metatable(SharedTable* tbl) {
    pthread_rwlock_rdlock(&tbl->lock);
    StoredObject* mt = tbl->metatable;
    pthread_rwlock_unlock(&tbl->lock);
    return mt;
}

// ---------- Lua 绑定 ----------

const char* SHARED_TABLE_MT = "xshare.shared_table";

// 辅助：从栈上获取SharedTable*（userdata）
SharedTable* check_shared_table(lua_State* L, int idx) {
    void* ud = luaL_checkudata(L, idx, SHARED_TABLE_MT);
    luaL_argcheck(L, ud != NULL, idx, "xshare.table expected");
    return *(SharedTable**)ud;
}

// 构造函数：xshare.table([tbl]) -> userdata
int l_shared_table_new(lua_State* L) {
    GC* gc = gc_instance();
    SharedTable* st = shared_table_create(gc);
    if (!st) return luaL_error(L, "cannot create shared table");

    // 创建userdata，存储指针
    SharedTable** ud = (SharedTable**)lua_newuserdata(L, sizeof(SharedTable*));
    *ud = st;
    luaL_setmetatable(L, SHARED_TABLE_MT);
    gc_retain((GCObject*)st);   // 增加引用

    // 如果提供了初始化表，则复制内容
    if (lua_gettop(L) >= 1 && !lua_isnil(L, 1)) {
        if (!lua_istable(L, 1))
            return luaL_argerror(L, 1, "table expected");
        // 遍历表并设置
        lua_pushnil(L);
        while (lua_next(L, 1)) {
            // 键在-2，值在-1
            StoredObject* key = stored_create(L, -2);
            StoredObject* val = stored_create(L, -1);
            if (!key || !val) {
                // 清理
                if (key) gc_release((GCObject*)key);
                if (val) gc_release((GCObject*)val);
                return luaL_error(L, "cannot store table entry");
            }
            if (!shared_table_set(st, key, val)) {
                gc_release((GCObject*)key);
                gc_release((GCObject*)val);
                return luaL_error(L, "failed to set table entry (out of memory)");
            }
            gc_release((GCObject*)key);
            gc_release((GCObject*)val);
            lua_pop(L, 1); // 弹出值，保留键
        }
    }
    return 1;
}

// __index 元方法
int l_shared_table_index(lua_State* L) {
    SharedTable* tbl = check_shared_table(L, 1);
    StoredObject* key = stored_create(L, 2);
    if (!key) return luaL_error(L, "invalid key");

    StoredObject* val = shared_table_get(tbl, key);
    gc_release((GCObject*)key);

    if (val) {
        stored_push(L, val);
        return 1;
    }

    // 检查元表的__index
    StoredObject* mt = shared_table_get_metatable(tbl);
    if (mt && mt->type == STORED_SHARED_TABLE) {
        SharedTable* mttbl = mt->data.shared_table;
        StoredObject* mtkey = stored_create(L, 2);
        if (!mtkey) return luaL_error(L, "invalid key for metatable");
        StoredObject* mtval = shared_table_get(mttbl, mtkey);
        gc_release((GCObject*)mtkey);
        if (mtval) {
            if (mtval->type == STORED_FUNCTION) {
                // 调用函数
                stored_push(L, mtval);
                lua_pushvalue(L, 1); // self
                lua_pushvalue(L, 2); // key
                lua_call(L, 2, LUA_MULTRET);
                return lua_gettop(L) - 3; // 减去栈上的self,key,func
            } else {
                stored_push(L, mtval);
                return 1;
            }
        }
    }
    lua_pushnil(L);
    return 1;
}

// __newindex 元方法
int l_shared_table_newindex(lua_State* L) {
    SharedTable* tbl = check_shared_table(L, 1);
    StoredObject* key = stored_create(L, 2);
    StoredObject* val = stored_create(L, 3);
    if (!key || !val) {
        if (key) gc_release((GCObject*)key);
        if (val) gc_release((GCObject*)val);
        return luaL_error(L, "invalid key or value");
    }

    // 检查元表的__newindex
    StoredObject* mt = shared_table_get_metatable(tbl);
    if (mt && mt->type == STORED_SHARED_TABLE) {
        SharedTable* mttbl = mt->data.shared_table;
        StoredObject* mtkey = stored_create(L, 2);
        if (mtkey) {
            StoredObject* mtfunc = shared_table_get(mttbl, mtkey);
            if (mtfunc && mtfunc->type == STORED_FUNCTION) {
                // 调用函数
                stored_push(L, mtfunc);
                lua_pushvalue(L, 1); // self
                lua_pushvalue(L, 2); // key
                lua_pushvalue(L, 3); // value
                lua_call(L, 3, 0);
                gc_release((GCObject*)key);
                gc_release((GCObject*)val);
                return 0;
            }
        }
    }

    if (val->type == STORED_NIL) {
        shared_table_delete(tbl, key);
    } else {
        if (!shared_table_set(tbl, key, val)) {
            gc_release((GCObject*)key);
            gc_release((GCObject*)val);
            return luaL_error(L, "failed to set table entry (out of memory)");
        }
    }
    gc_release((GCObject*)key);
    gc_release((GCObject*)val);
    return 0;
}

// __len 元方法
int l_shared_table_len(lua_State* L) {
    SharedTable* tbl = check_shared_table(L, 1);
    size_t len = shared_table_length(tbl);
    lua_pushinteger(L, len);
    return 1;
}

// next 辅助函数（用于pairs）
int l_shared_table_next(lua_State* L) {
    SharedTable* tbl = check_shared_table(L, 1);
    StoredObject* key = NULL;
    if (!lua_isnil(L, 2)) {
        key = stored_create(L, 2);
        if (!key) return luaL_error(L, "invalid key");
    }
    SharedTablePair pair = shared_table_next(tbl, key);
    if (key) gc_release((GCObject*)key);
    if (pair.key) {
        stored_push(L, pair.key);
        stored_push(L, pair.val);
        return 2;
    } else {
        return 0;
    }
}

// __pairs 元方法
int l_shared_table_pairs(lua_State* L) {
    SharedTable* tbl = check_shared_table(L, 1);
    // 返回迭代函数、表、初始nil
    lua_pushcfunction(L, l_shared_table_next);
    lua_pushvalue(L, 1);
    lua_pushnil(L);
    return 3;
}

int l_shared_table_ipairs_next(lua_State* L) {
    SharedTable* tbl = check_shared_table(L, 1);
    lua_Integer i = luaL_checkinteger(L, 2) + 1;
    StoredObject key;
    key.type = STORED_INTEGER;
    key.data.integer_val = i;
    StoredObject* val = shared_table_get(tbl, &key);
    if (val) {
        lua_pushinteger(L, i);
        stored_push(L, val);
        return 2;
    } else {
        return 0;
    }
}

// __ipairs 元方法
int l_shared_table_ipairs(lua_State* L) {
    SharedTable* tbl = check_shared_table(L, 1);
    lua_pushcfunction(L, l_shared_table_ipairs_next);
    lua_pushvalue(L, 1);
    lua_pushinteger(L, 0);
    return 3;
}

// __tostring 元方法
int l_shared_table_tostring(lua_State* L) {
    SharedTable* tbl = check_shared_table(L, 1);
    lua_pushfstring(L, "xshare.table: %p", tbl);
    return 1;
}

// xshare.setmetatable(tbl, mt)
int l_shared_table_setmetatable(lua_State* L) {
    SharedTable* tbl = check_shared_table(L, 1);
    StoredObject* mt = NULL;
    if (!lua_isnil(L, 2)) {
        // 如果第二个参数是普通表，先转换为SharedTable
        if (lua_istable(L, 2)) {
            // 创建新的SharedTable并从表复制内容
            GC* gc = gc_instance();
            SharedTable* mtbl = shared_table_create(gc);
            if (!mtbl) return luaL_error(L, "cannot create metatable");
            // 遍历并复制...
            lua_pushnil(L);
            while (lua_next(L, 2)) {
                StoredObject* k = stored_create(L, -2);
                StoredObject* v = stored_create(L, -1);
                if (k && v) {
                    if (!shared_table_set(mtbl, k, v)) {
                        gc_release((GCObject*)k);
                        gc_release((GCObject*)v);
                        return luaL_error(L, "failed to set table entry (out of memory)");
                    }
                    gc_release((GCObject*)k);
                    gc_release((GCObject*)v);
                } else {
                    if (k) gc_release((GCObject*)k);
                    if (v) gc_release((GCObject*)v);
                }
                lua_pop(L, 1);
            }
            mt = stored_create_from_sharedtable(mtbl);
            gc_release((GCObject*)mtbl); // 释放临时引用，mt持有新引用
        } else {
            // 假设已经是xshare.table userdata
            SharedTable* mtbl = check_shared_table(L, 2);
            mt = stored_create_from_sharedtable(mtbl);
        }
    }
    shared_table_set_metatable(tbl, mt);
    if (mt) gc_release((GCObject*)mt);
    lua_pushvalue(L, 1);
    return 1;
}

// xshare.getmetatable(tbl)
int l_shared_table_getmetatable(lua_State* L) {
    SharedTable* tbl = check_shared_table(L, 1);
    StoredObject* mt = shared_table_get_metatable(tbl);
    if (mt && mt->type == STORED_SHARED_TABLE) {
        SharedTable** ud = (SharedTable**)lua_newuserdata(L, sizeof(SharedTable*));
        *ud = mt->data.shared_table;
        luaL_setmetatable(L, SHARED_TABLE_MT);
        gc_retain((GCObject*)mt->data.shared_table);   // 增加引用
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

// xshare.rawset(tbl, key, value)
int l_shared_table_rawset(lua_State* L) {
    SharedTable* tbl = check_shared_table(L, 1);
    StoredObject* key = stored_create(L, 2);
    StoredObject* val = stored_create(L, 3);
    if (!key || !val) {
        if (key) gc_release((GCObject*)key);
        if (val) gc_release((GCObject*)val);
        return luaL_error(L, "invalid key or value");
    }
    if (val->type == STORED_NIL) {
        shared_table_delete(tbl, key);
    } else {
        if (!shared_table_set(tbl, key, val)) {
            gc_release((GCObject*)key);
            gc_release((GCObject*)val);
            return luaL_error(L, "failed to set table entry (out of memory)");
        }
    }
    gc_release((GCObject*)key);
    gc_release((GCObject*)val);
    lua_pushvalue(L, 1);
    return 1;
}

// xshare.rawget(tbl, key)
int l_shared_table_rawget(lua_State* L) {
    SharedTable* tbl = check_shared_table(L, 1);
    StoredObject* key = stored_create(L, 2);
    if (!key) return luaL_error(L, "invalid key");
    StoredObject* val = shared_table_get(tbl, key);
    gc_release((GCObject*)key);
    if (val) {
        stored_push(L, val);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

// xshare.size(tbl)
int l_shared_table_size(lua_State* L) {
    SharedTable* tbl = check_shared_table(L, 1);
    size_t sz = shared_table_size(tbl);
    lua_pushinteger(L, sz);
    return 1;
}

int l_shared_table_gc(lua_State* L) {
    SharedTable** ud = (SharedTable**)lua_touserdata(L, 1);
    if (*ud) {
        gc_release((GCObject*)(*ud));
        *ud = NULL;
    }
    return 0;
}