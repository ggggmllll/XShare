// stored_object.c
#include "stored_object.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "lauxlib.h"  // 用于luaL_loadbuffer

// visited链表节点（用于递归时防止循环）
typedef struct VisitedNode {
    const void* ptr;               // lua_topointer得到的唯一标识
    StoredObject* obj;             // 已经创建好的StoredObject
    struct VisitedNode* next;
} VisitedNode;

// 查找visited链表中是否已有对应Lua对象
static StoredObject* find_visited(VisitedNode* head, const void* ptr) {
    for (; head; head = head->next) {
        if (head->ptr == ptr) return head->obj;
    }
    return NULL;
}

// 内部writer，用于lua_dump收集字节码到动态缓冲区
typedef struct Buffer {
    char* data;
    size_t size;
    size_t capacity;
} Buffer;

static int writer(lua_State* L, const void* p, size_t sz, void* ud) {
    (void)L;
    Buffer* buf = (Buffer*)ud;
    if (buf->size + sz > buf->capacity) {
        size_t newcap = buf->capacity ? buf->capacity * 2 : 1024;
        while (newcap < buf->size + sz) newcap *= 2;
        char* newdata = realloc(buf->data, newcap);
        if (!newdata) return 1;  // 内存不足
        buf->data = newdata;
        buf->capacity = newcap;
    }
    memcpy(buf->data + buf->size, p, sz);
    buf->size += sz;
    return 0;
}

// 对象析构函数，由GC在回收时调用
static void stored_dtor(GCObject* obj) {
    StoredObject* sobj = (StoredObject*)obj;
    switch (sobj->type) {
        case STORED_STRING:
            free(sobj->data.string_val);
            break;
        case STORED_FUNCTION: {
            FunctionData* f = sobj->data.func_data;
            if (f) {
                for (int i = 0; i < f->upvalue_count; i++) {
                    if (f->upvalues[i]) {
                        gc_release((GCObject*)f->upvalues[i]);
                    }
                }
                free(f->bytecode);
                free(f);
            }
            break;
        }
        case STORED_TABLE_COPY: {
            TableCopy* tc = sobj->data.table_copy;
            if (tc) {
                for (size_t i = 0; i < tc->size; i++) {
                    gc_release((GCObject*)tc->keys[i]);
                    gc_release((GCObject*)tc->vals[i]);
                }
                free(tc->keys);
                free(tc->vals);
                free(tc);
            }
            break;
        }
        case STORED_SHARED_TABLE:
            gc_release((GCObject*)sobj->data.shared_table);
            break;

        default:
            break;
    }
}

