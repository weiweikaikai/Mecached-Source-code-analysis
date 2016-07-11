

#include "memcached.h"
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/signal.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>

static pthread_cond_t maintenance_cond = PTHREAD_COND_INITIALIZER;


typedef  unsigned long  int  ub4;   /* unsigned 4-byte quantities */
typedef  unsigned       char ub1;   /* unsigned 1-byte quantities */

/* how many powers of 2's worth of buckets we use */
unsigned int hashpower = HASHPOWER_DEFAULT;

#define hashsize(n) ((ub4)1<<(n))
#define hashmask(n) (hashsize(n)-1)

/* Main hash table. This is where we look except during expansion. */
//主hash表结构定义，在hash表扩容时，会有次hash表，所以有主次hash表区分，
//该结构是指针的指针，也即相当于数组指针  
static item** primary_hashtable = 0;

/*
 * Previous hash table. During expansion, we look here for keys that haven't
 * been moved over to the primary yet.
 */
static item** old_hashtable = 0;

/* Number of items in the hash table. */
static unsigned int hash_items = 0;

/* Flag: Are we in the middle of expanding now? */
static bool expanding = false;
static bool started_expanding = false;

/*
 * During expansion we migrate values with bucket granularity; this is how
 * far we've gotten so far. Ranges from 0 .. hashsize(hashpower - 1) - 1.
 */
static unsigned int expand_bucket = 0;

//hash表的初始化，传入的参数是启动时候传入的
void assoc_init(const int hashtable_init) {
    if (hashtable_init) { //如果设置了初始化参数，则按设置的参数进行初始化
        hashpower = hashtable_init;
    }
	 //hashpower的默认值为16,如果未设置新值，则按默认值进行初始化
    primary_hashtable = calloc(hashsize(hashpower), sizeof(void *));
    if (! primary_hashtable) {
        fprintf(stderr, "Failed to init hashtable.\n");
        exit(EXIT_FAILURE);
    }
	//全局统计信息加锁，保证数据同步
    STATS_LOCK();
    stats.hash_power_level = hashpower;
    stats.hash_bytes = hashsize(hashpower) * sizeof(void *);
    STATS_UNLOCK();
}

item *assoc_find(const char *key, const size_t nkey, const uint32_t hv) {
    item *it;
    unsigned int oldbucket;

    /* 如果hashtable正在扩容阶段，所找item在老hashtable中，则在老hashtable中查询，否则从新表中查询
     * 判断在哪个表中的思路：
     * 1. 定位key所在hashtable中桶的位置
     * 2. 如果此位置大于等于从旧hashtable中移到新hashtable的数量，则所查找元素在旧hashtable中，否则在新hash表中
     *
     * eg.
     * primary hashtable
     * [0]
     * [1] -> a -> b -> null
     * [2]
     * [3] -> x 
     *
     * old hashtable
     * [0]
     * [1]
     * [2]
     * [3]
     * [4] -> y ->null
     * [5] -> p -> null          <--- hash(key)
     * ...
     */
    if (expanding &&
        (oldbucket = (hv & hashmask(hashpower - 1))) >= expand_bucket)
    {
        it = old_hashtable[oldbucket];
    } else {
        it = primary_hashtable[hv & hashmask(hashpower)];
    }

    item *ret = NULL;
    int depth = 0;
    while (it) {
        if ((nkey == it->nkey) && (memcmp(key, ITEM_key(it), nkey) == 0)) {
            ret = it;
            break;
        }
        it = it->h_next;
        ++depth;
    }
    MEMCACHED_ASSOC_FIND(key, nkey, depth);
    return ret;
}

/* returns the address of the item pointer before the key.  if *item == 0,
   the item wasn't found */

