/* Acceptance-test controller route. This reads game state and writes SI input
 * only; the translated game owns movement, doors, warps and asset loading. */
static unsigned route_waypoint, route_level, route_level_frames, route_target_frames;
static bool route_done;
static bool route_mode(void) {
    return !strcmp(mode, "interior");
}
static void route_observe(unsigned level) {
    if (level != route_level) {
        route_level = level;
        route_waypoint = 0;
        route_level_frames = 0;
        printf("Route entered level %u at playable frame %u.\n", level, play_frames);
        fflush(stdout);
    }
    route_level_frames++;
    if (level == 6)
        route_target_frames++;
    route_done = route_target_frames >= goal;
}
static void route_input(Runtime *rt) {
    uint32_t m = (uint32_t)recomp_load(rt, GAME_GMARIOSTATE, 4), c = (uint32_t)recomp_load(rt, GAME_GCAMERA, 4);
    unsigned level = (unsigned)recomp_load(rt, GAME_GCURRLEVELNUM, 2);
    if (!m || !c)
        return;
    float x = guest_float(rt, m + 0x3c), z = guest_float(rt, m + 0x44);
    /* The two exterior door objects are at x=-76/77, y=803, z=-3155
     * in the original game's loaded Castle Grounds object pool. */
    static const float grounds[][2] = {{0, 4000}, {0, 1000}, {0, -3400}};
    const float (*points)[2] = grounds;
    unsigned count = sizeof(grounds) / sizeof(grounds[0]);
    if (level != 16)
        return;
    while (route_waypoint + 1 < count && hypotf(points[route_waypoint][0] - x, points[route_waypoint][1] - z) < 100)
        route_waypoint++;
    float dx = points[route_waypoint][0] - x, dz = points[route_waypoint][1] - z;
    float yaw = (float)(uint16_t)recomp_load(rt, c + 2, 2) * (6.283185307179586f / 65536);
    float angle = atan2f(dx, dz) - yaw;
    input.stick_x = (int8_t)lroundf(sinf(angle) * 80);
    input.stick_y = (int8_t)lroundf(-cosf(angle) * 80);
    if (route_level_frames % 60 == 0) {
        printf("Route level %u waypoint %u: (%.1f, %.1f), stick (%d, %d), camera yaw %.3f.\n", level, route_waypoint, x,
               z, input.stick_x, input.stick_y, yaw);
        fflush(stdout);
    }
}
