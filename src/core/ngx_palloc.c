
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>


static ngx_inline void *ngx_palloc_small(ngx_pool_t *pool, size_t size,
    ngx_uint_t align);
static void *ngx_palloc_block(ngx_pool_t *pool, size_t size);
static void *ngx_palloc_large(ngx_pool_t *pool, size_t size);


ngx_pool_t *
ngx_create_pool(size_t size, ngx_log_t *log)
{
    ngx_pool_t  *p;
    
    //申请内存，ngxmealign调用posixmemalign函数，能够自定义对齐大小来申请内存

    p = ngx_memalign(NGX_POOL_ALIGNMENT, size, log);
    if (p == NULL) {
        return NULL;
    }

    p->d.last = (u_char *) p + sizeof(ngx_pool_t); //设置last指向data区起始地址
    p->d.end = (u_char *) p + size; //设置指向end结束地址
    p->d.next = NULL; //当前是最后一个pool，所以指向空
    p->d.failed = 0;

    size = size - sizeof(ngx_pool_t); //计算除去pool的头以外，data区的大小 
    //设置max，max<=size 且不能超过设定值。如果data区申请特别大，超过max限定值的部分也不能用
    p->max = (size < NGX_MAX_ALLOC_FROM_POOL) ? size : NGX_MAX_ALLOC_FROM_POOL; //data区大小如果小于从地址池申请的最大限制，max就设置为data区的尺寸。否则，设置为从地址池的最大申请尺寸

    p->current = p;
    p->chain = NULL;
    p->large = NULL;
    p->cleanup = NULL;
    p->log = log;

    return p;
}


void
ngx_destroy_pool(ngx_pool_t *pool)
{
    ngx_pool_t          *p, *n;
    ngx_pool_large_t    *l;
    ngx_pool_cleanup_t  *c;

    for (c = pool->cleanup; c; c = c->next) { //清除所有的cleanup节点
        if (c->handler) {  
            ngx_log_debug1(NGX_LOG_DEBUG_ALLOC, pool->log, 0,
                           "run cleanup: %p", c);
            c->handler(c->data);  //销毁数据
        }
    }

#if (NGX_DEBUG)

    /*
     * we could allocate the pool->log from this pool
     * so we cannot use this log while free()ing the pool
     */

    for (l = pool->large; l; l = l->next) {
        ngx_log_debug1(NGX_LOG_DEBUG_ALLOC, pool->log, 0, "free: %p", l->alloc);
    }

    for (p = pool, n = pool->d.next; /* void */; p = n, n = n->d.next) {
        ngx_log_debug2(NGX_LOG_DEBUG_ALLOC, pool->log, 0,
                       "free: %p, unused: %uz", p, p->d.end - p->d.last);

        if (n == NULL) {
            break;
        }
    }

#endif

    for (l = pool->large; l; l = l->next) { //清除挂在在该pool下的所有large pool
        if (l->alloc) {
            ngx_free(l->alloc); //这个ngx_free就是free的重命名
        }
    }

    for (p = pool, n = pool->d.next; /* void */; p = n, n = n->d.next) { //清除该pool后的所有pool
        ngx_free(p);

        if (n == NULL) {
            break;
        }
    }
}


//重置数据
void
ngx_reset_pool(ngx_pool_t *pool)
{
    ngx_pool_t        *p;
    ngx_pool_large_t  *l;

    // 释放所有large节点 
    for (l = pool->large; l; l = l->next) {
        if (l->alloc) {
            ngx_free(l->alloc);
        }
    }
    // 重定向数据区开始指针和失败次数。此时没有清零数据
    for (p = pool; p; p = p->d.next) {
        p->d.last = (u_char *) p + sizeof(ngx_pool_t);
        p->d.failed = 0;
    }

    pool->current = pool;
    pool->chain = NULL;
    pool->large = NULL;
}

/**
 * @brief  申请内存
 * @note   根据size确定是pool里还是挂载到large上
 * @param  *pool: pool头结点
 * @param  size: 
 * @retval None
 */
void *
ngx_palloc(ngx_pool_t *pool, size_t size)
{
#if !(NGX_DEBUG_PALLOC)
    if (size <= pool->max) { //申请地址在pool的最大尺寸内，申请small
        return ngx_palloc_small(pool, size, 1);
    }
#endif
    //申请地址在pool的最大尺寸外，申请large
    return ngx_palloc_large(pool, size);
}


void *
ngx_pnalloc(ngx_pool_t *pool, size_t size)
{
#if !(NGX_DEBUG_PALLOC)
    if (size <= pool->max) { //从pool里申请
        return ngx_palloc_small(pool, size, 0);
    }
#endif
//从large里申请
    return ngx_palloc_large(pool, size);
}


/**
 * @brief  从pool里申请内存
 * @note   
 * @param  *pool: pool头
 * @param  size: 大小
 * @param  align: 是否对齐
 * @retval None
 */
