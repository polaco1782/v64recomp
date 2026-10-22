static const char *data_output;
static void debug_capture_startup(Runtime *rt) {
    for (uint32_t a = BOOT_BSS_START; a < BOOT_BSS_END; a += 4)
        require(recomp_load(rt, a, 4) == 0, "GoldenEye boot did not clear BSS", a);
    FILE *stream = fopen(data_output, "wb");
    require(stream != NULL, "cannot open GoldenEye startup capture", 0);
    uint8_t *data = malloc(BOOT_DATA_SIZE);
    require(data != NULL, "cannot allocate GoldenEye startup capture", 0);
    uint32_t physical = recomp_ram_address(rt, BOOT_DATA_ADDRESS, BOOT_DATA_SIZE);
    for (uint32_t i = 0; i < BOOT_DATA_SIZE; i++)
        data[i] = rt->rdram[(physical + i) ^ 3];
    bool wrote = fwrite(data, 1, BOOT_DATA_SIZE, stream) == BOOT_DATA_SIZE;
    free(data);
    int closed = fclose(stream);
    require(wrote && closed == 0, "cannot write GoldenEye startup capture", 0);
}