// 核心递归创建函数
static StoredObject* stored_create_impl(lua_State* L, int idx, VisitedNode** visited) {
    int type = lua_type(L, idx);
    GC* gc = gc_instance();

    // 先检查visited（只对需要递归的类型有效）
    if (type == LUA_TFUNCTION && !lua_iscfunction(L, idx)) {
        const void* ptr = lua_topointer(L, idx);
        StoredObject* found = find_visited(*visited, ptr);
        if (found) return found;
    }

    // 分配对象内存（通过GC）
    StoredObject* sobj = (StoredObject*)gc_create(gc, sizeof(StoredObject) - sizeof(GCObject));
    if (!sobj) return NULL;
    sobj->header.dtor = stored_dtor;

    switch (type) {
        case LUA_TNIL:
            sobj->type = STORED_NIL;
            break;
        case LUA_TBOOLEAN:
            sobj->type = STORED_BOOLEAN;
            sobj->data.boolean_val = lua_toboolean(L, idx);
            break;
        case LUA_TNUMBER:
#if LUA_VERSION_NUM >= 503
            if (lua_isinteger(L, idx)) {
                sobj->type = STORED_INTEGER;
                sobj->data.integer_val = lua_tointeger(L, idx);
            } else
#endif
            {
                sobj->type = STORED_NUMBER;
                sobj->data.number_val = lua_tonumber(L, idx);
            }
            break;
        case LUA_TSTRING: {
            size_t len;
            const char* s = lua_tolstring(L, idx, &len);
            sobj->type = STORED_STRING;
            sobj->data.string_val = malloc(len + 1);
            if (!sobj->data.string_val) goto fail;
            memcpy(sobj->data.string_val, s, len + 1);
            sobj->string_len = len;
            break;
        }
        case LUA_TLIGHTUSERDATA:
            sobj->type = STORED_LIGHTUSERDATA;
            sobj->data.lightuserdata_val = lua_touserdata(L, idx);
            break;
        case LUA_TFUNCTION:
            if (lua_iscfunction(L, idx)) {
                sobj->type = STORED_CFUNCTION;
                sobj->data.cfunction_val = lua_tocfunction(L, idx);
            } else {
                // Lua函数：需要存储字节码和upvalues
                // 先加入visited
                VisitedNode cur = { lua_topointer(L, idx), sobj, *visited };
                *visited = &cur;

                // 获取upvalue数量
                lua_Debug ar;
                lua_pushvalue(L, idx);   // 复制函数到栈顶供lua_getinfo使用
                if (lua_getinfo(L, ">u", &ar) == 0) {
                    lua_pop(L, 1);
                    goto fail;
                }
                int nup = ar.nups;
                lua_pop(L, 1);            // 弹出被lua_getinfo消耗的副本

                // 分配FunctionData
                FunctionData* fdata = calloc(1, sizeof(FunctionData) + nup * sizeof(StoredObject*));
                if (!fdata) goto fail;
                fdata->upvalue_count = nup;
                // 获取全局表指针（用于比较）
                #if LUA_VERSION_NUM >= 502
                    lua_pushglobaltable(L);
                #else
                    lua_pushvalue(L, LUA_GLOBALSINDEX);
                #endif
                const void* g_ptr = lua_topointer(L, -1);
                lua_pop(L, 1);   // 立即弹出

                // 获取字节码
                Buffer buf = {NULL, 0, 0};
                lua_pushvalue(L, idx);    // 再次复制函数
                if (lua_dump(L, writer, &buf, 0) != 0) {
                    lua_pop(L, 1);
                    free(buf.data);
                    free(fdata);
                    goto fail;
                }
                lua_pop(L, 1);             // 弹出函数副本
                fdata->bytecode = buf.data;
                fdata->bytecode_len = buf.size;

                // 获取upvalues
                lua_pushvalue(L, idx);     // 将函数压栈以便遍历upvalues
                int created_upvalues = 0;
                for (int i = 1; i <= nup; i++) {
                    const char* name = lua_getupvalue(L, -1, i);
                    (void)name;

                    // 检查是否为环境
                    const void* up_ptr = lua_topointer(L, -1);
                    if (up_ptr == g_ptr) {
                        lua_pop(L, 1);               // 弹出 upvalue 值
                        fdata->env_upvalue_pos = (unsigned char)i;   // 记录位置
                        fdata->upvalues[i-1] = NULL; // 留空
                        continue;
                    }

                    StoredObject* upval = stored_create_impl(L, -1, visited);
                    lua_pop(L, 1);                   // 弹出 upvalue 值
                    if (!upval) {
                        // 释放已创建的 upvalues
                        for (int j = 0; j < created_upvalues; j++) {
                            gc_release((GCObject*)fdata->upvalues[j]);
                        }
                        free(fdata->bytecode);
                        free(fdata);
                        lua_pop(L, 1);                // 弹出函数
                        goto fail;
                    }
                    fdata->upvalues[i-1] = upval;
                    created_upvalues++;
                    gc_add_reference((GCObject*)sobj, (GCObject*)upval);
                }
                lua_pop(L, 1);               // 弹出函数

                sobj->type = STORED_FUNCTION;
                sobj->data.func_data = fdata;

                // 从visited中移除当前节点
                *visited = cur.next;
            }
            break;
        case LUA_TTABLE: {
            const void* ptr = lua_topointer(L, idx);
            StoredObject* found = find_visited(*visited, ptr);
            if (found) return found;

            sobj->type = STORED_TABLE_COPY;
            sobj->data.table_copy = NULL;               // ① 先置空
            VisitedNode cur = { ptr, sobj, *visited };
            *visited = &cur;

            TableCopy* tc = calloc(1, sizeof(TableCopy));
            if (!tc) goto fail;
            tc->capacity = 4;
            tc->keys = malloc(tc->capacity * sizeof(StoredObject*));
            tc->vals = malloc(tc->capacity * sizeof(StoredObject*));
            if (!tc->keys || !tc->vals) {
                // 即使部分分配失败，也交给析构函数统一释放
                sobj->data.table_copy = tc;              // ② 尽早赋值
                goto fail;
            }
            sobj->data.table_copy = tc;                  // ③ 再次赋值（实际已赋过，但安全）

            lua_pushnil(L);
            while (lua_next(L, idx)) {
                StoredObject* key = stored_create_impl(L, -2, visited);
                StoredObject* val = stored_create_impl(L, -1, visited);
                lua_pop(L, 1);
                if (!key || !val) {
                    if (key) gc_release((GCObject*)key);
                    if (val) gc_release((GCObject*)val);
                    goto fail;                           // ④ 不再手动释放 tc
                }

                if (tc->size >= tc->capacity) {
                    size_t newcap = tc->capacity * 2;
                    StoredObject** newkeys = realloc(tc->keys, newcap * sizeof(StoredObject*));
                    StoredObject** newvals = realloc(tc->vals, newcap * sizeof(StoredObject*));
                    if (!newkeys || !newvals) {
                        gc_release((GCObject*)key);
                        gc_release((GCObject*)val);
                        goto fail;                       // ⑤ 直接失败，由析构清理
                    }
                    tc->keys = newkeys;
                    tc->vals = newvals;
                    tc->capacity = newcap;
                }
                tc->keys[tc->size] = key;
                tc->vals[tc->size] = val;
                tc->size++;
                gc_add_reference((GCObject*)sobj, (GCObject*)key);
                gc_add_reference((GCObject*)sobj, (GCObject*)val);
            }
            *visited = cur.next;
            break;
        }
        case LUA_TUSERDATA: {
            // 检查是否为共享表
            SharedTable** stp = (SharedTable**)luaL_testudata(L, idx, SHARED_TABLE_MT);
            if (stp && *stp) {
                return stored_create_from_sharedtable(*stp);
            }
            // 其他userdata不支持
            luaL_error(L, "cannot store userdata of unknown type");
            return NULL;
        }
        default:
            // 不支持的类型（表、userdata等）
            goto fail;
    }

    return sobj;

fail:
    // 分配失败时，需要释放sobj本身（GC会负责，但尚未加入strongRefs，可直接free）
    // 但sobj是通过gc_create分配的，已加入GC链表，所以不能直接free。将其refCount设为0？最好调用gc_release？但这里简化：直接返回NULL，让调用者处理。
    // 更安全的做法是调用gc_release(sobj)来释放，但需要先将其refCount设为1（目前已经是1）。我们直接调用gc_release。
    gc_release((GCObject*)sobj);  // 减少引用计数，下次GC会回收
    return NULL;
}

