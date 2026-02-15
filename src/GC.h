#ifndef GC_H
#define GC_H

#include <stddef.h>
#include <pthread.h>
#include <stdatomic.h>

/* 辅助宏：获取用户数据起始地址 */
#define gc_userdata(obj) ((void*)((char*)(obj) + sizeof(GCObject)))

/* 对象头结构 */
typedef struct GCObject {
    atomic_int refCount;            /* 外部引用计数（原子类型） */
    int mark;                       /* 标记颜色：0白色，1灰色，2黑色 */
    int strongCapacity;             /* 强引用数组容量 */
    int strongSize;                 /* 当前强引用数量 */
    struct GCObject** strongRefs;   /* 强引用动态数组 */
    struct GCObject *prev, *next;   /* 双向链表节点 */
    void (*dtor)(struct GCObject*);   // 析构函数，在对象被回收前调用
} GCObject;

/* GC全局结构 */
typedef struct GC {
    struct GCObject *head, *tail;   /* 对象链表 */
    int count;                      /* 对象总数 */
    int enabled;                    /* 是否允许自动收集 */
    double step;                    /* 触发阈值系数 */
    size_t lastCleanup;             /* 上次清理后的对象数 */
    pthread_rwlock_t rwlock;        /* 读写锁：读锁用于引用计数，写锁用于修改结构 */
} GC;

/* 全局单例访问 */
GC* gc_instance(void);

/* 创建新对象，返回句柄。data_size 为用户数据大小，将附加在对象后 */
GCObject* gc_create(GC* gc, size_t data_size);

/* 增加外部引用计数（例如Lua持有） */
void gc_retain(GCObject* obj);

/* 减少外部引用计数 */
void gc_release(GCObject* obj);

/* 添加从 from 到 to 的强引用 */
void gc_add_reference(GCObject* from, GCObject* to);

/* 移除从 from 到 to 的强引用 */
void gc_remove_reference(GCObject* from, GCObject* to);

/* 执行一次垃圾收集（三色标记清除） */
void gc_collect(GC* gc);

/* 暂停自动收集（create时不再触发collect） */
void gc_pause(GC* gc);

/* 恢复自动收集 */
void gc_resume(GC* gc);

/* 获取当前是否允许自动收集 */
int gc_enabled(GC* gc);

/* 设置触发收集的阈值系数，例如 2.0 表示对象数达到上次清理后对象数的2倍时触发 */
void gc_set_step(GC* gc, double step);

/* 获取当前阈值系数 */
double gc_get_step(GC* gc);

/* 返回当前管理的对象总数 */
int gc_count(GC* gc);

#endif // GC_H