static item** _hashitem_before (const char *key, const size_t nkey, const uint32_t hv) {
    item **pos;
    unsigned int oldbucket;

    if (expanding &&
        (oldbucket = (hv & hashmask(hashpower - 1))) >= expand_bucket)
    {
        pos = &old_hashtable[oldbucket];
    } else {
        pos = &primary_hashtable[hv & hashmask(hashpower)];
    }

    while (*pos && ((nkey != (*pos)->nkey) || memcmp(key, ITEM_key(*pos), nkey))) {
        pos = &(*pos)->h_next;
    }
    return pos;
}

/* grows the hashtable to the next power of 2.//按2倍容量扩容Hash表   */
static void assoc_expand(void) {
    old_hashtable = primary_hashtable;//old_hashtable指向主Hash表  

    primary_hashtable = calloc(hashsize(hashpower + 1), sizeof(void *));//申请新的空间  
    if (primary_hashtable) {//空间申请成功  
        if (settings.verbose > 1)
            fprintf(stderr, "Hash table expansion starting\n");
        hashpower++;//hash等级+1  
        expanding = true;//扩容标识打开
        expand_bucket = 0;
        STATS_LOCK();//更新全局统计信息
        stats.hash_power_level = hashpower;
        stats.hash_bytes += hashsize(hashpower) * sizeof(void *);
        stats.hash_is_expanding = 1;
        STATS_UNLOCK();
    } else {//空间申请失败
        primary_hashtable = old_hashtable;
        /* Bad news, but we can keep running. */
    }
}

//唤醒扩容线程
static void assoc_start_expand(void) {
    if (started_expanding)
        return;
    started_expanding = true;
    pthread_cond_signal(&maintenance_cond);//唤醒信号量
}

/* Note: this isn't an assoc_update.  The key must not already exist to call this
//hash表中增加元素*/
int assoc_insert(item *it, const uint32_t hv) {
    unsigned int oldbucket;

//    assert(assoc_find(ITEM_key(it), it->nkey) == 0);  /* shouldn't have duplicately named things defined */
     //如果已经进行扩容且目前进行扩容还没到需要插入元素的桶，则将元素添加到旧桶中  
    if (expanding &&
        (oldbucket = (hv & hashmask(hashpower - 1))) >= expand_bucket)
    {
        it->h_next = old_hashtable[oldbucket];
        old_hashtable[oldbucket] = it;
    } else {//如果没扩容，或者扩容已经到了新的桶中，则添加元素到新表中 
        it->h_next = primary_hashtable[hv & hashmask(hashpower)];//添加元素
        primary_hashtable[hv & hashmask(hashpower)] = it;
    }

    hash_items++;//元素数目+1  
	    //还没开始扩容，且表中元素个数已经超过Hash表容量的1.5倍  
    if (! expanding && hash_items > (hashsize(hashpower) * 3) / 2) {
        assoc_start_expand();//唤醒扩容线程  
    }

    MEMCACHED_ASSOC_INSERT(ITEM_key(it), it->nkey, hash_items);
    return 1;
}

void assoc_delete(const char *key, const size_t nkey, const uint32_t hv) {
    item **before = _hashitem_before(key, nkey, hv);

    if (*before) {
        item *nxt;
        hash_items--;
        /* The DTrace probe cannot be triggered as the last instruction
         * due to possible tail-optimization by the compiler
         */
        MEMCACHED_ASSOC_DELETE(key, nkey, hash_items);
        nxt = (*before)->h_next;
        (*before)->h_next = 0;   /* probably pointless, but whatever. */
        *before = nxt;
        return;
    }
    /* Note:  we never actually get here.  the callers don't delete things
       they can't find. */
    assert(*before != 0);
}


static volatile int do_run_maintenance_thread = 1;

#define DEFAULT_HASH_BULK_MOVE 1
int hash_bulk_move = DEFAULT_HASH_BULK_MOVE;

