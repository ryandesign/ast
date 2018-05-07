/***********************************************************************
 *                                                                      *
 *               This software is part of the ast package               *
 *          Copyright (c) 1985-2013 AT&T Intellectual Property          *
 *                      and is licensed under the                       *
 *                 Eclipse Public License, Version 1.0                  *
 *                    by AT&T Intellectual Property                     *
 *                                                                      *
 *                A copy of the License is available at                 *
 *          http://www.eclipse.org/org/documents/epl-v10.html           *
 *         (with md5 checksum b35adb5213ca9657e911e9befb180842)         *
 *                                                                      *
 *              Information and Software Systems Research               *
 *                            AT&T Research                             *
 *                           Florham Park NJ                            *
 *                                                                      *
 *               Glenn Fowler <glenn.s.fowler@gmail.com>                *
 *                    David Korn <dgkorn@gmail.com>                     *
 *                     Phong Vo <phongvo@gmail.com>                     *
 *                                                                      *
 ***********************************************************************/
#include "config_ast.h"  // IWYU pragma: keep

#include "vmhdr.h"

/* The below implements the discipline Vmdcsystem and the heap region Vmheap.
** There are 6 alternative ways to get raw memory:
**	win32, sbrk, safe_sbrk, mmap_anon, mmap_zero and reusing the native malloc
**
** Written by Kiem-Phong Vo, phongvo@gmail.com, 03/31/2012
*/

#define FD_INIT (-1) /* uninitialized file desc	*/

typedef struct Memdisc_s {
    Vmdisc_t disc;
    int fd;
    off_t offset;
} Memdisc_t;

#define STR(x) #x
#define XTR(x) STR(x)

#define RESTARTMEM(a, f)                   \
    do {                                   \
        a = (void *)f;                   \
        if (a == (void *)(-1)) a = NULL; \
    } while (a == NULL && errno == EINTR)

#define RESTARTSYS(r, f) \
    do {                 \
        r = (int)f;      \
    } while (r == -1 && errno == EINTR)

#define GETMEMCHK(vm, caddr, csize, nsize, disc) \
    /**/ DEBUG_ASSERT(csize > 0 || nsize > 0);     \
    if ((csize > 0 && !caddr) || (csize == 0 && nsize == 0)) return NULL

#define GETMEMUSE(getmem, disc)                                                          \
    if (_Vmassert & VM_verbose) debug_printf(2, "vmalloc: getmemory=%s\n", XTR(getmem)); \
    (disc)->memoryf = _Vmemoryf = (getmem)

static Vmemory_f _Vmemoryf = 0;

#if _std_malloc
#undef _mem_mmap_anon
#undef _mem_sbrk
#undef _mem_win32
#endif

#if _mem_win32
#undef _mem_mmap_anon
#undef _mem_sbrk
#endif

#if _mem_mmap_anon /* may get space using mmap */
#include <sys/mman.h>
#ifndef MAP_ANON
#ifdef MAP_ANONYMOUS
#define MAP_ANON MAP_ANONYMOUS
#else
#define MAP_ANON 0
#endif /*MAP_ANONYMOUS*/
#endif /*MAP_ANON*/
#endif /*_mem_mmap_anon*/

/*
 * hint at "transparent huge pages" (=largepages) if
 * we allocate more memory than fit into 8 "normal"
 * MMU pages (we silently assume that largepages are
 * always at least 8 times larger than the next smaller
 * page size - which is true for all known platforms)
 */

#ifdef MADV_HUGEPAGE
#define ADVISE(v, a, z) ((a) && ((z) >= 8 * _Vmpagesize) ? madvise((a), (z), MADV_HUGEPAGE) : 0)
#else
#define ADVISE(v, a, z)
#endif

/*
 * report region v memory usage stats
 */

#define USAGE(v, a, z)                                            \
    if ((a) && (_Vmassert & VM_usage)) {                          \
        Vmstat_t vs;                                              \
        debug_printf(2, "vmalloc: %p %zu %s\n", (a), (z),         \
                     _vmstat(v, &vs, z) >= 0 ? vs.mesg : "init"); \
    }

/*
 * return addr a size z from region v
 */

#undef RETURN