// 对外接口
StoredObject* stored_create(lua_State* L, int index) {
    VisitedNode* visited = NULL;
    return stored_create_impl(L, index, &visited);
}

void stored_push_impl(lua_State* L, StoredObject* obj) {
    GC* gc = gc_instance();

    if (!obj) {
        lua_pushnil(L);
        return;
    }

    switch (obj->type) {
        case STORED_NIL:
            lua_pushnil(L);
            break;
        case STORED_BOOLEAN:
            lua_pushboolean(L, obj->data.boolean_val);
            break;
        case STORED_NUMBER:
            lua_pushnumber(L, obj->data.number_val);
            break;
        case STORED_INTEGER:
#if LUA_VERSION_NUM >= 503
            lua_pushinteger(L, obj->data.integer_val);
#else
            lua_pushnumber(L, obj->data.integer_val);
#endif
            break;
        case STORED_STRING:
            lua_pushlstring(L, obj->data.string_val, obj->string_len);
            break;
        case STORED_LIGHTUSERDATA:
            lua_pushlightuserdata(L, obj->data.lightuserdata_val);
            break;
        case STORED_CFUNCTION:
            lua_pushcfunction(L, obj->data.cfunction_val);
            break;
        case STORED_FUNCTION: {
            FunctionData* f = obj->data.func_data;
            if (luaL_loadbuffer(L, f->bytecode, f->bytecode_len, "=stored") != LUA_OK) {
                lua_pushnil(L);
                break;
            }
            for (int i = 1; i <= f->upvalue_count; i++) {
                if (i == f->env_upvalue_pos) {
                    // 环境：压入目标线程的全局表
        #if LUA_VERSION_NUM >= 502
                    lua_pushglobaltable(L);
        #else
                    lua_pushvalue(L, LUA_GLOBALSINDEX);
        #endif
                } else {
                    StoredObject* upval = f->upvalues[i-1];
                    stored_push_impl(L, upval);   // 递归调用无锁版本
                }
                lua_setupvalue(L, -2, i);
            }
            break;
        }
        case STORED_TABLE_COPY: {
            TableCopy* tc = obj->data.table_copy;
            lua_newtable(L);
            for (size_t i = 0; i < tc->size; i++) {
                stored_push_impl(L, tc->keys[i]);
                stored_push_impl(L, tc->vals[i]);
                lua_settable(L, -3);
            }
            break;
        }
        case STORED_SHARED_TABLE: {
            SharedTable* st = obj->data.shared_table;
            SharedTable** ud = (SharedTable**)lua_newuserdata(L, sizeof(SharedTable*));
            *ud = st;
            luaL_getmetatable(L, SHARED_TABLE_MT);
            lua_setmetatable(L, -2);
            gc_retain((GCObject*)st);   // userdata 持有引用
            break;
        }
        default:
            lua_pushnil(L);
            break;
    }
}

