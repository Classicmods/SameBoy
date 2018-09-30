#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <time.h>
#include <assert.h>
#include <signal.h>
#include <stdarg.h>

#ifndef WIIU
#define AUDIO_FREQUENCY 384000
#else
/* Use the internal sample rate for the Wii U */
#define AUDIO_FREQUENCY 48000
#endif

#define FRAME_RATE (0x400000 / 70224.0)

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define snprintf _snprintf
#endif

#include <Core/gb.h>
#include "libretro.h"

#ifdef _WIN32
static const char slash = '\\';
#else
static const char slash = '/';
#endif

#define VIDEO_WIDTH 160
#define VIDEO_HEIGHT 144
#define VIDEO_PIXELS (VIDEO_WIDTH * VIDEO_HEIGHT)

#define RETRO_MEMORY_GAMEBOY_1_SRAM ((1 << 8) | RETRO_MEMORY_SAVE_RAM)
#define RETRO_MEMORY_GAMEBOY_1_RTC ((2 << 8) | RETRO_MEMORY_RTC)
#define RETRO_MEMORY_GAMEBOY_2_SRAM ((3 << 8) | RETRO_MEMORY_SAVE_RAM)
#define RETRO_MEMORY_GAMEBOY_2_RTC ((3 << 8) | RETRO_MEMORY_RTC)

#define RETRO_GAME_TYPE_GAMEBOY_LINK_2P 0x101

char battery_save_path[512];
char symbols_path[512];

enum model {
    MODEL_DMG,
    MODEL_CGB,
    MODEL_AGB,
    MODEL_AUTO
};

static const GB_model_t libretro_to_internal_model[] =
{
    [MODEL_DMG] = GB_MODEL_DMG_B,
    [MODEL_CGB] = GB_MODEL_CGB_E,
    [MODEL_AGB] = GB_MODEL_AGB
};

enum screen_layout {
    LAYOUT_TOP_DOWN,
    LAYOUT_LEFT_RIGHT
};

enum audio_out {
    GB_1,
    GB_2
};

enum mode{
    MODE_SINGLE_GAME,
    MODE_SINGLE_GAME_DUAL,
    MODE_DUAL_GAME
};

static enum model model[2];
static enum model auto_model = MODEL_CGB;

static uint32_t *frame_buf = NULL;
static uint32_t *frame_buf_copy = NULL;
static struct retro_log_callback logging;
static retro_log_printf_t log_cb;

static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

static unsigned emulated_devices = 1;
static bool initialized = false;
static unsigned screen_layout = 0;
static unsigned audio_out = 0;

static enum mode mode = MODE_SINGLE_GAME;

static bool geometry_updated = false;
static bool sameboy_dual = false;

static bool link_cable_emulation = false;
/*static bool infrared_emulation   = false;*/

signed short soundbuf[1024 * 2];

char retro_system_directory[4096];
char retro_save_directory[4096];
char retro_game_path[4096];

GB_gameboy_t gameboy[2];

extern const unsigned char dmg_boot[], cgb_boot[], agb_boot[];
extern const unsigned dmg_boot_length, cgb_boot_length, agb_boot_length;
bool vblank1_occurred = false, vblank2_occurred = false;

static void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
    (void)level;
    va_list va;
    va_start(va, fmt);
    vfprintf(stderr, fmt, va);
    va_end(va);
}

static struct retro_rumble_interface rumble;

static void extract_basename(char *buf, const char *path, size_t size)
{
   const char *base = strrchr(path, '/');
   if (!base)
      base = strrchr(path, '\\');
   if (!base)
      base = path;

   if (*base == '\\' || *base == '/')
      base++;

   strncpy(buf, base, size - 1);
   buf[size - 1] = '\0';

   char *ext = strrchr(buf, '.');
   if (ext)
      *ext = '\0';
}

static void GB_update_keys_status(GB_gameboy_t *gb, unsigned port)
{

    input_poll_cb();

    GB_set_key_state(gb, GB_KEY_RIGHT,input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT));
    GB_set_key_state(gb, GB_KEY_LEFT, input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT));
    GB_set_key_state(gb, GB_KEY_UP,input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP) );
    GB_set_key_state(gb, GB_KEY_DOWN,input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN));
    GB_set_key_state(gb, GB_KEY_A,input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A) );
    GB_set_key_state(gb, GB_KEY_B,input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B) );
    GB_set_key_state(gb, GB_KEY_SELECT,input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT));
    GB_set_key_state(gb, GB_KEY_START,input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START) );

    if (gb->rumble_state)
        rumble.set_rumble_state(port, RETRO_RUMBLE_STRONG, 65535);
    else
        rumble.set_rumble_state(port, RETRO_RUMBLE_STRONG, 0);

}