static ngx_inline void *
ngx_palloc_small(ngx_pool_t *pool, size_t size, ngx_uint_t align)
{
    u_char      *m;
    ngx_pool_t  *p;

    p = pool->current;

    do {
        m = p->d.last; //获取空闲地址开始位置

        if (align) {
            m = ngx_align_ptr(m, NGX_ALIGNMENT);
        }
        //当前池空闲空间够用，直接返回存储地址起始位置
        if ((size_t) (p->d.end - m) >= size) {
            p->d.last = m + size;

            return m;
        }
        //空闲空间不够，不用这个地址池，换下一个地址池
        p = p->d.next;

    } while (p);
    //如果地址池的data空间都小于申请空间，创建下一个地址池
    return ngx_palloc_block(pool, size);
}

/**
 * @brief  创建新的地址池，并且更新之前的地址池failed值和pool.current指向
 * @note   
 * @param  *pool:pool头 
 * @param  size: 新的大小
 * @retval None
 */
static void *
ngx_palloc_block(ngx_pool_t *pool, size_t size)
{   
    u_char      *m;
    size_t       psize;
    ngx_pool_t  *p, *new;
    //psize 是整个pool的大小，包含数据区
    psize = (size_t) (pool->d.end - (u_char *) pool);
    
    //创建新的结点时可以使用ngx_create_pool，然后再调整里面的参数。
    //申请一个pool包含数据区大小的动态内存
    m = ngx_memalign(NGX_POOL_ALIGNMENT, psize, pool->log);
    //申请失败
    if (m == NULL) {
        return NULL;
    }
    //新的pool的地址
    new = (ngx_pool_t *) m;
    //设置新的pool里面相关参数
    new->d.end = m + psize;
    new->d.next = NULL;
    new->d.failed = 0;

    //重定向m到数据去开始位置
    m += sizeof(ngx_pool_data_t);
    m = ngx_align_ptr(m, NGX_ALIGNMENT);
    new->d.last = m + size; //新的结束位置，分配完后的。

    //pool指的是第一个池。
    //处理failed的值，之前失败的都加1
    //如果某个已经失败超过4次了，就把pool头的current指针指向这个的下一个。
    //对于failed的值，后面结点的失败次数值一定小于前面结点的值。因为从头来尝试，能到达后边说明前边失败已经加1了。
    //current指针的作用是直接指向pool链表中失败次数5以下的pool。避免了地址申请时从头遍历效率降低
    for (p = pool->current; p->d.next; p = p->d.next) {
        if (p->d.failed++ > 4) {
            pool->current = p->d.next;
        }
    }
    //添加新结点
    p->d.next = new;

    return m;
}


static void *
ngx_palloc_large(ngx_pool_t *pool, size_t size)
{
    void              *p;
    ngx_uint_t         n;
    ngx_pool_large_t  *large;

    //申请内存
    p = ngx_alloc(size, pool->log);
    if (p == NULL) {
        return NULL;
    }

    n = 0;

    //从pool开始遍历large，large.alloc如果为空，就挂载刚刚申请的地址并退出程序。
    for (large = pool->large; large; large = large->next) {
        if (large->alloc == NULL) {
            large->alloc = p; //把申请来的地址挂在到large->alloc上
            return p;
        }
        //只遍历四个large。n=0/1/2/3时        
        //weakness: 4个以后的结点如果被释放，这个large结点就不能再挂载了。浪费两个指针空间    
        if (n++ > 3) {
            break;
        }
    }
    
    //连续四个large都没有空闲的，就新建一个large的struct，存储在pool的data区域中。
    large = ngx_palloc_small(pool, sizeof(ngx_pool_large_t), 1);
    if (large == NULL) {
        ngx_free(p);
        return NULL;
    }
    //处理large的参数
    large->alloc = p;
    //头插法，把large插入在了结点头。
    large->next = pool->large;
    pool->large = large;

    return p;
}

/**
 * @brief  对齐申请large结点挂在到pool上。
 * @note   
 * @param  *pool: 
 * @param  size: 
 * @param  alignment: 
 * @retval None
 */
void *
ngx_pmemalign(ngx_pool_t *pool, size_t size, size_t alignment)
{
    void              *p;
    ngx_pool_large_t  *large;

    p = ngx_memalign(alignment, size, pool->log);
    if (p == NULL) {
        return NULL;
    }

    large = ngx_palloc_small(pool, sizeof(ngx_pool_large_t), 1);
    if (large == NULL) {
        ngx_free(p);
        return NULL;
    }

    large->alloc = p;
    large->next = pool->large;
    pool->large = large;

    return p;
}

/**
 * @brief  从large中释放p
 * @note   
 * @param  *pool: pool头
 * @param  *p: 要释放的结点
 * @retval 
 */
