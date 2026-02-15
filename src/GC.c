#include "GC.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

GC* gc_instance(void) {
    static GC global_gc = {
        .head = NULL,
        .tail = NULL,
        .count = 0,
        .enabled = 1,
        .step = 2.0,
        .lastCleanup = 100,
        .rwlock = PTHREAD_RWLOCK_INITIALIZER
    };
    return &global_gc;
}

/* 内部：确保强引用数组有足够容量（必须持有写锁） */
static int ensure_strong_capacity(GCObject* obj, int needed) {
    if (needed <= obj->strongCapacity) return 1;
    
    int newCap = obj->strongCapacity * 2;
    if (newCap < needed) newCap = needed;
    
    GCObject** newRefs = (GCObject**)realloc(obj->strongRefs, newCap * sizeof(GCObject*));
    if (!newRefs) return 0;   // 内存不足
    
    obj->strongRefs = newRefs;
    obj->strongCapacity = newCap;
    return 1;
}

/* 内部：分配新对象，返回已加入链表的对象（必须持有写锁） */
static GCObject* alloc_object(GC* gc, size_t data_size) {
    GCObject* obj = (GCObject*)malloc(sizeof(GCObject) + data_size);
    if (!obj) return NULL;
    
    atomic_init(&obj->refCount, 1);   // GC自身持有1个引用
    obj->mark = 0;
    obj->strongCapacity = 4;
    obj->strongSize = 0;
    
    obj->strongRefs = (GCObject**)malloc(obj->strongCapacity * sizeof(GCObject*));
    if (!obj->strongRefs) {
        free(obj);
        return NULL;
    }
    
    /* 插入链表尾部 */
    obj->prev = gc->tail;
    obj->next = NULL;
    if (gc->tail)
        gc->tail->next = obj;
    else
        gc->head = obj;
    gc->tail = obj;
    gc->count++;
    
    return obj;
}

GCObject* gc_create(GC* gc, size_t data_size) {
    pthread_rwlock_wrlock(&gc->rwlock);   // 写锁，因为可能触发GC且需修改链表
    
    /* 根据阈值决定是否自动收集 */
    if (gc->enabled && gc->count >= gc->step * gc->lastCleanup) {
        gc_collect(gc);   // 已持有写锁，直接收集
    }
    
    GCObject* obj = alloc_object(gc, data_size);
    pthread_rwlock_unlock(&gc->rwlock);
    return obj;
}

void gc_retain(GCObject* obj) {
    GC* gc = gc_instance();
    atomic_fetch_add(&obj->refCount, 1);
}

void gc_release(GCObject* obj) {
    GC* gc = gc_instance();
    int old = atomic_fetch_sub(&obj->refCount, 1);
    assert(old > 0);
}

void gc_add_reference(GCObject* from, GCObject* to) {
    if (!from || !to) return;
    GC* gc = gc_instance();
    pthread_rwlock_wrlock(&gc->rwlock);
    
    if (!ensure_strong_capacity(from, from->strongSize + 1)) {
        // 内存不足，放弃添加引用
        pthread_rwlock_unlock(&gc->rwlock);
        return;
    }
    
    from->strongRefs[from->strongSize++] = to;
    pthread_rwlock_unlock(&gc->rwlock);
}

void gc_remove_reference(GCObject* from, GCObject* to) {
    if (!from || !to) return;
    GC* gc = gc_instance();
    pthread_rwlock_wrlock(&gc->rwlock);
    
    for (int i = 0; i < from->strongSize; i++) {
        if (from->strongRefs[i] == to) {
            // 用最后一个元素覆盖要删除的元素
            from->strongRefs[i] = from->strongRefs[--from->strongSize];
            break;
        }
    }
    
    /* 缩容优化：当实际使用量小于容量的1/3且容量大于4时，尝试缩小一半 */
    if (from->strongSize * 3 < from->strongCapacity && from->strongCapacity > 4) {
        int newCap = from->strongCapacity / 2;
        if (newCap < 4) newCap = 4;
        GCObject** newRefs = (GCObject**)realloc(from->strongRefs, newCap * sizeof(GCObject*));
        if (newRefs) {
            from->strongRefs = newRefs;
            from->strongCapacity = newCap;
        }
    }
    
    pthread_rwlock_unlock(&gc->rwlock);
}

void gc_collect(GC* gc) {
    // 调用时已经持有写锁（由上层保证）
    size_t objCount = gc->count;
    if (objCount == 0) return;
    
    /* 预分配灰色队列数组，大小为当前对象总数 */
    GCObject** grey = (GCObject**)malloc(objCount * sizeof(GCObject*));
    if (!grey) {
        // 内存不足，跳过本次收集（比部分标记更安全）
        return;
    }
    int greySize = 0;

    /* 第一步：重置所有对象为白色，找出根（refCount > 1 的对象） */
    for (GCObject* obj = gc->head; obj; obj = obj->next) {
        obj->mark = 0;
        int rc = atomic_load(&obj->refCount);
        if (rc > 1) {   // 有外部引用（排除GC自身持有的1）
            grey[greySize++] = obj;
            obj->mark = 1;   // 灰色
        }
    }

    /* 第二步：标记传播 */
    for (int i = 0; i < greySize; i++) {
        GCObject* cur = grey[i];
        for (int j = 0; j < cur->strongSize; j++) {
            GCObject* ref = cur->strongRefs[j];
            if (ref && ref->mark == 0) {
                ref->mark = 1;
                grey[greySize++] = ref;
            }
        }
        cur->mark = 2;   // 黑色
    }

    /* 第三步：清除白色对象 */
    GCObject* obj = gc->head;
    while (obj) {
        GCObject* next = obj->next;
        if (obj->mark == 0) {
            /* 从链表中移除 */
            if (obj->prev) obj->prev->next = obj->next;
            else gc->head = obj->next;
            if (obj->next) obj->next->prev = obj->prev;
            else gc->tail = obj->prev;

            /* 调用析构函数（如果存在） */
            if (obj->dtor) {
                obj->dtor(obj);
            }
            
            /* 释放强引用数组和对象本身 */
            free(obj->strongRefs);
            free(obj);
            gc->count--;
        }
        obj = next;
    }

    free(grey);
    gc->lastCleanup = gc->count;
}

void gc_pause(GC* gc) {
    pthread_rwlock_wrlock(&gc->rwlock);
    gc->enabled = 0;
    pthread_rwlock_unlock(&gc->rwlock);
}

void gc_resume(GC* gc) {
    pthread_rwlock_wrlock(&gc->rwlock);
    gc->enabled = 1;
    pthread_rwlock_unlock(&gc->rwlock);
}

int gc_enabled(GC* gc) {
    pthread_rwlock_rdlock(&gc->rwlock);
    int ret = gc->enabled;
    pthread_rwlock_unlock(&gc->rwlock);
    return ret;
}

void gc_set_step(GC* gc, double step) {
    pthread_rwlock_wrlock(&gc->rwlock);
    if (step > 1.0) {
        gc->step = step;
    } else {
        gc->step = 1.01;   // 最小阈值，避免无限触发
    }
    pthread_rwlock_unlock(&gc->rwlock);
}

double gc_get_step(GC* gc) {
    pthread_rwlock_rdlock(&gc->rwlock);
    double ret = gc->step;
    pthread_rwlock_unlock(&gc->rwlock);
    return ret;
}

int gc_count(GC* gc) {
    pthread_rwlock_rdlock(&gc->rwlock);
    int ret = gc->count;
    pthread_rwlock_unlock(&gc->rwlock);
    return ret;
}