static void audio_callback(void *gb)
{
    size_t length = GB_apu_get_current_buffer_length(gb);

    while (length > sizeof(soundbuf) / 4)
    {
        GB_apu_copy_buffer(gb, (GB_sample_t *) soundbuf, 1024);
        audio_batch_cb(soundbuf, 1024);
        length -= 1024;
    }
    if (length) {
        GB_apu_copy_buffer(gb, (GB_sample_t *) soundbuf, length);
        audio_batch_cb(soundbuf, length);
    }
}

static void vblank1(GB_gameboy_t *gb)
{
    vblank1_occurred = true;
    if (audio_out == GB_1)
        audio_callback(gb);
}

static void vblank2(GB_gameboy_t *gb)
{
    vblank2_occurred = true;
    if (audio_out == GB_2)
        audio_callback(gb);
}

static uint8_t byte_to_send1 = 0xFF, byte_to_send2 = 0xFF;

static void serial_start1(GB_gameboy_t *gb, uint8_t byte_received)
{
    byte_to_send1 = byte_received;
}

static uint8_t serial_end1(GB_gameboy_t *gb)
{
    uint8_t ret = GB_serial_get_data(&gameboy[1]);
    GB_serial_set_data(&gameboy[1], byte_to_send1);
    return ret;
}

static void serial_start2(GB_gameboy_t *gb, uint8_t byte_received)
{
    byte_to_send2 = byte_received;
}

static uint8_t serial_end2(GB_gameboy_t *gb)
{
    uint8_t ret = GB_serial_get_data(&gameboy[0]);
    GB_serial_set_data(&gameboy[0], byte_to_send2);
    return ret;
}

static uint32_t rgb_encode(GB_gameboy_t *gb, uint8_t r, uint8_t g, uint8_t b)
{
    return r<<16|g<<8|b;
}

static retro_environment_t environ_cb;

/* variables for single cart mode */
static const struct retro_variable vars_single[] = {
    { "sameboy_dual", "Single cart dual mode (reload); disabled|enabled" },
    { "sameboy_color_correction_mode", "Color correction; off|correct curves|emulate hardware|preserve brightness" },
    { "sameboy_high_pass_filter_mode", "High-pass filter; off|accurate|remove dc offset" },
    { "sameboy_model", "Emulated model; Game Boy Color|Game Boy Advance|Auto|Game Boy" },
    { NULL }
};

/* variables for single cart dual gameboy mode */
static const struct retro_variable vars_single_dual[] = {
    { "sameboy_dual", "Single cart dual mode (reload); disabled|enabled" },
    { "sameboy_link", "Link cable emulation; enabled|disabled" },
    /*{ "sameboy_ir",   "Infrared Sensor Emulation; disabled|enabled" },*/
    { "sameboy_screen_layout", "Screen layout; top-down|left-right" },
    { "sameboy_audio_output", "Audio output; Game Boy #1|Game Boy #2" },
    { "sameboy_model_1", "Emulated model for Game Boy #1; Game Boy Color|Game Boy Advance|Auto|Game Boy" },
    { "sameboy_model_2", "Emulated model for Game Boy #2; Game Boy Color|Game Boy Advance|Auto|Game Boy" },
    { "sameboy_color_correction_mode_1", "Color correction for Game Boy #1; off|correct curves|emulate hardware|preserve brightness" },
    { "sameboy_color_correction_mode_2", "Color correction for Game Boy #2; off|correct curves|emulate hardware|preserve brightness" },
    { "sameboy_high_pass_filter_mode_1", "High-pass filter for Game Boy #1; off|accurate|remove dc offset" },
    { "sameboy_high_pass_filter_mode_2", "High-pass filter for Game Boy #2; off|accurate|remove dc offset" },
    { NULL }
};

/* variables for dual cart dual gameboy mode */
static const struct retro_variable vars_dual[] = {
    { "sameboy_link", "Link cable emulation; enabled|disabled" },
    /*{ "sameboy_ir",   "Infrared Sensor Emulation; disabled|enabled" },*/
    { "sameboy_screen_layout", "Screen layout; top-down|left-right" },
    { "sameboy_audio_output", "Audio output; Game Boy #1|Game Boy #2" },
    { "sameboy_model_1", "Emulated model for Game Boy #1; Game Boy Color|Game Boy Advance|Auto|Game Boy" },
    { "sameboy_model_2", "Emulated model for Game Boy #2; Game Boy Color|Game Boy Advance|Auto|Game Boy" },
    { "sameboy_color_correction_mode_1", "Color correction for Game Boy #1; off|correct curves|emulate hardware|preserve brightness" },
    { "sameboy_color_correction_mode_2", "Color correction for Game Boy #2; off|correct curves|emulate hardware|preserve brightness" },
    { "sameboy_high_pass_filter_mode_1", "High-pass filter for Game Boy #1; off|accurate|remove dc offset" },
    { "sameboy_high_pass_filter_mode_2", "High-pass filter for Game Boy #2; off|accurate|remove dc offset" },
    { NULL }
};

