/* A pack that exports its entry only: dlsym of the other names reaches the
 * shell's own forwarders, which the loader must not take for the pack's. */
extern int toolkit_add(int a, int b);

int recomp_title_main(const void *store)
{
    (void)store;
    return toolkit_add(7, 0);
}