ngx_int_t
ngx_pfree(ngx_pool_t *pool, void *p)
{
    ngx_pool_large_t  *l;
    //查找并释放该large结点指向的内存。
    //weakness： 没有清除这个结点
    for (l = pool->large; l; l = l->next) {
        if (p == l->alloc) {
            ngx_log_debug1(NGX_LOG_DEBUG_ALLOC, pool->log, 0,
                           "free: %p", l->alloc);
            ngx_free(l->alloc);
            l->alloc = NULL;

            return NGX_OK;
        }
    }

    return NGX_DECLINED;
}

/**
 * @brief  
 * @note   递归调用，
 * @param  *pool: 头结点
 * @param  size: 
 * @retval None
 */
void *
ngx_pcalloc(ngx_pool_t *pool, size_t size)
{
    void *p;

    p = ngx_palloc(pool, size);
    if (p) {
        ngx_memzero(p, size); //清0 调用memset()
    }

    return p;
}

/**
 * @brief  为某个pool结点条件cleanup结点
 * @note 创建cleanup结点，申请size大小内存， 并挂载到cleanup上。cleanup也在pool的data区。  
 * @param  *p: pool的某个结点
 * @param  size: 
 * @retval 返回新创建的clean_up结点指针。
 */
ngx_pool_cleanup_t *
ngx_pool_cleanup_add(ngx_pool_t *p, size_t size)
{
    ngx_pool_cleanup_t  *c;

    c = ngx_palloc(p, sizeof(ngx_pool_cleanup_t)); //这个尺寸就进了pool里了。
    if (c == NULL) {
        return NULL;
    }

    if (size) {
        c->data = ngx_palloc(p, size); //申请大小为size的内存，挂载到c->data上
        if (c->data == NULL) {
            return NULL;
        }

    } else {
        c->data = NULL;
    }
    //处理函数为空
    c->handler = NULL;
    //头插法。
    c->next = p->cleanup;
    p->cleanup = c;

    ngx_log_debug1(NGX_LOG_DEBUG_ALLOC, p->log, 0, "add cleanup: %p", c);

    return c;
}

/**
 * @brief 清理文件  
 * @note   
 * @param  *p: pool结点
 * @param  fd: 
 * @retval 
 */
void
ngx_pool_run_cleanup_file(ngx_pool_t *p, ngx_fd_t fd)
{
    ngx_pool_cleanup_t       *c;
    ngx_pool_cleanup_file_t  *cf;
    //遍历pool的所有cleanup结点
    for (c = p->cleanup; c; c = c->next) {
        //如果handler(处理函数)是ngx_pool_cleanup_file
        if (c->handler == ngx_pool_cleanup_file) {
            //设置cf指向cleanup的data所指内容
            //cf只是指针，还没有给struct分配空间。
            //c->data本来指向的也应该是cleanup_file_t的数据类型。fd存着文件句柄
            cf = c->data; //cf->fd 如何获取的？
            //cf->fd和所要关闭的文件fd相等，则运行处理函数，关闭文件
            if (cf->fd == fd) {
                c->handler(cf);
                c->handler = NULL;
                return;
            }
        }
    }
}

/**
 * @brief  关闭文件回调函
 * @note   
 * @param  *data: 
 * @retval 
 */
void
ngx_pool_cleanup_file(void *data)
{
    ngx_pool_cleanup_file_t  *c = data;

    ngx_log_debug1(NGX_LOG_DEBUG_ALLOC, c->log, 0, "file cleanup: fd:%d",
                   c->fd);

    if (ngx_close_file(c->fd) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_ALERT, c->log, ngx_errno,
                      ngx_close_file_n " \"%s\" failed", c->name);
    }
}

/**
 * @brief  删除文件回调函数
 * @note   
 * @param  *data: 
 * @retval 
 */
void
ngx_pool_delete_file(void *data)
{
    ngx_pool_cleanup_file_t  *c = data;

    ngx_err_t  err;

    ngx_log_debug2(NGX_LOG_DEBUG_ALLOC, c->log, 0, "file cleanup: fd:%d %s",
                   c->fd, c->name);

    if (ngx_delete_file(c->name) == NGX_FILE_ERROR) {
        err = ngx_errno;

        if (err != NGX_ENOENT) {
            ngx_log_error(NGX_LOG_CRIT, c->log, err,
                          ngx_delete_file_n " \"%s\" failed", c->name);
        }
    }

    if (ngx_close_file(c->fd) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_ALERT, c->log, ngx_errno,
                      ngx_close_file_n " \"%s\" failed", c->name);
    }
}


#if 0

static void *
ngx_get_cached_block(size_t size)
{
    void                     *p;
    ngx_cached_block_slot_t  *slot;

    if (ngx_cycle->cache == NULL) {
        return NULL;
    }

    slot = &ngx_cycle->cache[(size + ngx_pagesize - 1) / ngx_pagesize];

    slot->tries++;

    if (slot->number) {
        p = slot->block;
        slot->block = slot->block->next;
        slot->number--;
        return p;
    }

    return NULL;
}

#endif