static const struct retro_subsystem_memory_info gb1_memory[] = {
    { "srm", RETRO_MEMORY_GAMEBOY_1_SRAM },
    { "rtc", RETRO_MEMORY_GAMEBOY_1_RTC },
};

static const struct retro_subsystem_memory_info gb2_memory[] = {
    { "srm", RETRO_MEMORY_GAMEBOY_2_SRAM },
    { "rtc", RETRO_MEMORY_GAMEBOY_2_RTC },
};

static const struct retro_subsystem_rom_info gb_roms[] = {
    { "GameBoy #1", "gb|gbc", true, false, true, gb1_memory, 1 },
    { "GameBoy #2", "gb|gbc", true, false, true, gb2_memory, 1 },
};

   static const struct retro_subsystem_info subsystems[] = {
      { "2 Player Game Boy Link", "gb_link_2p", gb_roms, 2, RETRO_GAME_TYPE_GAMEBOY_LINK_2P },
      { NULL },
};

static const struct retro_controller_description controllers[] = {
    { "Nintendo Game Boy", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0) },
};

static void set_link_cable_state(bool state)
{
    if (state && emulated_devices == 2)
    {
        GB_set_serial_transfer_start_callback(&gameboy[0], serial_start1);
        GB_set_serial_transfer_end_callback(&gameboy[0], serial_end1);
        GB_set_serial_transfer_start_callback(&gameboy[1], serial_start2);
        GB_set_serial_transfer_end_callback(&gameboy[1], serial_end2);
    }
    else if (!state)
    {
        GB_set_serial_transfer_start_callback(&gameboy[0], NULL);
        GB_set_serial_transfer_end_callback(&gameboy[0], NULL);
        GB_set_serial_transfer_start_callback(&gameboy[1], NULL);
        GB_set_serial_transfer_end_callback(&gameboy[1], NULL);
    }
}

static void init_for_current_model(unsigned id)
{
    unsigned i = id;
    enum model effective_model;

    effective_model = model[i];
    if (effective_model == MODEL_AUTO) {
        effective_model = auto_model;
    }


    if (GB_is_inited(&gameboy[i])) {
        GB_switch_model_and_reset(&gameboy[i], libretro_to_internal_model[effective_model]);
    }
    else {
        GB_init(&gameboy[i], libretro_to_internal_model[effective_model]);
    }
    const char *model_name = (const char *[]){"dmg", "cgb", "agb"}[effective_model];
    const unsigned char *boot_code = (const unsigned char *[]){dmg_boot, cgb_boot, agb_boot}[effective_model];
    unsigned boot_length = (unsigned []){dmg_boot_length, cgb_boot_length, agb_boot_length}[effective_model];

    char buf[256];
    snprintf(buf, sizeof(buf), "%s%c%s_boot.bin", retro_system_directory, slash, model_name);
    log_cb(RETRO_LOG_INFO, "Initializing as model: %s\n", model_name);
    log_cb(RETRO_LOG_INFO, "Loading boot image: %s\n", buf);
    if (GB_load_boot_rom(&gameboy[i], buf))
        GB_load_boot_rom_from_buffer(&gameboy[i], boot_code, boot_length);
    GB_set_user_data(&gameboy[i], (void*)NULL);
    GB_set_pixels_output(&gameboy[i],(unsigned int*)(frame_buf + i * VIDEO_PIXELS));
    GB_set_rgb_encode_callback(&gameboy[i], rgb_encode);
    GB_set_sample_rate(&gameboy[i], AUDIO_FREQUENCY);

    /* todo: attempt to make these more generic */
    GB_set_vblank_callback(&gameboy[0], (GB_vblank_callback_t) vblank1);
    if (emulated_devices == 2)
    {
        GB_set_vblank_callback(&gameboy[1], (GB_vblank_callback_t) vblank2);
        if (link_cable_emulation)
            set_link_cable_state(true);
    }

    struct retro_memory_descriptor descs[7];
    size_t size;
    uint16_t bank;


    /* todo: add netplay awareness for this so achievements can be granted on the respective client */
    i = 0;
    memset(descs, 0, sizeof(descs));

    descs[0].ptr   = GB_get_direct_access(&gameboy[i], GB_DIRECT_ACCESS_IE, &size, &bank);
    descs[0].start = 0xFFFF;
    descs[0].len   = 1;

    descs[1].ptr   = GB_get_direct_access(&gameboy[i], GB_DIRECT_ACCESS_HRAM, &size, &bank);
    descs[1].start = 0xFF80;
    descs[1].len   = 0x0080;

    descs[2].ptr   = GB_get_direct_access(&gameboy[i], GB_DIRECT_ACCESS_RAM, &size, &bank);
    descs[2].start = 0xC000;
    descs[2].len   = 0x2000;

    descs[3].ptr   = GB_get_direct_access(&gameboy[i], GB_DIRECT_ACCESS_CART_RAM, &size, &bank);
    descs[3].start = 0xA000;
    descs[3].len   = 0x2000;

    descs[4].ptr   = GB_get_direct_access(&gameboy[i], GB_DIRECT_ACCESS_VRAM, &size, &bank);
    descs[4].start = 0x8000;
    descs[4].len   = 0x2000;

    descs[5].ptr   = GB_get_direct_access(&gameboy[i], GB_DIRECT_ACCESS_ROM, &size, &bank);
    descs[5].start = 0x0000;
    descs[5].len   = 0x4000;
    descs[5].flags = RETRO_MEMDESC_CONST;

    descs[6].ptr   = GB_get_direct_access(&gameboy[i], GB_DIRECT_ACCESS_OAM, &size, &bank);
    descs[6].start = 0xFE00;
    descs[6].len   = 0x00A0;

    struct retro_memory_map mmaps;
    mmaps.descriptors = descs;
    mmaps.num_descriptors = sizeof(descs) / sizeof(descs[0]);
    environ_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &mmaps);
}

