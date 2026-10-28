/*
 * sram_test.c — Headless test for the libretro SRAM interface.
 *
 * Tests:
 *   1. SRAM write: Load eeprom_test ROM, run frames, verify SRAM buffer
 *      contains expected big-endian packed EEPROM data.
 *   2. SRAM load: Pre-fill SRAM buffer, run one frame (triggers unpack),
 *      save again and verify round-trip.
 *   3. Memory size: Verify retro_get_memory_size returns correct value.
 *
 * Build:
 *   cc -o sram_test sram_test.c -ldl  (Linux)
 *   cc -o sram_test sram_test.c       (macOS)
 *
 * Usage:
 *   ./sram_test <core.dylib|so> <eeprom_test.j64>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __APPLE__
#include <dlfcn.h>
#define LIBEXT "dylib"
#else
#include <dlfcn.h>
#define LIBEXT "so"
#endif

/* Minimal libretro types needed for loading the core */
#include "../../libretro-common/include/libretro.h"

/* Function pointer types for the core API */
typedef void   (*retro_init_t)(void);
typedef void   (*retro_deinit_t)(void);
typedef void   (*retro_set_environment_t)(retro_environment_t);
typedef void   (*retro_set_video_refresh_t)(retro_video_refresh_t);
typedef void   (*retro_set_audio_sample_t)(retro_audio_sample_t);
typedef void   (*retro_set_audio_sample_batch_t)(retro_audio_sample_batch_t);
typedef void   (*retro_set_input_poll_t)(retro_input_poll_t);
typedef void   (*retro_set_input_state_t)(retro_input_state_t);
typedef bool   (*retro_load_game_t)(const struct retro_game_info *);
typedef void   (*retro_unload_game_t)(void);
typedef void   (*retro_run_t)(void);
typedef void   (*retro_reset_t)(void);
typedef void  *(*retro_get_memory_data_t)(unsigned);
typedef size_t (*retro_get_memory_size_t)(unsigned);

/* Dummy callbacks */
static bool env_cb(unsigned cmd, void *data)
{
    switch (cmd)
    {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        return false;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE:
        return false;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        if (data) *(bool *)data = false;
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        if (data) *(const char **)data = ".";
        return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        if (data) *(const char **)data = ".";
        return true;
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK:
    case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
    case RETRO_ENVIRONMENT_GET_VFS_INTERFACE:
        return false;
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        if (data) *(unsigned *)data = 0;
        return true;
    default:
        return false;
    }
}

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
    (void)data; (void)w; (void)h; (void)pitch;
}
static void audio_cb(int16_t left, int16_t right)
{
    (void)left; (void)right;
}
static size_t audio_batch(const int16_t *data, size_t frames)
{
    (void)data;
    return frames;
}
static void input_poll(void) {}
static int16_t input_state(unsigned port, unsigned dev, unsigned idx, unsigned id)
{
    (void)port; (void)dev; (void)idx; (void)id;
    return 0;
}

/* Load a symbol from the core or die */
static void *load_sym(void *handle, const char *name)
{
    void *sym = dlsym(handle, name);
    if (!sym)
    {
        fprintf(stderr, "ERROR: Missing symbol: %s\n", name);
        exit(1);
    }
    return sym;
}

/* Read a file into a malloc'd buffer */
static uint8_t *read_file(const char *path, size_t *size_out)
{
    FILE *fp = fopen(path, "rb");
    uint8_t *buf;
    size_t sz;

    if (!fp) { perror(path); exit(1); }
    fseek(fp, 0, SEEK_END);
    sz = (size_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);
    buf = (uint8_t *)malloc(sz);
    if (!buf) { fprintf(stderr, "OOM\n"); exit(1); }
    if (fread(buf, 1, sz, fp) != sz)
    {
        fprintf(stderr, "Short read: %s\n", path);
        exit(1);
    }
    fclose(fp);
    *size_out = sz;
    return buf;
}

