[**中文**](README.md) | [**English**](README_en.md)

# XShare - 跨线程数据共享库

XShare 是一个为 Lua 提供的轻量级跨线程数据共享库。它允许在多个独立的 Lua 状态之间安全地传递和共享数据，包括基本类型、表、函数等，并集成了三色标记垃圾回收器，自动管理内存。XShare 适用于多线程 Lua 应用，例如并发任务处理、数据交换等场景。

## 特性

- 线程安全的共享表（`xshare.table`）
- 支持基本类型、函数、表的跨线程传递（深拷贝或共享）
- 自定义三色标记 GC，自动回收循环引用
- 提供 C API 和 Lua API，易于集成
- 支持 Lua 5.1、5.2、5.3 及 LuaJIT

## 编译与安装

### 依赖
- Lua 开发库（5.1+）
- CMake 3.10+
- pthread（多线程支持）

### 编译
```bash
git clone <repository>
cd XShare
mkdir build && cd build
cmake ..
make
```

### 安装
```bash
make install
```

安装后，您可以在 Lua 中通过 `require("xshare")` 加载模块，并在 C 代码中链接 `libxshare.a` 并包含头文件。

## C API 参考

C API 定义在以下头文件中：
- `GC.h` - 垃圾回收器核心
- `stored_object.h` - 可存储对象的序列化
- `shared_table.h` - 共享表操作

### GC 管理

```c
GC* gc_instance(void);
```
返回全局 GC 实例指针。

```c
GCObject* gc_create(GC* gc, size_t data_size);
```
创建一个新的 GC 对象，返回指针。`data_size` 为对象实际数据大小（不包含 `GCObject` 头）。新对象的引用计数初始为 1。

```c
void gc_retain(GCObject* obj);
void gc_release(GCObject* obj);
```
增加/减少对象的引用计数。计数为 0 的对象会在下次 GC 时被回收。

```c
void gc_add_reference(GCObject* from, GCObject* to);
void gc_remove_reference(GCObject* from, GCObject* to);
```
在 `from` 和 `to` 之间建立/移除强引用关系，用于 GC 标记传播。

```c
void gc_collect(GC* gc);
```
执行一次完整的标记-清除回收。

```c
void gc_pause(GC* gc);
void gc_resume(GC* gc);
int gc_enabled(GC* gc);
```
暂停/恢复自动 GC，查询当前状态。

```c
void gc_set_step(GC* gc, double step);
double gc_get_step(GC* gc);
int gc_count(GC* gc);
```
设置/获取 GC 触发阈值系数（`count >= lastCleanup * step`），返回当前对象总数。

### StoredObject 序列化

`StoredObject` 是将 Lua 值转换为可跨线程存储的表示，支持的类型：nil、boolean、number、integer、string、lightuserdata、C 函数、Lua 函数、普通表深拷贝、共享表。

```c
StoredObject* stored_create(lua_State* L, int index);
```
从 Lua 栈上指定索引处创建 `StoredObject`。返回值需要调用 `gc_release` 释放。

```c
void stored_push(lua_State* L, StoredObject* obj);
```
将 `StoredObject` 推回 Lua 栈，还原为原始值。

```c
int stored_compare(const StoredObject* a, const StoredObject* b);
```
比较两个 `StoredObject`，用于有序容器（如 `SharedTable` 的键查找）。返回值：<0 表示 a < b，0 相等，>0 表示 a > b。

```c
StoredObject* stored_create_from_sharedtable(SharedTable* st);
```
将共享表包装为 `StoredObject`，用于在共享表中存储其他共享表。

### SharedTable 操作

`SharedTable` 是线程安全的共享表，可被多个 Lua 状态同时访问。

```c
SharedTable* shared_table_create(GC* gc);
```
创建一个新的空共享表。

```c
int shared_table_set(SharedTable* tbl, StoredObject* key, StoredObject* val);
```
设置键值对。成功返回 1，失败（内存不足）返回 0。该函数会自动增加对 `key` 和 `val` 的引用。

```c
StoredObject* shared_table_get(SharedTable* tbl, StoredObject* key);
```
获取键对应的值。返回的 `StoredObject*` 属于表内部，调用者不应释放。如果键不存在，返回 NULL。

```c
void shared_table_delete(SharedTable* tbl, StoredObject* key);
```
删除指定键及其值，并释放相关引用。

```c
size_t shared_table_size(SharedTable* tbl);
size_t shared_table_length(SharedTable* tbl);
```
返回表中元素个数，以及数组部分的长度（连续整数键从 1 开始的最大长度）。