static void check_variables(bool link)
{
    struct retro_variable var = {0};
    if (!link)
    {
        var.key = "sameboy_color_correction_mode";
        var.value = NULL;
        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value && GB_is_cgb(&gameboy[0]))
        {
            if (strcmp(var.value, "off") == 0)
                GB_set_color_correction_mode(&gameboy[0], GB_COLOR_CORRECTION_DISABLED);
            else if (strcmp(var.value, "correct curves") == 0)
                GB_set_color_correction_mode(&gameboy[0], GB_COLOR_CORRECTION_CORRECT_CURVES);
            else if (strcmp(var.value, "emulate hardware") == 0)
                GB_set_color_correction_mode(&gameboy[0], GB_COLOR_CORRECTION_EMULATE_HARDWARE);
            else if (strcmp(var.value, "preserve brightness") == 0)
                GB_set_color_correction_mode(&gameboy[0], GB_COLOR_CORRECTION_PRESERVE_BRIGHTNESS);
        }

        var.key = "sameboy_high_pass_filter_mode";
        var.value = NULL;
        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        {
            if (strcmp(var.value, "off") == 0)
                GB_set_highpass_filter_mode(&gameboy[0], GB_HIGHPASS_OFF);
            else if (strcmp(var.value, "accurate") == 0)
                GB_set_highpass_filter_mode(&gameboy[0], GB_HIGHPASS_ACCURATE);
            else if (strcmp(var.value, "remove dc offset") == 0)
                GB_set_highpass_filter_mode(&gameboy[0], GB_HIGHPASS_REMOVE_DC_OFFSET);
        }

        var.key = "sameboy_model";
        var.value = NULL;
        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        {
            enum model new_model = model[0];
            if (strcmp(var.value, "Game Boy") == 0)
                new_model = MODEL_DMG;
            else if (strcmp(var.value, "Game Boy Color") == 0)
                new_model = MODEL_CGB;
            else if (strcmp(var.value, "Game Boy Advance") == 0)
                new_model = MODEL_AGB;
            else
                new_model = MODEL_AUTO;

            model[0] = new_model;
            init_for_current_model(0);
        }
    }
    else
    {
        var.key = "sameboy_color_correction_mode_1";
        var.value = NULL;
        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value && GB_is_cgb(&gameboy[0]))
        {
            if (strcmp(var.value, "off") == 0)
                GB_set_color_correction_mode(&gameboy[0], GB_COLOR_CORRECTION_DISABLED);
            else if (strcmp(var.value, "correct curves") == 0)
                GB_set_color_correction_mode(&gameboy[0], GB_COLOR_CORRECTION_CORRECT_CURVES);
            else if (strcmp(var.value, "emulate hardware") == 0)
                GB_set_color_correction_mode(&gameboy[0], GB_COLOR_CORRECTION_EMULATE_HARDWARE);
            else if (strcmp(var.value, "preserve brightness") == 0)
                GB_set_color_correction_mode(&gameboy[0], GB_COLOR_CORRECTION_PRESERVE_BRIGHTNESS);
        }

        var.key = "sameboy_color_correction_mode_2";
        var.value = NULL;
        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value && GB_is_cgb(&gameboy[1]))
        {
            if (strcmp(var.value, "off") == 0)
                GB_set_color_correction_mode(&gameboy[1], GB_COLOR_CORRECTION_DISABLED);
            else if (strcmp(var.value, "correct curves") == 0)
                GB_set_color_correction_mode(&gameboy[1], GB_COLOR_CORRECTION_CORRECT_CURVES);
            else if (strcmp(var.value, "emulate hardware") == 0)
                GB_set_color_correction_mode(&gameboy[1], GB_COLOR_CORRECTION_EMULATE_HARDWARE);
            else if (strcmp(var.value, "preserve brightness") == 0)
                GB_set_color_correction_mode(&gameboy[1], GB_COLOR_CORRECTION_PRESERVE_BRIGHTNESS);
        }

        var.key = "sameboy_high_pass_filter_mode_1";
        var.value = NULL;
        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        {
            if (strcmp(var.value, "off") == 0)
                GB_set_highpass_filter_mode(&gameboy[0], GB_HIGHPASS_OFF);
            else if (strcmp(var.value, "accurate") == 0)
                GB_set_highpass_filter_mode(&gameboy[0], GB_HIGHPASS_ACCURATE);
            else if (strcmp(var.value, "remove dc offset") == 0)
                GB_set_highpass_filter_mode(&gameboy[0], GB_HIGHPASS_REMOVE_DC_OFFSET);
        }

        var.key = "sameboy_high_pass_filter_mode_2";
        var.value = NULL;
        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        {
            if (strcmp(var.value, "off") == 0)
                GB_set_highpass_filter_mode(&gameboy[1], GB_HIGHPASS_OFF);
            else if (strcmp(var.value, "accurate") == 0)
                GB_set_highpass_filter_mode(&gameboy[1], GB_HIGHPASS_ACCURATE);
            else if (strcmp(var.value, "remove dc offset") == 0)
                GB_set_highpass_filter_mode(&gameboy[1], GB_HIGHPASS_REMOVE_DC_OFFSET);
        }

        var.key = "sameboy_model_1";
        var.value = NULL;
        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        {
            enum model new_model = model[0];
            if (strcmp(var.value, "Game Boy") == 0)
                new_model = MODEL_DMG;
            else if (strcmp(var.value, "Game Boy Color") == 0)
                new_model = MODEL_CGB;
            else if (strcmp(var.value, "Game Boy Advance") == 0)
                new_model = MODEL_AGB;
            else
                new_model = MODEL_AUTO;

            if (model[0] != new_model)
            {
                model[0] = new_model;
                init_for_current_model(0);
            }
        }

        var.key = "sameboy_model_2";
        var.value = NULL;
        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        {
            enum model new_model = model[1];
            if (strcmp(var.value, "Game Boy") == 0)
                new_model = MODEL_DMG;
            else if (strcmp(var.value, "Game Boy Color") == 0)
                new_model = MODEL_CGB;
            else if (strcmp(var.value, "Game Boy Advance") == 0)
                new_model = MODEL_AGB;
            else
                new_model = MODEL_AUTO;

            if (model[1] != new_model)
            {
                model[1] = new_model;
                init_for_current_model(1);
            }
        }

    }

    var.key = "sameboy_screen_layout";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (strcmp(var.value, "top-down") == 0)
            screen_layout = LAYOUT_TOP_DOWN;
        else
            screen_layout = LAYOUT_LEFT_RIGHT;

        geometry_updated = true;
    }

    var.key = "sameboy_dual";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (strcmp(var.value, "enabled") == 0)
            sameboy_dual = true;
        else
            sameboy_dual = false;
    }

    var.key = "sameboy_link";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        bool tmp = link_cable_emulation;
        if (strcmp(var.value, "enabled") == 0)
            link_cable_emulation = true;
        else
            link_cable_emulation = false;
        if (link_cable_emulation && link_cable_emulation != tmp)
            set_link_cable_state(true);
        else if (!link_cable_emulation && link_cable_emulation != tmp)
            set_link_cable_state(false);
    }

    var.key = "sameboy_audio_output";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (strcmp(var.value, "Game Boy #1") == 0)
            audio_out = GB_1;
        else
            audio_out = GB_2;
    }
}

