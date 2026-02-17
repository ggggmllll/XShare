[**中文**](README.md) | [**English**](README_en.md)

# XShare - Cross-Thread Data Sharing Library

XShare is a lightweight cross‑thread data sharing library for Lua. It allows safe passing and sharing of data, including primitive types, tables, and functions, across multiple independent Lua states. The library integrates a custom tri‑color mark‑and‑sweep garbage collector that automatically manages memory. XShare is ideal for multi‑threaded Lua applications such as concurrent task processing and data exchange.

## Features

- Thread‑safe shared tables (`xshare.table`)
- Supports passing of primitive types, functions, and tables across threads (deep copy or sharing)
- Custom tri‑color mark‑and‑sweep GC that automatically reclaims cyclic references
- Provides both C API and Lua API for easy integration
- Compatible with Lua 5.1, 5.2, 5.3 and LuaJIT

## Compilation and Installation

### Dependencies
- Lua development libraries (5.1+)
- CMake 3.10+
- pthread (for multi‑threading support)

### Compilation
```bash
git clone <repository>
cd XShare
mkdir build && cd build
cmake ..
make
```

### Installation
```bash
make install
```

After installation, you can load the module in Lua via `require("xshare")` and link against `libxshare.a` in your C code, including the necessary headers.

## C API Reference

The C API is defined in the following headers:
- `GC.h` – core garbage collector
- `stored_object.h` – serialisation of storable objects
- `shared_table.h` – shared table operations

### GC Management

```c
GC* gc_instance(void);
```
Returns the global GC instance pointer.

```c
GCObject* gc_create(GC* gc, size_t data_size);
```
Creates a new GC object and returns a pointer. `data_size` is the size of the actual object data (excluding the `GCObject` header). The new object’s reference count is initialised to 1.

```c
void gc_retain(GCObject* obj);
void gc_release(GCObject* obj);
```
Increments/decrements the object’s reference count. Objects with a count of zero will be reclaimed during the next GC cycle.

```c
void gc_add_reference(GCObject* from, GCObject* to);
void gc_remove_reference(GCObject* from, GCObject* to);
```
Establishes/removes a strong reference between `from` and `to`. These relationships are used during GC marking.

```c
void gc_collect(GC* gc);
```
Performs a full mark‑and‑sweep collection.

```c
void gc_pause(GC* gc);
void gc_resume(GC* gc);
int gc_enabled(GC* gc);
```
Pauses/resumes automatic GC and queries its current state.

```c
void gc_set_step(GC* gc, double step);
double gc_get_step(GC* gc);
int gc_count(GC* gc);
```
Sets/gets the GC trigger threshold coefficient (`count >= lastCleanup * step`) and returns the total number of objects currently managed.

### StoredObject Serialisation

`StoredObject` is a cross‑thread representation of Lua values. Supported types: nil, boolean, number, integer, string, lightuserdata, C function, Lua function, deep‑copied table, and shared table.

```c
StoredObject* stored_create(lua_State* L, int index);
```
Creates a `StoredObject` from the Lua value at the given stack index. The returned object must eventually be released with `gc_release`.

```c
void stored_push(lua_State* L, StoredObject* obj);
```
Pushes the `StoredObject` back onto the Lua stack, restoring its original value.

```c
int stored_compare(const StoredObject* a, const StoredObject* b);
```
Compares two `StoredObject`s for ordering (e.g., for key lookup in `SharedTable`). Returns <0 if a < b, 0 if equal, >0 if a > b.

```c
StoredObject* stored_create_from_sharedtable(SharedTable* st);
```
Wraps a shared table into a `StoredObject`, allowing a shared table to be stored inside another shared table.

### SharedTable Operations

`SharedTable` is a thread‑safe shared table that can be accessed concurrently by multiple Lua states.

```c
SharedTable* shared_table_create(GC* gc);
```
Creates a new empty shared table.

```c
int shared_table_set(SharedTable* tbl, StoredObject* key, StoredObject* val);
```
Sets the key–value pair. Returns 1 on success, 0 on failure (out of memory). The function automatically adds references to `key` and `val`.

```c
StoredObject* shared_table_get(SharedTable* tbl, StoredObject* key);
```
Returns the value associated with `key`. The returned `StoredObject*` belongs to the table and must not be released by the caller. Returns NULL if the key does not exist.

```c
void shared_table_delete(SharedTable* tbl, StoredObject* key);
```
Deletes the entry for `key` and releases the references held.

```c
size_t shared_table_size(SharedTable* tbl);
size_t shared_table_length(SharedTable* tbl);
```
Returns the total number of entries and the length of the array part (the longest consecutive integer‑key sequence starting at 1).

```c
SharedTablePair shared_table_next(SharedTable* tbl, StoredObject* key);
```
Iterator. If `key` is NULL, returns the first key–value pair; otherwise returns the pair after `key`. The returned `SharedTablePair` contains internal pointers that must not be released.