int main(int argc, char **argv)
{
    void *handle;
    uint8_t *rom_data;
    size_t rom_size;
    struct retro_game_info game;
    int pass = 0, fail = 0;

    /* Core API functions */
    retro_init_t                core_init;
    retro_deinit_t              core_deinit;
    retro_set_environment_t     core_set_env;
    retro_set_video_refresh_t   core_set_video;
    retro_set_audio_sample_t    core_set_audio;
    retro_set_audio_sample_batch_t core_set_audio_batch;
    retro_set_input_poll_t      core_set_input_poll;
    retro_set_input_state_t     core_set_input_state;
    retro_load_game_t           core_load_game;
    retro_unload_game_t         core_unload_game;
    retro_run_t                 core_run;
    retro_reset_t               core_reset;
    retro_get_memory_data_t     core_get_memory_data;
    retro_get_memory_size_t     core_get_memory_size;

    if (argc != 3)
    {
        fprintf(stderr, "Usage: %s <core.dylib|so> <eeprom_test.j64>\n", argv[0]);
        return 1;
    }

    /* Load core */
    handle = dlopen(argv[1], RTLD_NOW);
    if (!handle)
    {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }

    core_init            = (retro_init_t)load_sym(handle, "retro_init");
    core_deinit          = (retro_deinit_t)load_sym(handle, "retro_deinit");
    core_set_env         = (retro_set_environment_t)load_sym(handle, "retro_set_environment");
    core_set_video       = (retro_set_video_refresh_t)load_sym(handle, "retro_set_video_refresh");
    core_set_audio       = (retro_set_audio_sample_t)load_sym(handle, "retro_set_audio_sample");
    core_set_audio_batch = (retro_set_audio_sample_batch_t)load_sym(handle, "retro_set_audio_sample_batch");
    core_set_input_poll  = (retro_set_input_poll_t)load_sym(handle, "retro_set_input_poll");
    core_set_input_state = (retro_set_input_state_t)load_sym(handle, "retro_set_input_state");
    core_load_game       = (retro_load_game_t)load_sym(handle, "retro_load_game");
    core_unload_game     = (retro_unload_game_t)load_sym(handle, "retro_unload_game");
    core_run             = (retro_run_t)load_sym(handle, "retro_run");
    core_reset           = (retro_reset_t)load_sym(handle, "retro_reset");
    core_get_memory_data = (retro_get_memory_data_t)load_sym(handle, "retro_get_memory_data");
    core_get_memory_size = (retro_get_memory_size_t)load_sym(handle, "retro_get_memory_size");

    /* Set up callbacks */
    core_set_env(env_cb);
    core_set_video(video_cb);
    core_set_audio(audio_cb);
    core_set_audio_batch(audio_batch);
    core_set_input_poll(input_poll);
    core_set_input_state(input_state);

    core_init();

    /* Load ROM */
    rom_data = read_file(argv[2], &rom_size);
    memset(&game, 0, sizeof(game));
    game.path = argv[2];
    game.data = rom_data;
    game.size = rom_size;

    if (!core_load_game(&game))
    {
        fprintf(stderr, "ERROR: retro_load_game failed\n");
        free(rom_data);
        dlclose(handle);
        return 1;
    }

    printf("==> SRAM Test Suite\n\n");

    /* ---- Test 1: Memory size ---- */
    {
        size_t sram_size = core_get_memory_size(RETRO_MEMORY_SAVE_RAM);
        printf("Test 1: retro_get_memory_size(SAVE_RAM) = %zu ... ", sram_size);
        if (sram_size == 128)
        {
            printf("PASS\n");
            pass++;
        }
        else
        {
            printf("FAIL (expected 128)\n");
            fail++;
        }
    }

    /* ---- Test 2: Memory data pointer ---- */
    {
        void *sram_ptr = core_get_memory_data(RETRO_MEMORY_SAVE_RAM);
        printf("Test 2: retro_get_memory_data(SAVE_RAM) != NULL ... ");
        if (sram_ptr != NULL)
        {
            printf("PASS (ptr=%p)\n", sram_ptr);
            pass++;
        }
        else
        {
            printf("FAIL\n");
            fail++;
        }
    }

    /* ---- Test 3: EEPROM write detection ---- */
    /* Run enough frames for the 68K to execute the EEPROM write sequence.
     * The ROM writes immediately on boot, so a few frames should suffice. */
    {
        int i;
        uint8_t *sram;
        size_t sram_size;
        int writes_ok = 1;

        /* Expected EEPROM values (big-endian packed in the save buffer):
         * Address 0: 0xCAFE → bytes [0]=0xCA, [1]=0xFE
         * Address 1: 0xBEEF → bytes [2]=0xBE, [3]=0xEF
         * Address 2: 0xDEAD → bytes [4]=0xDE, [5]=0xAD
         * Address 63: 0x1234 → bytes [126]=0x12, [127]=0x34 */
        struct { int offset; uint8_t hi; uint8_t lo; const char *desc; } checks[] = {
            {   0, 0xCA, 0xFE, "addr 0 = 0xCAFE" },
            {   2, 0xBE, 0xEF, "addr 1 = 0xBEEF" },
            {   4, 0xDE, 0xAD, "addr 2 = 0xDEAD" },
            { 126, 0x12, 0x34, "addr 63 = 0x1234" },
        };
        int num_checks = sizeof(checks) / sizeof(checks[0]);

        printf("Test 3: Running 300 frames for EEPROM writes...\n");
        for (i = 0; i < 300; i++)
            core_run();

        sram = (uint8_t *)core_get_memory_data(RETRO_MEMORY_SAVE_RAM);
        sram_size = core_get_memory_size(RETRO_MEMORY_SAVE_RAM);

        if (!sram || sram_size < 128)
        {
            printf("   FAIL: SRAM not available after run\n");
            fail++;
        }
        else
        {
            for (i = 0; i < num_checks; i++)
            {
                int off = checks[i].offset;
                printf("   Check %s: [%d]=0x%02X [%d]=0x%02X ... ",
                       checks[i].desc, off, sram[off], off + 1, sram[off + 1]);
                if (sram[off] == checks[i].hi && sram[off + 1] == checks[i].lo)
                {
                    printf("PASS\n");
                }
                else
                {
                    printf("FAIL (expected 0x%02X%02X)\n", checks[i].hi, checks[i].lo);
                    writes_ok = 0;
                }
            }

            if (writes_ok)
            {
                printf("   PASS: All EEPROM writes detected in SRAM buffer\n");
                pass++;
            }
            else
            {
                fail++;
            }
        }
    }

    /* ---- Test 4: SRAM load (round-trip) ---- */
    /* Unload, reload, pre-fill SRAM, run one frame to trigger unpack,
     * then verify the buffer survives. */
    {
        uint8_t test_pattern[128];
        uint8_t *sram;
        int i, match;

        core_unload_game();

        /* Prepare test pattern: each word = address * 0x0101 */
        for (i = 0; i < 64; i++)
        {
            test_pattern[i * 2 + 0] = (uint8_t)i;       /* high byte */
            test_pattern[i * 2 + 1] = (uint8_t)(i ^ 0xFF); /* low byte */
        }

        /* Reload */
        if (!core_load_game(&game))
        {
            printf("Test 4: FAIL (retro_load_game failed on reload)\n");
            fail++;
        }
        else
        {
            /* Pre-fill the SRAM buffer (simulating frontend .srm load) */
            sram = (uint8_t *)core_get_memory_data(RETRO_MEMORY_SAVE_RAM);
            if (sram)
                memcpy(sram, test_pattern, 128);

            /* Run one frame — triggers eeprom_unpack_save_buf */
            core_run();

            /* The EEPROM test ROM will immediately overwrite the EEPROM
             * with its own values (0xCAFE etc.), so instead we verify that
             * addresses NOT written by the ROM still have our test pattern.
             * Check addresses 3-62 (not touched by the ROM). */
            sram = (uint8_t *)core_get_memory_data(RETRO_MEMORY_SAVE_RAM);
            match = 1;
            printf("Test 4: SRAM load round-trip (addresses 3-62)...\n");
            for (i = 3; i < 63; i++)
            {
                uint8_t expect_hi = (uint8_t)i;
                uint8_t expect_lo = (uint8_t)(i ^ 0xFF);
                int off = i * 2;
                if (sram[off] != expect_hi || sram[off + 1] != expect_lo)
                {
                    printf("   FAIL at address %d: got 0x%02X%02X, expected 0x%02X%02X\n",
                           i, sram[off], sram[off + 1], expect_hi, expect_lo);
                    match = 0;
                }
            }
            if (match)
            {
                printf("   PASS: Pre-loaded SRAM data survives unpack/repack cycle\n");
                pass++;
            }
            else
            {
                fail++;
            }
        }
    }

    /* ---- Test 5: SRAM survives soft reset ---- */
    {
        uint8_t pre_reset[128];
        uint8_t *sram;
        int match;

        sram = (uint8_t *)core_get_memory_data(RETRO_MEMORY_SAVE_RAM);
        if (sram)
        {
            memcpy(pre_reset, sram, 128);
            core_reset();
            /* Run a few frames post-reset */
            {
                int i;
                for (i = 0; i < 10; i++)
                    core_run();
            }

            sram = (uint8_t *)core_get_memory_data(RETRO_MEMORY_SAVE_RAM);
            /* After reset + a few frames, the ROM will re-write its known
             * addresses (0,1,2,63). Check that the other addresses are
             * preserved from before reset. */
            match = 1;
            printf("Test 5: SRAM survives soft reset (addresses 3-62)...\n");
            {
                int i;
                for (i = 3; i < 63; i++)
                {
                    int off = i * 2;
                    if (sram[off] != pre_reset[off] || sram[off + 1] != pre_reset[off + 1])
                    {
                        printf("   FAIL at address %d: got 0x%02X%02X, was 0x%02X%02X\n",
                               i, sram[off], sram[off + 1], pre_reset[off], pre_reset[off + 1]);
                        match = 0;
                    }
                }
            }
            if (match)
            {
                printf("   PASS: SRAM preserved across retro_reset()\n");
                pass++;
            }
            else
            {
                fail++;
            }
        }
        else
        {
            printf("Test 5: FAIL (no SRAM pointer)\n");
            fail++;
        }
    }

    /* Cleanup */
    core_unload_game();
    core_deinit();
    free(rom_data);
    dlclose(handle);

    printf("\n==> Results: %d passed, %d failed\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