```c
SharedTablePair shared_table_next(SharedTable* tbl, StoredObject* key);
```
迭代器。若 `key` 为 NULL，返回第一个键值对；否则返回 `key` 之后的下一个键值对。`SharedTablePair` 包含 `key` 和 `val` 指针，均属于表内部，调用者不应释放。

```c
void shared_table_set_metatable(SharedTable* tbl, StoredObject* mt);
StoredObject* shared_table_get_metatable(SharedTable* tbl);
```
设置/获取元表（`mt` 必须是包装了另一个共享表的 `StoredObject`）。

## Lua API 参考

Lua 模块名为 `xshare`，通过 `require("xshare")` 加载。返回一个表，包含以下函数：

### `xshare.table([tbl])`
创建一个新的共享表。如果提供了 Lua 表 `tbl`，会将其所有字段深拷贝到新共享表中（普通表转为 `STORED_TABLE_COPY`，共享表保留引用）。

**参数**：`tbl` (可选) - 普通 Lua 表  
**返回**：共享表 userdata

### 共享表元方法
共享表支持以下 Lua 元方法，行为与普通表一致：
- `__index`、`__newindex`、`__len`、`__pairs`、`__ipairs`、`__tostring`

因此可以直接使用标准的表语法：
```lua
local t = xshare.table()
t.key = "value"
print(t.key)      -- value
print(#t)         -- 0
```

### 元表操作
```lua
xshare.setmetatable(tbl, mt)
xshare.getmetatable(tbl)
```
设置/获取共享表的元表。`mt` 可以是普通 Lua 表（会被自动转换为共享表），也可以是另一个共享表。

### 原始访问
```lua
xshare.rawset(tbl, key, value)
xshare.rawget(tbl, key)
```
绕过元表直接设置/获取值。等价于 Lua 的 `rawset`/`rawget`。

### `xshare.size(tbl)`
返回共享表中的元素个数（等价于 `pairs` 遍历计数，但更高效）。

### GC 控制
```lua
xshare.gc.collect()          -- 手动触发一次 GC
xshare.gc.count()            -- 返回当前管理的对象总数
xshare.gc.step([new_step])   -- 获取/设置 GC 步长系数（默认 2.0）
xshare.gc.pause()            -- 暂停自动 GC
xshare.gc.resume()           -- 恢复自动 GC
xshare.gc.enabled()          -- 返回自动 GC 是否启用
```

### 类型检查
```lua
xshare.type(obj)
```
返回对象的类型字符串。对于共享表返回 `"xshare.table"`，对于普通 Lua 类型返回标准类型名（如 `"number"`、`"string"`）。

## 示例

### Lua 示例：基本用法
```lua
local xshare = require("xshare")

-- 创建共享表
local t = xshare.table({a = 1, b = 2})
t.c = 3
print(t.a, t.b, t.c)          -- 1   2   3
print(xshare.size(t))          -- 3

-- 使用元表
local mt = xshare.table({__index = function(_, k) return "mt:" .. k end})
xshare.setmetatable(t, mt)
print(t.x)                     -- mt:x

-- 原始访问
xshare.rawset(t, "a", 100)
print(xshare.rawget(t, "a"))   -- 100
print(t.a)                     -- 100 (仍受元表影响)

-- GC 控制
print(xshare.gc.count())       -- 输出当前对象数
xshare.gc.collect()
```

### C 示例：跨线程传递函数
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

## 注意事项

1. **线程安全**：所有 `xshare.table` 操作都是线程安全的，内部使用读写锁。
2. **引用计数与 GC**：`StoredObject` 和共享表均由 GC 管理，手动调用 `gc_retain`/`gc_release` 需谨慎，确保引用平衡。
3. **函数传递**：Lua 函数被序列化为字节码和 upvalues。环境（`_ENV`）会被特殊处理：在目标线程中，函数将使用该线程的全局环境。
4. **不支持的类型**：无法传递 `thread`（协程）、完整 userdata（除共享表外）、带有循环引用的表（但 GC 可处理循环，序列化时使用 visited 表防止无限递归）。
5. **内存限制**：当内存不足时，部分操作可能失败并返回错误，Lua 层会抛出错误。
6. **C API 错误处理**：大多数 C 函数返回 NULL 或 0 表示失败，调用者需检查并适当处理。

---

XShare 旨在提供简洁高效的跨线程数据共享能力，欢迎使用并提出改进建议。