//启动扩容线程，扩容线程在main函数中会启动，启动运行一遍之后会阻塞在条件变量maintenance_cond上面，
//插入元素超过规定，唤醒条件变量  
static void *assoc_maintenance_thread(void *arg) {
    //do_run_maintenance_thread的值为1，即该线程持续运行
    while (do_run_maintenance_thread) {
        int ii = 0;

        /* Lock the cache, and bulk move multiple buckets to the new
         * hash table. */
        item_lock_global();//加Hash表的全局锁  
        mutex_lock(&cache_lock);//加cache_lock锁  
           //执行扩容时，每次按hash_bulk_move个桶来扩容  
        for (ii = 0; ii < hash_bulk_move && expanding; ++ii) {
            item *it, *next;
            int bucket;
              //老表每次移动一个桶中的一个元素  
            for (it = old_hashtable[expand_bucket]; NULL != it; it = next) {
                next = it->h_next;//要移动的下一个元素 

                bucket = hash(ITEM_key(it), it->nkey, 0) & hashmask(hashpower);//按新的Hash规则进行定位 
                it->h_next = primary_hashtable[bucket];//挂载到新的Hash表中  
                primary_hashtable[bucket] = it;
            }

            old_hashtable[expand_bucket] = NULL;//旧表中的这个Hash桶已经按新规则完成了扩容  

            expand_bucket++;//老表中的桶计数+1
            if (expand_bucket == hashsize(hashpower - 1)) {//hash表扩容结束,expand_bucket从0开始,一直递增 
                expanding = false;//修改扩容标志  
                free(old_hashtable);//释放老的表结构  
                STATS_LOCK();//更新一些统计信息  
                stats.hash_bytes -= hashsize(hashpower - 1) * sizeof(void *);
                stats.hash_is_expanding = 0;
                STATS_UNLOCK();
                if (settings.verbose > 1)
                    fprintf(stderr, "Hash table expansion done\n");
            }
        }

        mutex_unlock(&cache_lock);//释放cache_lock锁  
        item_unlock_global();//释放Hash表的全局锁  

        if (!expanding) {//完成扩容
            /* finished expanding. tell all threads to use fine-grained locks 
			 //修改Hash表的锁类型，此时锁类型更新为分段锁，默认是分段锁，在进行扩容时，改为全局锁  */
            switch_item_lock_type(ITEM_LOCK_GRANULAR);
            slabs_rebalancer_resume();//释放用于扩容的锁  
            /* We are done expanding.. just wait for next invocation */
            mutex_lock(&cache_lock);//加cache_lock锁，保护条件变量 
            started_expanding = false;//修改扩容标识  
            pthread_cond_wait(&maintenance_cond, &cache_lock); //阻塞扩容线程  
            /* Before doing anything, tell threads to use a global lock */
            mutex_unlock(&cache_lock);
            slabs_rebalancer_pause();//加用于扩容的锁
            switch_item_lock_type(ITEM_LOCK_GLOBAL);//修改锁类型为全局锁 
            mutex_lock(&cache_lock);//临时用来实现临界区  
            assoc_expand();//执行扩容
            mutex_unlock(&cache_lock);
        }
    }
    return NULL;
}

static pthread_t maintenance_tid;

/* 创建hashtable维护线程 */
int start_assoc_maintenance_thread() {
    int ret;
    char *env = getenv("MEMCACHED_HASH_BULK_MOVE");

    // 如果环境变量MEMCACHED_HASH_BULK_MOVE设置，则使用此设置值  
    // 维护线程中，每次扩展的粒度(每次hash_bulk_move个buckets)
    if (env != NULL) {
        hash_bulk_move = atoi(env);
        if (hash_bulk_move == 0) {
            hash_bulk_move = DEFAULT_HASH_BULK_MOVE;
        }
    }
    if ((ret = pthread_create(&maintenance_tid, NULL,
                              assoc_maintenance_thread, NULL)) != 0) {
        fprintf(stderr, "Can't create thread: %s\n", strerror(ret));
        return -1;
    }
    return 0;
}

void stop_assoc_maintenance_thread() {
    mutex_lock(&cache_lock);
    do_run_maintenance_thread = 0;
    pthread_cond_signal(&maintenance_cond);
    mutex_unlock(&cache_lock);

    /* Wait for the maintenance thread to stop */
    pthread_join(maintenance_tid, NULL);
}


