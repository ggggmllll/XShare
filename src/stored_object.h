// stored_object.h
#ifndef STORED_OBJECT_H
#define STORED_OBJECT_H

#include <lua.h>
#include "GC.h"  // 包含之前实现的GC头文件

extern const char* SHARED_TABLE_MT;

typedef enum {
    STORED_NIL,
    STORED_BOOLEAN,
    STORED_NUMBER,
    STORED_INTEGER,
    STORED_STRING,
    STORED_LIGHTUSERDATA,
    STORED_CFUNCTION,
    STORED_FUNCTION,
    STORED_TABLE_COPY,
    STORED_SHARED_TABLE
} StoredType;

typedef struct FunctionData {
    char* bytecode;
    size_t bytecode_len;
    int upvalue_count;
    unsigned char env_upvalue_pos;  // 0 表示无环境 upvalue
    struct StoredObject* upvalues[];  // 灵活数组
} FunctionData;

typedef struct SharedTable SharedTable;
typedef struct TableCopy TableCopy;

typedef struct StoredObject {
    GCObject header;      // GC头，必须为第一个成员
    StoredType type;
    union {
        int boolean_val;
        lua_Number number_val;
        lua_Integer integer_val;
        char* string_val;
        void* lightuserdata_val;
        lua_CFunction cfunction_val;
        FunctionData* func_data;
        TableCopy* table_copy;
        SharedTable* shared_table;   // 存储SharedTable指针
    } data;
    size_t string_len;    // 仅当type为STRING时有效
} StoredObject;

struct TableCopy {
    StoredObject** keys;
    StoredObject** vals;
    size_t size;
    size_t capacity;
};

// 从Lua栈上指定索引处创建StoredObject（可能递归）
StoredObject* stored_create(lua_State* L, int index);

// 将StoredObject推回Lua栈
void stored_push(lua_State* L, StoredObject* obj);

// 比较两个StoredObject（用于查找键）
int stored_compare(const StoredObject* a, const StoredObject* b);

// 创建一个包装SharedTable的StoredObject（增加对SharedTable的引用）
StoredObject* stored_create_from_sharedtable(SharedTable* st);

#endif