void retro_init(void)
{
    const char *dir = NULL;

    if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir)
        snprintf(retro_system_directory, sizeof(retro_system_directory), "%s", dir);
    else
        snprintf(retro_system_directory, sizeof(retro_system_directory), "%s", ".");

    if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir)
        snprintf(retro_save_directory, sizeof(retro_save_directory), "%s", dir);
    else
        snprintf(retro_save_directory, sizeof(retro_save_directory), "%s", ".");

    static const struct retro_controller_info ports[] = {
        { controllers, 1 },
        { controllers, 1 },
        { NULL, 0 },
    };

    environ_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);

    struct retro_input_descriptor desc[] = {
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "Left" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "Up" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "Down" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "B" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "A" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "Left" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "Up" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "Down" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "B" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "A" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" },
        { 0 },
    };

    environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);

}

void retro_deinit(void)
{
    free(frame_buf);
    free(frame_buf_copy);
    frame_buf = NULL;
    frame_buf_copy = NULL;
}

unsigned retro_api_version(void)
{
    return RETRO_API_VERSION;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
    log_cb(RETRO_LOG_INFO, "Connecting device %u into port %u\n", device, port);
}

void retro_get_system_info(struct retro_system_info *info)
{
    memset(info, 0, sizeof(*info));
    info->library_name     = "SameBoy";
#ifdef GIT_VERSION
    info->library_version  = SAMEBOY_CORE_VERSION GIT_VERSION;
#else
    info->library_version  = SAMEBOY_CORE_VERSION;
#endif
    info->need_fullpath    = true;
    info->valid_extensions = "gb|gbc";
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
    struct retro_game_geometry geom;
    struct retro_system_timing timing = { FRAME_RATE, AUDIO_FREQUENCY };

    if (screen_layout == LAYOUT_TOP_DOWN) {
        geom.base_width = VIDEO_WIDTH;
        geom.base_height = VIDEO_HEIGHT * emulated_devices;
        geom.aspect_ratio = (double)VIDEO_WIDTH / (emulated_devices * VIDEO_HEIGHT);
    }else if (screen_layout == LAYOUT_LEFT_RIGHT) {
        geom.base_width = VIDEO_WIDTH * emulated_devices;
        geom.base_height = VIDEO_HEIGHT;
        geom.aspect_ratio = ((double)VIDEO_WIDTH * emulated_devices) / VIDEO_HEIGHT;
    }

    geom.max_width = VIDEO_WIDTH * emulated_devices;
    geom.max_height = VIDEO_HEIGHT * emulated_devices;

    info->geometry = geom;
    info->timing   = timing;
}

