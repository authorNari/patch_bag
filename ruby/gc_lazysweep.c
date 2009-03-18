/* #define GC_PROF 0 */
#define GC_NOTIFY 0

#ifdef GC_PROF
#define PREPARE_GC_PROF double gc_time = 0, mark_time = 0, sweep_time = 0

#define GC_TIMER_START do {\
    printf("gc_start!\n");\
    gc_time = cputime();\
} while(0)

#define GC_TIMER_END do {\
    gc_time = cputime() - gc_time;\
    printf("gc_end!\n");\
    printf("gc_time : %10.30f\n", gc_time);\
} while(0)

#define GC_MARK_TIMER_START do {\
    printf("mark_start!\n");\
    mark_time = cputime();\
} while(0)

#define GC_MARK_TIMER_END do {\
    mark_time = cputime() - mark_time;\
    printf("mark_time : %10.30f\n", mark_time);\
    printf("mark_end!\n");\
} while(0)

#define GC_SWEEP_TIMER_START do {\
    printf("sweep_start!\n");\
    sweep_time = cputime();\
} while(0)

#define GC_SWEEP_TIMER_END do {\
    sweep_time = cputime() - sweep_time;\
    printf("sweep_time : %10.30f\n", sweep_time);\
    printf("sweep_end!\n");\
} while(0)

#define GC_INFO_PUT do {\
    printf("heaps_used : %d\n", heaps_used);\
    printf("heaps_inc : %d\n", heaps_inc);\
    printf("heaps_sweep_inc : %d\n", heaps_sweep_inc);\
    printf("live : %d\n", live);\
    printf("freed : %d\n", heaps_used * HEAP_OBJ_LIMIT - live);\
} while(0)

#else
#define PREPARE_GC_PROF
#define GC_TIMER_START
#define GC_TIMER_END
#define GC_MARK_TIMER_START
#define GC_MARK_TIMER_END
#define GC_SWEEP_TIMER_START
#define GC_SWEEP_TIMER_END
#define GC_INFO_PUT
#endif

#ifdef GC_PROF
double cputime(void)
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec + (double)tv.tv_usec*1e-6;
}
#endif

void
init_gc_profile(rb_objspace_t *objspace)
{
    gc_profile = rb_ary_new();
}

#endif

#define HEAPS_SWEEP_INC_MIN 5
#define HEAPS_SWEEP_INC_MAX 100

void
set_lazy_sweep_params(rb_objspace_t *objspace)
{
    size_t free_min = 0;

    dead = 0;
    heaps_sweep_index = 0;
    heaps_sweep_inc = (heaps_used / 10);
    do_heap_free = (heaps_used * HEAP_OBJ_LIMIT) * 0.65;
    free_min = (heaps_used * HEAP_OBJ_LIMIT)  * 0.01;
    if (free_min < FREE_MIN) free_min = FREE_MIN;
    if (free_min > (heaps_used * HEAP_OBJ_LIMIT - live)) {
	set_heaps_increment(objspace);
	heaps_sweep_inc = (heaps_used + heaps_sweep_inc) / heaps_sweep_inc + 1;
    }
    if (heaps_sweep_inc < HEAPS_SWEEP_INC_MIN) heaps_sweep_inc = HEAPS_SWEEP_INC_MIN;
    if (heaps_sweep_inc > HEAPS_SWEEP_INC_MAX) heaps_sweep_inc = HEAPS_SWEEP_INC_MAX;
}

static void
slot_sweep(rb_objspace_t *objspace, struct heaps_slot *target)
{
    RVALUE *p, *pend, *free, *final;
    int freed = 0;

    during_gc_sweep = Qtrue;
    final = deferred_final_list;
    free = freelist;
    p = target->slot; pend = p + target->limit;
    while (p < pend) {
        if (!(p->as.basic.flags & FL_MARK)) {
            if (p->as.basic.flags) {
                obj_free(objspace, (VALUE)p);
            }
            if (need_call_final && FL_TEST(p, FL_FINALIZE)) {
                p->as.free.flags = FL_MARK; /* remain marked */
                p->as.free.next = deferred_final_list;
                deferred_final_list = p;
            }
            else {
                VALGRIND_MAKE_MEM_UNDEFINED((void*)p, sizeof(RVALUE));
                p->as.free.flags = 0;
                p->as.free.next = freelist;
                freelist = p;
            }
            freed++;
        }
        else if (RBASIC(p)->flags == FL_MARK) {
        }
        else {
            p->as.basic.flags &= ~FL_MARK;
        }
        p++;
    }
    dead += freed;
    if (freed == target->limit && dead > do_heap_free) {
        RVALUE *pp;

        target->limit = 0;
        target->state = GARBAGE;
	need_slot_free = Qtrue;
        for (pp = deferred_final_list; pp != final; pp = pp->as.free.next) {
            pp->as.free.flags |= FL_SINGLETON; /* freeing page mark */
        }
        freelist = free;	/* cancel this page from freelist */
    }
    else {
        target->state = NEED_MARK;
    }
}

static void
heaps_sweep_increment(rb_objspace_t *objspace)
{
    int i = 0;

    while (i < heaps_sweep_inc && heaps_sweep_index < heaps_used) {
	if (heaps[heaps_sweep_index].state == NEED_SWEEP) {
	    slot_sweep(objspace, &heaps[heaps_sweep_index]);
            i++;
        }
        heaps_sweep_index++;
    }
}

static void
heaps_sweep(rb_objspace_t *objspace)
{
    while (!freelist && heaps_sweep_index < heaps_used) {
	if (heaps[heaps_sweep_index].state == NEED_SWEEP) {
	    slot_sweep(objspace, &heaps[heaps_sweep_index]);
	}
        heaps_sweep_index++;
    }
}

static int
gc_lazysweep(rb_objspace_t *objspace, rb_thread_t *th)
{
    heaps_increment(objspace);
    heaps_sweep_increment(objspace);

    if (!freelist && heaps_sweep_index < heaps_used) {
	heaps_sweep(objspace);
    }

    if (!freelist) {
	if (!deferred_final_list && need_slot_free) {
	    free_unused_heaps(objspace);
	}
	return Qfalse;
    }
    return Qtrue;
}

int
garbage_collect_lazysweep(rb_objspace_t *objspace)
{
    PREPARE_GC_PROF;
    GC_TIMER_START;

    rb_thread_t *th = GET_THREAD();

    if (GC_NOTIFY) printf("start garbage_collect_lazysweep()\n");

    if (!heaps) {
        return Qfalse;
    }

    if (dont_gc || during_gc) {
        if (!freelist) {
            if (!heaps_increment(objspace)) {
                set_heaps_increment(objspace);
                heaps_increment(objspace);
            }
        }
        return Qtrue;
    }
    during_gc++;
    objspace->count++;

    GC_SWEEP_TIMER_START;
    if (!gc_lazysweep(objspace, th)) {

	GC_MARK_TIMER_START;

        gc_marks(objspace, th);
	during_gc_sweep = Qtrue;

	GC_MARK_TIMER_END;
	GC_INFO_PUT;

        if (!heaps_increment(objspace)) {
	    gc_lazysweep(objspace, th);
	}
    }

    GC_SWEEP_TIMER_END;

    GC_TIMER_END;

    if (GC_NOTIFY) printf("end garbage_collect_lazysweep()\n");
    during_gc = 0;

    return Qtrue;
}

