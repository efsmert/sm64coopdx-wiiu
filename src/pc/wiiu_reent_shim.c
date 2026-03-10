#ifdef TARGET_WII_U

#include <sys/reent.h>

// Route newlib reent lookup through WUT's thread-aware implementation.
extern struct _reent* __wut_getreent(void);

struct _reent* __getreent(void) {
    struct _reent* r = __wut_getreent();
    if (r != NULL) {
        return r;
    }
    return _GLOBAL_REENT;
}

#endif