void retro_set_environment(retro_environment_t cb)
{
    environ_cb = cb;

    if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
        log_cb = logging.log;
    else
        log_cb = fallback_log;

    cb(RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO,  (void*)subsystems);
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
    audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
    input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
    input_state_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
    video_cb = cb;
}

void retro_reset(void)
{
    for (int i = 0; i < emulated_devices; i++)
       GB_reset(&gameboy[i]);

}

void retro_run(void)
{

    bool updated = false;

    if (!initialized)
        geometry_updated = false;

    if (geometry_updated) {
        struct retro_system_av_info info;
        retro_get_system_av_info(&info);
        geometry_updated = false;
        environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &info.geometry);
    }

    if (!frame_buf)
        return;

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
        check_variables(emulated_devices == 2 ? true : false);

    GB_update_keys_status(&gameboy[0], 0);
    if (emulated_devices == 2)
      GB_update_keys_status(&gameboy[1], 1);

    vblank1_occurred = vblank2_occurred = false;
    signed delta = 0;
    if (emulated_devices == 2)
    {
    while (!vblank1_occurred || !vblank2_occurred) {
            if (delta >= 0) {
                delta -= GB_run(&gameboy[0]);
            }
            else {
                delta += GB_run(&gameboy[1]);
            }
        }
    }
    else
        GB_run_frame(&gameboy[0]);

    if (screen_layout == LAYOUT_TOP_DOWN) {
        video_cb(frame_buf, VIDEO_WIDTH, VIDEO_HEIGHT * emulated_devices, VIDEO_WIDTH * sizeof(uint32_t));
    }else if (screen_layout == LAYOUT_LEFT_RIGHT) {
        /* use slow memcpy method for now */
        for (int index = 0; index < emulated_devices; index++) {
            for (int y = 0; y < VIDEO_HEIGHT; y++) {
                for (int x = 0; x < VIDEO_WIDTH; x++) {
                    frame_buf_copy[VIDEO_WIDTH * emulated_devices * y + (x + VIDEO_WIDTH * index)] = frame_buf[VIDEO_WIDTH * (y + VIDEO_HEIGHT * index) + x];
                }
            }
        }

        video_cb(frame_buf_copy, VIDEO_WIDTH * emulated_devices, VIDEO_HEIGHT, VIDEO_WIDTH * emulated_devices * sizeof(uint32_t));
    }

    initialized = true;
}

bool retro_load_game(const struct retro_game_info *info)
{
    environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void *)vars_single);
    check_variables(false);

    if (sameboy_dual)
    {
        emulated_devices = 2;
        mode = MODE_SINGLE_GAME_DUAL;
        environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void *)vars_single_dual);
        check_variables(true);
    }
    else
        mode = MODE_SINGLE_GAME;

    frame_buf = (uint32_t*)malloc(emulated_devices * VIDEO_PIXELS * sizeof(uint32_t));
    frame_buf_copy = (uint32_t*)malloc(emulated_devices * VIDEO_PIXELS * sizeof(uint32_t));

    memset(frame_buf, 0, emulated_devices * VIDEO_PIXELS * sizeof(uint32_t));
    memset(frame_buf_copy, 0, emulated_devices * VIDEO_PIXELS * sizeof(uint32_t));

    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
    {
        log_cb(RETRO_LOG_INFO, "XRGB8888 is not supported\n");
        return false;
    }

    auto_model = (info->path[strlen(info->path) - 1] & ~0x20) == 'C' ? MODEL_CGB : MODEL_DMG;
    snprintf(retro_game_path, sizeof(retro_game_path), "%s", info->path);

    for (int i = 0; i < emulated_devices; i++)
    {
        init_for_current_model(i);
        if (GB_load_rom(&gameboy[i],info->path))
        {
            log_cb(RETRO_LOG_INFO, "Failed to load ROM at %s\n", info->path);
            return false;
        }
    }

    bool achievements = true;
    environ_cb(RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS, &achievements);

    if (environ_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble))
        log_cb(RETRO_LOG_INFO, "Rumble environment supported\n");
    else
        log_cb(RETRO_LOG_INFO, "Rumble environment not supported\n");

    check_variables(emulated_devices == 2 ? true : false);

    /* hack: use upstream's file based I/O for Game Boy 2 battery in single cart mode */
    if (mode == MODE_SINGLE_GAME_DUAL)
    {
        char path[PATH_MAX];
        char file[PATH_MAX];

        extract_basename(file, retro_game_path, sizeof(file));
        snprintf (path, sizeof(path), "%s%c%s.srm.2", retro_save_directory, slash, file);
        log_cb(RETRO_LOG_INFO, "Loading battery for Game Boy 2 from: %s\n", path);
        GB_load_battery(&gameboy[1], path);
    }

    return true;
}

