/* relload's module A (XBA-264): a REL built by gc_tools.elf2rel from this object. It calls back into the DOL
 * (rel_svc_mul3, rel_shared_counter: module 0 imports), reads its own table and calls its own function through a
 * pointer in its data (self imports). _prolog(5) = (5 * 3 + 1) + 100 + (1 + 2 + 3 + 4) = 126. */
extern int rel_svc_mul3(int x);
extern int rel_shared_counter;

static const int table[4] = {1, 2, 3, 4};
static int scratch[8]; /* bss */

static int helper(int x)
{
    return x + 100;
}

int (*const a_fp)(int) = helper;

int _prolog(int x);
int _prolog(int x)
{
    int s = 0;
    rel_shared_counter += 1;
    for (int i = 0; i < 4; i++) {
        scratch[i] = table[i];
        s += scratch[i];
    }
    return a_fp(rel_svc_mul3(x)) + s;
}
