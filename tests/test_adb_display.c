/* Real asynchronous subprocesses, no ADB/hardware. Include to exercise timeout
 * and debounce deterministically without multi-second test sleeps.
 * SPDX-License-Identifier: Apache-2.0 */
#include "../src/adb_display.c"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while (0)

static void delay(void)
{
#ifdef _WIN32
    Sleep(1);
#else
    const struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL);
#endif
}

static bool wait_sample(struct otg_display_monitor *m, struct otg_display_size *out)
{
    uint64_t deadline = now_ms() + 10000;
    while (now_ms() < deadline) {
        if (!running(m)) m->next_query = 0;
        if (otg_display_monitor_poll(m, out)) return true;
        delay();
    }
    return false;
}

static void check_parse(void)
{
    struct otg_display_sample s;
    CHECK(otg_display_parse("Physical size: 1080x2400\nOverride size: 720x1600\nSurfaceOrientation: 1\n", &s));
    CHECK(s.width == 720 && s.height == 1600 && s.rotation == 1);
    CHECK(otg_display_parse("Physical size: 2400x1080\n  Viewport EXTERNAL: displayId=2, orientation=3\n"
                            "  Viewport INTERNAL: displayId=0, orientation=0\n", &s));
    CHECK(s.width == 2400 && s.rotation == 0);
    CHECK(!otg_display_parse("Physical size: 1080x2400\nViewport EXTERNAL: displayId=1, orientation=1\n", &s));
    CHECK(!otg_display_parse("Physical size: 1080x2400\nSurfaceOrientation: 4\n", &s));
    CHECK(!otg_display_parse("Physical size: 0x2400\nSurfaceOrientation: 0\n", &s));
    CHECK(!otg_display_parse("Physical size: 32768x2400\nSurfaceOrientation: 0\n", &s));
    CHECK(!otg_display_parse("Physical size: 1080x2400junk\nSurfaceOrientation: 0\n", &s));
    CHECK(!otg_display_parse("Physical size: 1080x2400\nViewport INTERNAL: displayId=0\nViewport EXTERNAL: displayId=1 orientation=1\n", &s));
    bool locked = false;
    CHECK(parse_lock_state("mKeyguardShowing=true\nmShowingLockscreen=true\nmIsShowing=true\n", &locked) && locked);
    CHECK(parse_lock_state("mKeyguardShowing=false\nmShowingLockscreen=false\nmIsShowing=false\n", &locked) && !locked);
    CHECK(parse_companion_lock_state("Broadcast completed: result=0, data=\"locked\"\n", &locked) && locked);
    CHECK(parse_companion_lock_state("Broadcast completed: result=0, data=\"unlocked\"\n", &locked) && !locked);
}

static void check_observe(void)
{
    struct otg_display_monitor m = {0};
    m.initial_w = 309; m.initial_h = 549; // Rounded 3.5x sensitivity axes.
    struct otg_display_size size;
    struct otg_display_sample s = {1080, 1920, 0};
    CHECK(!observe(&m, &s, &size)); CHECK(observe(&m, &s, &size));
    CHECK(size.virtual_width == 309 && size.virtual_height == 549);
    CHECK(!observe(&m, &s, &size));
    s.rotation = 1; CHECK(!observe(&m, &s, &size));
    s.rotation = 0; CHECK(!observe(&m, &s, &size)); CHECK(!observe(&m, &s, &size));
    s.rotation = 3; CHECK(!observe(&m, &s, &size)); CHECK(observe(&m, &s, &size));
    CHECK(size.width == 1920 && size.height == 1080 && size.virtual_width == 549 && size.virtual_height == 309);
    s.rotation = 1; CHECK(!observe(&m, &s, &size)); CHECK(!observe(&m, &s, &size)); // 180 degrees: no size change.
    s.rotation = 2; CHECK(!observe(&m, &s, &size)); CHECK(observe(&m, &s, &size));
    CHECK(size.virtual_width == 309 && size.virtual_height == 549);
    s.width = 720; s.height = 1280;
    CHECK(!observe(&m, &s, &size)); CHECK(observe(&m, &s, &size));
    CHECK(size.virtual_width == 206 && size.virtual_height == 366);

    // Stale portrait settings while already landscape; natural-landscape tablet.
    memset(&m, 0, sizeof(m)); m.initial_w = 540; m.initial_h = 1200;
    s = (struct otg_display_sample){1080, 2400, 1};
    CHECK(!observe(&m, &s, &size)); CHECK(observe(&m, &s, &size));
    CHECK(size.virtual_width == 1200 && size.virtual_height == 540);
    memset(&m, 0, sizeof(m)); m.initial_w = 540; m.initial_h = 1200;
    s = (struct otg_display_sample){2400, 1080, 1};
    CHECK(!observe(&m, &s, &size)); CHECK(observe(&m, &s, &size));
    CHECK(size.virtual_width == 540 && size.virtual_height == 1200);
}