void retro_unload_game(void)
{
    /* hack: use upstream's file based I/O for Game Boy 2 battery in single cart mode */
    if (mode == MODE_SINGLE_GAME_DUAL)
    {
        char path[PATH_MAX];
        char file[PATH_MAX];

        extract_basename(file, retro_game_path, sizeof(file));
        snprintf (path, sizeof(path), "%s%c%s.srm.2", retro_save_directory, slash, file);
        log_cb(RETRO_LOG_INFO, "Saving battery for Game Boy 2 to: %s\n", path);
        GB_save_battery(&gameboy[1], path);
    }

    for (int i = 0; i < emulated_devices; i++)
    {
        log_cb(RETRO_LOG_INFO, "Unloading GB: %d\n", emulated_devices);
        GB_free(&gameboy[i]);
    }
}

unsigned retro_get_region(void)
{
    return RETRO_REGION_NTSC;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num_info)
{

    if (type == RETRO_GAME_TYPE_GAMEBOY_LINK_2P)
    {
        emulated_devices = 2;
        mode = MODE_DUAL_GAME;
    }
    else
        return false; /* all other types are unhandled for now */

    environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void *)vars_dual);
    check_variables(true);

    frame_buf = (uint32_t*)malloc(emulated_devices * VIDEO_PIXELS * sizeof(uint32_t));
    frame_buf_copy = (uint32_t*)malloc(emulated_devices * VIDEO_PIXELS * sizeof(uint32_t));

    memset(frame_buf, 0, emulated_devices * VIDEO_PIXELS * sizeof(uint32_t));
    memset(frame_buf_copy, 0, emulated_devices * VIDEO_PIXELS * sizeof(uint32_t));

    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
    {
        log_cb(RETRO_LOG_INFO, "XRGB8888 is not supported\n");
        return false;
    }

    auto_model = (info->path[strlen(info->path) - 1] & ~0x20) == 'C' ? MODEL_CGB : MODEL_DMG;
    snprintf(retro_game_path, sizeof(retro_game_path), "%s", info->path);

    for (int i = 0; i < emulated_devices; i++)
    {
        init_for_current_model(i);
        if (GB_load_rom(&gameboy[i], info[i].path))
        {
            log_cb(RETRO_LOG_INFO, "Failed to load ROM\n");
            return false;
        }
    }

    bool achievements = true;
    environ_cb(RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS, &achievements);

    if (environ_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble))
        log_cb(RETRO_LOG_INFO, "Rumble environment supported\n");
    else
        log_cb(RETRO_LOG_INFO, "Rumble environment not supported\n");

    check_variables(emulated_devices == 2 ? true : false);
    return true;
}

size_t retro_serialize_size(void)
{
    if (emulated_devices == 2)
        return GB_get_save_state_size(&gameboy[0]) + GB_get_save_state_size(&gameboy[1]);
    else
        return GB_get_save_state_size(&gameboy[0]);
}

bool retro_serialize(void *data, size_t size)
{

    if (!initialized)
        return false;

    void* save_data[2];
    size_t state_size[2];
    size_t offset = 0;

    for (int i = 0; i < emulated_devices; i++)
    {
        state_size[i] = GB_get_save_state_size(&gameboy[i]);
        save_data[i] = (uint8_t*)malloc(state_size[i]);
        GB_save_state_to_buffer(&gameboy[i], (uint8_t*) save_data[i]);
        memcpy(data + offset, save_data[i], state_size[i]);
        offset += state_size[i];
        free(save_data[i]);
    }

    if (data)
        return true;
    else
        return false;
}