void stored_push(lua_State* L, StoredObject* obj) {
    GC* gc = gc_instance();
    pthread_rwlock_rdlock(&gc->rwlock);
    stored_push_impl(L, obj);
    pthread_rwlock_unlock(&gc->rwlock);
}

// stored_compare 实现
int stored_compare(const StoredObject* a, const StoredObject* b) {
    if (a->type != b->type) return (a->type < b->type) ? -1 : 1;
    switch (a->type) {
        case STORED_NIL: return 0;
        case STORED_BOOLEAN: return (a->data.boolean_val == b->data.boolean_val) ? 0 :
                                    (a->data.boolean_val ? 1 : -1);
        case STORED_NUMBER:
            if (a->data.number_val < b->data.number_val) return -1;
            if (a->data.number_val > b->data.number_val) return 1;
            return 0;
        case STORED_INTEGER:
            if (a->data.integer_val < b->data.integer_val) return -1;
            if (a->data.integer_val > b->data.integer_val) return 1;
            return 0;
        case STORED_STRING: {
            size_t min = (a->string_len < b->string_len) ? a->string_len : b->string_len;
            int cmp = memcmp(a->data.string_val, b->data.string_val, min);
            if (cmp != 0) return cmp;
            if (a->string_len < b->string_len) return -1;
            if (a->string_len > b->string_len) return 1;
            return 0;
        }
        case STORED_LIGHTUSERDATA:
            return (a->data.lightuserdata_val < b->data.lightuserdata_val) ? -1 :
                   (a->data.lightuserdata_val > b->data.lightuserdata_val) ? 1 : 0;
        case STORED_CFUNCTION: {
            uintptr_t pa = (uintptr_t)(void*)a->data.cfunction_val;
            uintptr_t pb = (uintptr_t)(void*)b->data.cfunction_val;
            return (pa < pb) ? -1 : (pa > pb) ? 1 : 0;
        }
        case STORED_FUNCTION: {
            // 简化为指针比较（因为函数实例可能重复，但这里接受）
            uintptr_t pa = (uintptr_t)a->data.func_data;
            uintptr_t pb = (uintptr_t)b->data.func_data;
            return (pa < pb) ? -1 : (pa > pb) ? 1 : 0;
        }
        case STORED_SHARED_TABLE: {
            // 比较SharedTable指针（实际应比较句柄，但此处简化为指针）
            uintptr_t pa = (uintptr_t)a->data.shared_table;
            uintptr_t pb = (uintptr_t)b->data.shared_table;
            return (pa < pb) ? -1 : (pa > pb) ? 1 : 0;
        }
        case STORED_TABLE_COPY: {
            uintptr_t pa = (uintptr_t)a->data.table_copy;
            uintptr_t pb = (uintptr_t)b->data.table_copy;
            return (pa < pb) ? -1 : (pa > pb) ? 1 : 0;
        }
        default: return 0;
    }
}

StoredObject* stored_create_from_sharedtable(SharedTable* st) {
    GC* gc = gc_instance();
    StoredObject* sobj = (StoredObject*)gc_create(gc, sizeof(StoredObject) - sizeof(GCObject));
    if (!sobj) return NULL;
    sobj->header.dtor = stored_dtor;
    sobj->type = STORED_SHARED_TABLE;
    sobj->data.shared_table = st;
    gc_add_reference((GCObject*)sobj, (GCObject*)st);   // StoredObject持有引用
    return sobj;
}