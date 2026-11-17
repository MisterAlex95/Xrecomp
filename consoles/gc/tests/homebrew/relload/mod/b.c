/* relload's module B (XBA-264): loaded where A was, after A is unlinked. _prolog(x) = (x * x * 3 + 1) + 20, the 20
 * from a loop over a switch (some branches for the lifter); it counts its calls in rel_shared_counter (module 0) and
 * in its own bss. relload's main calls it every field. */
extern int rel_svc_mul3(int x);
extern int rel_shared_counter;

static int calls;

static int step(int x, int k)
{
    switch (k & 3) {
    case 0: return x + k;
    case 1: return x ^ k;
    case 2: return x - k;
    default: return x + 2 * k;
    }
}

int _prolog(int x);
int _prolog(int x)
{
    int v = 0;
    for (int k = 0; k < 8; k++)
        v = step(v, k);
    calls++;
    rel_shared_counter += 1;
    return rel_svc_mul3(x * x) + v + (calls > 0 ? 0 : 1);
}