#define RETURN(v, a, z) \
    if (a) {            \
        USAGE(v, a, z); \
    }                   \
    return (a)

#if _mem_win32 /* getting memory on a window system */
#if _PACKAGE_ast
#include <ast_windows.h>
#else
#include <windows.h>
#endif

static void *win32mem(Vmalloc_t *vm, void *caddr, size_t csize, size_t nsize, Vmdisc_t *disc) {
    GETMEMCHK(vm, caddr, csize, nsize, disc);
    if (csize == 0) {
        caddr = (void *)VirtualAlloc(0, nsize, MEM_COMMIT, PAGE_READWRITE);
        RETURN(vm, caddr, nsize);
    } else if (nsize == 0) {
        (void)VirtualFree((LPVOID)caddr, 0, MEM_RELEASE);
        RETURN(vm, caddr, nsize);
    } else
        return NULL;
}
#endif /* _mem_win32 */

#if _mem_sbrk /* getting space via sbrk() */
/*
** this may be unsafe due to code external to Vmalloc that may also use sbrk()
*/
static void *sbrkmem(Vmalloc_t *vm, void *caddr, size_t csize, size_t nsize, Vmdisc_t *disc) {
    Vmuchar_t *newm;
    unsigned int key;

    GETMEMCHK(vm, caddr, csize, nsize, disc);
    if ((csize % _Vmpagesize) != 0)
        return NULL; /* bad call! */
    else if ((nsize = ROUND(nsize, _Vmpagesize)) == csize)
        return caddr;       /* nothing to do */
    else if (nsize < csize) /* no memory reduction */
        return NULL;

    key = asothreadid(); /**/
    DEBUG_ASSERT(key > 0);
    asolock(&_Vmsbrklock, key, ASO_LOCK);

    nsize -= csize; /* amount of new memory needed */

    if (csize > 0) {
        RESTARTMEM(newm, sbrk(0));
        if (newm && newm != ((Vmuchar_t *)caddr + csize)) {
            newm = NULL; /* non-contiguous memory */
            goto bad;
        }
    }
    RESTARTMEM(newm, sbrk(nsize));
    if (newm && csize > 0 && newm != ((Vmuchar_t *)caddr + csize + nsize))
        newm = NULL; /* non-contiguous memory again */

bad:
    asolock(&_Vmsbrklock, key, ASO_UNLOCK);

    RETURN(vm, (newm && caddr) ? caddr : newm, nsize);
}
#endif /* _mem_sbrk */

#if _mem_mmap_anon /* getting space via mmap(MAP_ANON) emulation of sbrk() */
/*
** concurrency-safe memory allocation via mmap-anonymous providing an emulation of sbrk()
*/
static void *safebrkmem(Vmalloc_t *vm, void *caddr, size_t csize, size_t nsize,
                          Vmdisc_t *disc) {
    Vmuchar_t *newm;
    unsigned int key;

    GETMEMCHK(vm, caddr, csize, nsize, disc);
    if ((csize % _Vmpagesize) != 0)
        return NULL; /* bad call! */
    else if ((nsize = ROUND(nsize, _Vmpagesize)) == csize)
        return caddr;       /* nothing to do */
    else if (nsize < csize) /* no memory reduction */
        return NULL;

    key = asothreadid(); /**/
    DEBUG_ASSERT(key > 0);
    asolock(&_Vmsbrklock, key, ASO_LOCK);

    newm = NULL;
    nsize -= csize; /* amount of new memory needed */

    VMBOUNDARIES();
    if (_Vmmemsbrk && _Vmmemsbrk < _Vmmemmax) {
        if (_Vmchkmem && (_Vmassert & VM_check_seg)) /* mmap must use MAP_FIXED (eg, solaris) */
        {
            for (;;) /* search for a mappable address */
            {
                newm = (*_Vmchkmem)(_Vmmemsbrk, nsize) ? _Vmmemsbrk : NULL;
                if (csize > 0 && newm != ((Vmuchar_t *)caddr + csize)) {
                    newm = NULL;
                    break;
                } else if (newm)
                    break;
                else
                    _Vmmemsbrk += nsize;
            }
            if (newm)
                RESTARTMEM(newm, mmap((void *)newm, nsize, PROT_READ | PROT_WRITE,
                                      MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0));
        } else
            RESTARTMEM(newm, mmap((void *)_Vmmemsbrk, nsize, PROT_READ | PROT_WRITE,
                                  MAP_ANON | MAP_PRIVATE, -1, 0));

        if (newm) {
            if (csize > 0 &&
                newm != ((Vmuchar_t *)caddr + csize)) /* new memory is not contiguous */
            {
                munmap((void *)newm, nsize); /* remove it and wait for a call for new memory */
                newm = NULL;
            } else
                _Vmmemsbrk = newm + nsize;
        }
    }

    asolock(&_Vmsbrklock, key, ASO_UNLOCK);

    RETURN(vm, (newm && caddr) ? caddr : newm, nsize);
}
#endif /* _mem_mmap_anon */