```c
void shared_table_set_metatable(SharedTable* tbl, StoredObject* mt);
StoredObject* shared_table_get_metatable(SharedTable* tbl);
```
Sets/gets the metatable. `mt` must be a `StoredObject` wrapping another shared table.

## Lua API Reference

The Lua module is named `xshare` and is loaded via `require("xshare")`. It returns a table with the following functions.

### `xshare.table([tbl])`
Creates a new shared table. If a Lua table `tbl` is provided, all its fields are deep‑copied into the new shared table (ordinary tables become `STORED_TABLE_COPY`; shared tables are referenced).

**Parameters:** `tbl` (optional) – a regular Lua table  
**Returns:** a shared table userdata

### Metamethods for Shared Tables
Shared tables support the following metamethods, which behave like their Lua counterparts:
- `__index`, `__newindex`, `__len`, `__pairs`, `__ipairs`, `__tostring`

Thus you can use standard table syntax:
```lua
local t = xshare.table()
t.key = "value"
print(t.key)      -- value
print(#t)         -- 0
```

### Metatable Operations
```lua
xshare.setmetatable(tbl, mt)
xshare.getmetatable(tbl)
```
Sets/gets the metatable of a shared table. `mt` can be a regular Lua table (automatically converted to a shared table) or another shared table.

### Raw Access
```lua
xshare.rawset(tbl, key, value)
xshare.rawget(tbl, key)
```
Sets/gets a value without invoking the `__index`/`__newindex` metamethods. Equivalent to Lua’s `rawset`/`rawget`.

### `xshare.size(tbl)`
Returns the number of entries in a shared table (equivalent to counting with `pairs`, but more efficient).

### GC Control
```lua
xshare.gc.collect()          -- manually trigger a GC cycle
xshare.gc.count()            -- return the current total number of managed objects
xshare.gc.step([new_step])   -- get/set the GC step multiplier (default 2.0)
xshare.gc.pause()            -- pause automatic GC
xshare.gc.resume()           -- resume automatic GC
xshare.gc.enabled()          -- return whether automatic GC is enabled
```

### Type Checking
```lua
xshare.type(obj)
```
Returns a string describing the type of `obj`. For shared tables it returns `"xshare.table"`; for ordinary Lua types it returns the standard type name (e.g., `"number"`, `"string"`).

## Examples

### Lua Example: Basic Usage
```lua
local xshare = require("xshare")

-- Create a shared table
local t = xshare.table({a = 1, b = 2})
t.c = 3
print(t.a, t.b, t.c)          -- 1   2   3
print(xshare.size(t))          -- 3

-- Use a metatable
local mt = xshare.table({__index = function(_, k) return "mt:" .. k end})
xshare.setmetatable(t, mt)
print(t.x)                     -- mt:x

-- Raw access
xshare.rawset(t, "a", 100)
print(xshare.rawget(t, "a"))   -- 100
print(t.a)                     -- 100 (still affected by the metatable)

-- GC control
print(xshare.gc.count())       -- prints the current object count
xshare.gc.collect()
```

### C Example: Passing a Function Across Threads
```c
#include "xshare.h"
#include <pthread.h>

void* thread_func(void* arg) {
    StoredObject* func_obj = (StoredObject*)arg;
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    luaopen_xshare(L);
    lua_pop(L, 1);

    stored_push(L, func_obj);
    lua_pushnumber(L, 5);
    if (lua_pcall(L, 1, 1, 0) == LUA_OK) {
        double result = lua_tonumber(L, -1);
        printf("Thread result: %f\n", result);
        lua_pop(L, 1);
    }
    lua_close(L);
    gc_release((GCObject*)func_obj);
    return NULL;
}

void start_thread(lua_State* L, int func_idx) {
    StoredObject* func_obj = stored_create(L, func_idx);
    gc_retain((GCObject*)func_obj);
    pthread_t thr;
    pthread_create(&thr, NULL, thread_func, func_obj);
    pthread_detach(thr);
}
```

## Important Notes

1. **Thread Safety:** All operations on `xshare.table` are thread‑safe, implemented with read‑write locks.
2. **Reference Counting and GC:** Both `StoredObject` and shared tables are managed by the GC. Manual calls to `gc_retain`/`gc_release` must be balanced to avoid leaks or premature collection.
3. **Function Passing:** Lua functions are serialised as bytecode and upvalues. The environment (`_ENV`) is treated specially: when the function is restored in a target thread, it will use that thread’s global environment.
4. **Unsupported Types:** The following cannot be passed: coroutines (`thread`), full userdata (other than shared tables), and tables with cycles that would cause infinite recursion during serialisation (the GC handles cycles, but serialisation uses a visited table to prevent recursion).
5. **Memory Limits:** When memory is exhausted, some operations may fail and return an error; the Lua bindings will raise an appropriate Lua error.
6. **C API Error Handling:** Most C functions return NULL or 0 to indicate failure; callers must check these return values and handle errors accordingly.

---

XShare aims to provide a simple yet efficient foundation for cross‑thread data sharing. Feel free to contribute or report issues.