int main(int argc, char **argv)
{
    if (argc == 5 && !strcmp(argv[1], "-s")) {
        if (strcmp(argv[3], "shell") || strcmp(argv[4], "wm size; dumpsys input; dumpsys window policy")) return 5;
        if (!strcmp(argv[2], "slow")) { for (int i = 0; i < 10000; ++i) delay(); return 0; }
        if (!strcmp(argv[2], "fail")) { puts("Physical size: 1080x2400\nSurfaceOrientation: 1"); return 1; }
        if (!strcmp(argv[2], "overflow")) { for (int i = 0; i < OUTPUT_MAX + 1024; ++i) putchar('x'); return 0; }
        if (strcmp(argv[2], "exact-phone:5555")) return 6; // Never select another transport.
        puts("Physical size: 1080x2400");
        for (int i = 0; i < 3000; ++i) puts("ignored input diagnostic line");
        puts("Viewport INTERNAL: displayId=0, orientation=1");
        puts("mKeyguardShowing=false");
        puts("mShowingLockscreen=false");
        puts("mIsShowing=false");
        return 0;
    }
    check_parse(); check_observe();
    CHECK(!otg_display_monitor_create("adb", "", 1, 1));
    CHECK(!otg_display_monitor_create("adb", "phone", 0, 1));
    struct otg_display_size size;
    CHECK(!otg_display_monitor_poll(NULL, &size));
    otg_display_monitor_destroy(NULL);
    struct otg_display_monitor *success = otg_display_monitor_create(argv[0], "exact-phone:5555", 540, 1200);
    CHECK(success && wait_sample(success, &size));
    CHECK(size.width == 2400 && size.height == 1080 && size.virtual_width == 1200 && size.virtual_height == 540);
    otg_display_monitor_destroy(success);
    const char *modes[] = {"slow", "fail", "overflow"};
    for (unsigned i = 0; i < sizeof(modes) / sizeof(*modes); ++i) {
        struct otg_display_monitor *m = otg_display_monitor_create(argv[0], modes[i], 540, 1200);
        CHECK(m && !running(m));
        CHECK(!otg_display_monitor_poll(m, &size) && running(m));
        uint64_t deadline = now_ms() + 5000;
        if (!strcmp(modes[i], "slow")) m->deadline = 0;
        while (running(m) && now_ms() < deadline) {
            CHECK(!otg_display_monitor_poll(m, &size)); delay();
        }
        CHECK(!running(m) && !m->have_candidate && !m->have_published);
        otg_display_monitor_destroy(m);
    }
    // Cancellation while a query is in flight is also used on GUI stop/EOF.
    struct otg_display_monitor *m = otg_display_monitor_create(argv[0], "slow", 1, 1);
    CHECK(m && !otg_display_monitor_poll(m, &size));
    otg_display_monitor_destroy(m);
    puts("PASS: display parsing, scaling, debounce, failure, output bounds and cancellation");
    return 0;
}