#if _mem_mmap_anon /* getting space via mmap(MAP_ANON) */
static void *mmapanonmem(Vmalloc_t *vm, void *caddr, size_t csize, size_t nsize,
                           Vmdisc_t *disc) {
    GETMEMCHK(vm, caddr, csize, nsize, disc);
    if (csize == 0) {
        nsize = ROUND(nsize, _Vmpagesize);
        RESTARTMEM(caddr, mmap(NULL, nsize, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0));
        ADVISE(vm, caddr, nsize);
        RETURN(vm, caddr, nsize);
    } else if (nsize == 0) {
        (void)munmap(caddr, csize);
        RETURN(vm, caddr, nsize);
    } else
        return NULL;
}
#endif /* _mem_mmap_anon */

#if _std_malloc /* using native malloc as a last resort */
static void *mallocmem(Vmalloc_t *vm, void *caddr, size_t csize, size_t nsize, Vmdisc_t *disc) {
    GETMEMCHK(vm, caddr, csize, nsize, disc);
    if (csize == 0) {
        RESTARTMEM(caddr, malloc(nsize));
        RETURN(vm, caddr, nsize);
    } else if (nsize == 0) {
        free(caddr);
        RETURN(vm, caddr, nsize);
    } else
        return NULL;
}
#endif

/* A discipline to get raw memory */
static void *getmemory(Vmalloc_t *vm, void *caddr, size_t csize, size_t nsize, Vmdisc_t *disc) {
    Vmuchar_t *addr;

    if (_Vmemoryf) {
        (disc)->memoryf = _Vmemoryf;
        return (*_Vmemoryf)(vm, caddr, csize, nsize, disc);
    }
    GETMEMCHK(vm, caddr, csize, nsize, disc);
#if _mem_mmap_anon
    if ((_Vmassert & VM_safe) && (addr = safebrkmem(vm, caddr, csize, nsize, disc))) {
        GETMEMUSE(safebrkmem, disc);
        return (void *)addr;
    }
#endif
#if _mem_mmap_anon
    if ((_Vmassert & VM_anon) && (addr = mmapanonmem(vm, caddr, csize, nsize, disc))) {
        GETMEMUSE(mmapanonmem, disc);
        return (void *)addr;
    }
#endif
#if _mem_sbrk
    if ((_Vmassert & VM_break) && (addr = sbrkmem(vm, caddr, csize, nsize, disc))) {
        GETMEMUSE(sbrkmem, disc);
        return (void *)addr;
    }
#endif
#if _mem_win32
    if ((addr = win32mem(vm, caddr, csize, nsize, disc))) {
        GETMEMUSE(win32mem, disc);
        return (void *)addr;
    }
#endif
#if _std_malloc
    if ((_Vmassert & VM_native) && (addr = mallocmem(vm, caddr, csize, nsize, disc))) {
        GETMEMUSE(mallocmem, disc);
        return (void *)addr;
    }
#endif
    write(2, "vmalloc: panic: all memory allocation disciplines failed\n", 57);
    abort();
    return NULL;
}

static Memdisc_t _Vmdcsystem = {
    {getmemory, NULL, 0 * 64 * 1024 * 1024, sizeof(Memdisc_t)}, FD_INIT, 0};

