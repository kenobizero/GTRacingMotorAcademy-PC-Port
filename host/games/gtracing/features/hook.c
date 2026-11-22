/* hook.c -- the vtable-slot hooks and in-place changes of game data the features make (game.h). */
#include "plat.h"

#include "features.h"
#include "game.h"

void gt_unprotect(u32 addr, u32 n)
{
    DWORD old;
    VirtualProtect(GP(addr), n, PAGE_READWRITE, &old);
}

int gt_hook_slot(u32 slot, u32 fn, void (**orig)(CPU *), void (*h)(CPU *), const char *name)
{
    void (*f)(CPU *) = rt_lookup(fn);
    if (!f || rd32(slot) != fn) {
        rt_log("[hook] %s: slot 0x%x holds 0x%x, not 0x%x: not hooked\n", name, slot, rd32(slot), fn);
        return 0;
    }
    if (orig)
        *orig = f;
    gt_unprotect(slot, 4);
    wr32(slot, rt_host_addr(h, name));
    return 1;
}