bool retro_unserialize(const void *data, size_t size)
{
    void* save_data[2];
    size_t state_size[2];
    int ret;

    for (int i = 0; i < emulated_devices; i++)
    {
        state_size[i] = GB_get_save_state_size(&gameboy[i]);
        save_data[i] = (uint8_t*)malloc(state_size[i]);
        memcpy (save_data[i], data + (state_size[i] * i), state_size[i]);
        ret = GB_load_state_from_buffer(&gameboy[i], save_data[i], state_size[i]);
        free(save_data[i]);

        if (ret != 0)
            return false;
    }

    return true;

}

void *retro_get_memory_data(unsigned type)
{
    void* data = NULL;
    switch(mode)
    {
        case MODE_SINGLE_GAME:
        case MODE_SINGLE_GAME_DUAL: /* todo: hook this properly */
            {
                switch(type)
                {
                    case RETRO_MEMORY_SYSTEM_RAM:
                        data = gameboy[0].ram;
                        break;
                    case RETRO_MEMORY_SAVE_RAM:
                        if (gameboy[0].cartridge_type->has_battery && gameboy[0].mbc_ram_size != 0)
                            data = gameboy[0].mbc_ram;
                        else
                            data = NULL;
                        break;
                    case RETRO_MEMORY_VIDEO_RAM:
                        data = gameboy[0].vram;
                        break;
                    case RETRO_MEMORY_RTC:
                        if(gameboy[0].cartridge_type->has_battery)
                            data = &gameboy[0].rtc_real;
                        else
                            data = NULL;
                        break;
                    default:
                        break;
                }
            }
            break;
        case MODE_DUAL_GAME:
            {
                switch (type)
                {
                    case RETRO_MEMORY_GAMEBOY_1_SRAM:
                        if (gameboy[0].cartridge_type->has_battery && gameboy[0].mbc_ram_size != 0)
                            data = gameboy[0].mbc_ram;
                        else
                            data = NULL;
                        break;
                    case RETRO_MEMORY_GAMEBOY_2_SRAM:
                        if (gameboy[1].cartridge_type->has_battery && gameboy[1].mbc_ram_size != 0)
                            data = gameboy[1].mbc_ram;
                        else
                            data = NULL;
                        break;
                    case RETRO_MEMORY_GAMEBOY_1_RTC:
                        if(gameboy[0].cartridge_type->has_battery)
                            data = &gameboy[0].rtc_real;
                        else
                            data = NULL;
                        break;
                    case RETRO_MEMORY_GAMEBOY_2_RTC:
                        if(gameboy[1].cartridge_type->has_battery)
                            data = &gameboy[1].rtc_real;
                        else
                            data = NULL;
                        break;
                    default:
                        break;
                }
            }
            break;
        default:
            break;
    }
    return data;
}

size_t retro_get_memory_size(unsigned type)
{
    size_t size = 0;
    switch(mode)
    {
        case MODE_SINGLE_GAME:
        case MODE_SINGLE_GAME_DUAL: /* todo: hook this properly */
            {
                switch(type)
                {
                    case RETRO_MEMORY_SYSTEM_RAM:
                        size = gameboy[0].ram_size;
                        break;
                    case RETRO_MEMORY_SAVE_RAM:
                        if (gameboy[0].cartridge_type->has_battery && gameboy[0].mbc_ram_size != 0)
                            size = gameboy[0].mbc_ram_size;
                        else
                            size = 0;
                        break;
                    case RETRO_MEMORY_VIDEO_RAM:
                        size = gameboy[0].vram_size;
                        break;
                    case RETRO_MEMORY_RTC:
                        if(gameboy[0].cartridge_type->has_battery)
                            size = sizeof (gameboy[0].rtc_real);
                        else
                            size =  0;
                        break;
                    default:
                        break;
                }
            }
            break;
        case MODE_DUAL_GAME:
            {
                switch (type)
                {
                    case RETRO_MEMORY_GAMEBOY_1_SRAM:
                        if (gameboy[0].cartridge_type->has_battery && gameboy[0].mbc_ram_size != 0)
                            size = gameboy[0].mbc_ram_size;
                        else
                            size = 0;
                        break;
                    case RETRO_MEMORY_GAMEBOY_2_SRAM:
                        if (gameboy[1].cartridge_type->has_battery && gameboy[1].mbc_ram_size != 0)
                            size = gameboy[1].mbc_ram_size;
                        else
                            size = 0;
                        break;
                    case RETRO_MEMORY_GAMEBOY_1_RTC:
                        if(gameboy[0].cartridge_type->has_battery)
                            size = sizeof (gameboy[0].rtc_real);
                        break;
                    case RETRO_MEMORY_GAMEBOY_2_RTC:
                        if(gameboy[1].cartridge_type->has_battery)
                            size = sizeof (gameboy[1].rtc_real);
                        break;
                    default:
                        break;
                }
            }
            break;
        default:
            break;
    }

    return size;
}

void retro_cheat_reset(void)
{}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
    (void)index;
    (void)enabled;
    (void)code;
}