__DEFINE__(Vmdisc_t *, Vmdcsystem, (Vmdisc_t *)(&_Vmdcsystem));
__DEFINE__(Vmdisc_t *, Vmdcsbrk, (Vmdisc_t *)(&_Vmdcsystem));

/* Note that the below function may be invoked from multiple threads.
** It initializes the Vmheap region to use the VMHEAPMETH method.
*/
#define HEAPINIT (1)
#define HEAPDONE (2)
#define VMHEAPMETH Vmbest

static unsigned int Init = 0;

int _vmheapbusy(void) { return asogetint(&Init) == HEAPINIT; }

Vmalloc_t *_vmheapinit(Vmalloc_t *vm) {
    Vmalloc_t *heap;
    unsigned int status;
    unsigned int vm_assert;

    /**/ DEBUG_ASSERT(!vm /* called from _vmstart() in malloc.c */ || vm == Vmheap);

    if ((status = asocasint(&Init, 0, HEAPINIT)) == HEAPDONE)
        return Vmheap;
    else if (status == HEAPINIT) /* someone else is doing initialization */
    {
        asospindecl(); /* so we wait until that is done */
        for (asospininit();; asospinnext())
            if ((status = asogetint(&Init)) != HEAPINIT) break;
        /**/ DEBUG_ASSERT(status == HEAPDONE);
        return Vmheap;
    } else /* we won the right to initialize Vmheap below */
    {      /**/
        DEBUG_ASSERT(status == 0);
        /**/ DEBUG_ASSERT(Init == HEAPINIT);
        _vmoptions(3);
        _Vmdcsystem.disc.round = _Vmsegsize;
    }

    vm_assert = _Vmassert;
    _Vmassert &= ~(VM_usage);
    heap = vmopen(Vmheap->disc, VMHEAPMETH, VM_HEAPINIT);
    _Vmassert = vm_assert;

    if (_Vmassert & VM_verbose)
        debug_printf(2, "vmalloc: pagesize=%zu segsize=%zu assert=0x%08x\n", _Vmpagesize,
                     _Vmsegsize, _Vmassert);

    if (!vm && heap != Vmheap) {
        if (heap)
            debug_printf(9, "\n\nFATAL: _vmheapinit() != Vmheap\n\n");
        else
            debug_printf(9, "\n\nFATAL: _vmheapinit() == 0\n\n");
    }

    /**/ DEBUG_ASSERT(Init == HEAPINIT);
    asocasint(&Init, HEAPINIT, heap == Vmheap ? HEAPDONE : 0);

    if (vm == Vmheap && Init == HEAPDONE) /* initialize malloc/free/realloc interface */
        free(NULL);

    return heap;
}

static void *heapalloc(Vmalloc_t *vm, size_t size, int local) {
    return (vm = _vmheapinit(vm)) ? (*vm->meth.allocf)(vm, size, local) : NULL;
}
static void *heapresize(Vmalloc_t *vm, void *obj, size_t size, int type, int local) {
    return (vm = _vmheapinit(vm)) ? (*vm->meth.resizef)(vm, obj, size, type, local) : NULL;
}
static int heapfree(Vmalloc_t *vm, void *obj, int local) {
    return (vm = _vmheapinit(vm)) ? (*vm->meth.freef)(vm, obj, local) : -1;
}
static void *heapalign(Vmalloc_t *vm, size_t size, size_t align, int local) {
    return (vm = _vmheapinit(vm)) ? (*vm->meth.alignf)(vm, size, align, local) : NULL;
}
static int heapstat(Vmalloc_t *vm, Vmstat_t *st, int local) {
    return (vm = _vmheapinit(vm)) ? (*vm->meth.statf)(vm, st, local) : -1;
}

Vmalloc_t _Vmheap = {
    {
        heapalloc, heapresize, heapfree, 0, /* nopf		*/
        heapstat, 0,                        /* eventf	*/
        heapalign, 0                        /* method ID	*/
    },
    NULL, /* file	name	*/
    0,    /* line number	*/
    0,    /* function	*/
    (Vmdisc_t *)(&_Vmdcsystem),
    NULL, /* see heapinit	*/
};

__DEFINE__(Vmalloc_t *, Vmheap, &_Vmheap);
__DEFINE__(Vmalloc_t *, Vmregion, &_Vmheap);
