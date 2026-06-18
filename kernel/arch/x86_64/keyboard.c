#include <arch/cpu.h>
#include <arch/io.h>
#include <arch/keyboard.h>
#include <arch/pit.h>
#include <tnu/string.h>
#include <tnu/types.h>

/*
 * TNU i8042/PS2 keyboard driver.
 *
 * This supports:
 * - QEMU PS/2 keyboards
 * - most bare-metal laptop internal keyboards
 *
 * Important:
 * many laptop internal keyboards are still exposed through the legacy
 * i8042 controller, even on modern machines.
 *
 * External USB keyboards are different: they need either firmware USB legacy
 * emulation or a future USB HID keyboard driver.
 */

#define KBD_BUFFER 256
/* Separate event buffer for /dev/input/kbd: uint16_t events with 0x8000=release */
#define KBD_EVENT_BUFFER 1024
static uint16_t evt_buffer[KBD_EVENT_BUFFER];
static volatile size_t evt_read_pos;
static volatile size_t evt_write_pos;

static void push_event(uint16_t ev)
{
    size_t next = (evt_write_pos + 1) % KBD_EVENT_BUFFER;
    if (next == evt_read_pos) {
        /* Buffer full: drop oldest event (advance read_pos) to make room */
        evt_read_pos = (evt_read_pos + 1) % KBD_EVENT_BUFFER;
    }
    evt_buffer[evt_write_pos] = ev;
    evt_write_pos = next;
}

int keyboard_try_get_event(void)
{
    keyboard_poll();
    if (evt_read_pos == evt_write_pos) return -1;
    uint16_t ev = evt_buffer[evt_read_pos];
    evt_read_pos = (evt_read_pos + 1) % KBD_EVENT_BUFFER;
    return (int)(unsigned int)ev;
}

bool keyboard_event_available(void)
{
    keyboard_poll();
    return evt_read_pos != evt_write_pos;
}

#define I8042_DATA 0x60
#define I8042_STATUS 0x64
#define I8042_COMMAND 0x64

#define I8042_STATUS_OUTPUT_FULL 0x01
#define I8042_STATUS_INPUT_FULL 0x02
#define I8042_STATUS_AUX_DATA 0x20

#define I8042_CMD_READ_CONFIG 0x20
#define I8042_CMD_WRITE_CONFIG 0x60
#define I8042_CMD_SELF_TEST 0xaa
#define I8042_CMD_TEST_FIRST_PORT 0xab
#define I8042_CMD_DISABLE_FIRST_PORT 0xad
#define I8042_CMD_ENABLE_FIRST_PORT 0xae
#define I8042_CMD_DISABLE_SECOND_PORT 0xa7

#define I8042_SELF_TEST_OK 0x55
#define I8042_FIRST_PORT_TEST_OK 0x00

#define I8042_CONFIG_IRQ1 0x01
#define I8042_CONFIG_IRQ12 0x02
#define I8042_CONFIG_FIRST_PORT_CLOCK_DISABLE 0x10
#define I8042_CONFIG_SECOND_PORT_CLOCK_DISABLE 0x20
#define I8042_CONFIG_TRANSLATION 0x40

#define PS2_CMD_SET_LEDS 0xed
#define PS2_CMD_ENABLE_SCANNING 0xf4
#define PS2_CMD_DISABLE_SCANNING 0xf5
#define PS2_CMD_SET_DEFAULTS 0xf6

#define PS2_ACK 0xfa
#define PS2_RESEND 0xfe
#define PS2_BAT_OK 0xaa

#define WAIT_ITERS 100000
#define FLUSH_LIMIT 256
#define PS2_COMMAND_RETRIES 3

static int buffer[KBD_BUFFER];
static volatile size_t read_pos;
static volatile size_t write_pos;
static volatile uint64_t interrupt_generation;
static volatile uint64_t interrupt_consumed_generation;
static volatile uint64_t irq_count = 0;
static volatile uint64_t poll_count = 0;

static bool key_down[128];
static bool ext_key_down[128];

static int shift_down;
static int left_shift_down;
static int right_shift_down;
static int ctrl_down;
static int alt_down;
static int altgr_down;
static int caps_lock;
static int extended_prefix;
static int set2_break_prefix;
static int raw_set2_mode;
static uint64_t backspace_last_make_tick;

/*
 * We intentionally use translated scancode set 1.
 * This is the most compatible mode for QEMU, old BIOS-style firmware,
 * and many laptop i8042 controllers.
 */
static int scancode_set = 1;

static const char *layout = "us";

static void keyboard_controller_setup(void);
static void process_scancode(uint8_t scancode);

static const char normal_map[128] = {
    [0x01] = 27,  [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
    [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0a] = '9',
    [0x0b] = '0', [0x0c] = '-', [0x0d] = '=', [0x0e] = '\b',
    [0x0f] = '\t', [0x10] = 'q', [0x11] = 'w', [0x12] = 'e',
    [0x13] = 'r', [0x14] = 't', [0x15] = 'y', [0x16] = 'u',
    [0x17] = 'i', [0x18] = 'o', [0x19] = 'p', [0x1a] = '[',
    [0x1b] = ']', [0x1c] = '\n', [0x1e] = 'a', [0x1f] = 's',
    [0x20] = 'd', [0x21] = 'f', [0x22] = 'g', [0x23] = 'h',
    [0x24] = 'j', [0x25] = 'k', [0x26] = 'l', [0x27] = ';',
    [0x28] = '\'', [0x29] = '`', [0x2b] = '\\', [0x2c] = 'z',
    [0x2d] = 'x', [0x2e] = 'c', [0x2f] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.',
    [0x35] = '/', [0x39] = ' ', [0x56] = '<',
};

static const char shift_map[128] = {
    [0x01] = 27,  [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$',
    [0x06] = '%', [0x07] = '^', [0x08] = '&', [0x09] = '*', [0x0a] = '(',
    [0x0b] = ')', [0x0c] = '_', [0x0d] = '+', [0x0e] = '\b',
    [0x0f] = '\t', [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E',
    [0x13] = 'R', [0x14] = 'T', [0x15] = 'Y', [0x16] = 'U',
    [0x17] = 'I', [0x18] = 'O', [0x19] = 'P', [0x1a] = '{',
    [0x1b] = '}', [0x1c] = '\n', [0x1e] = 'A', [0x1f] = 'S',
    [0x20] = 'D', [0x21] = 'F', [0x22] = 'G', [0x23] = 'H',
    [0x24] = 'J', [0x25] = 'K', [0x26] = 'L', [0x27] = ':',
    [0x28] = '"', [0x29] = '~', [0x2b] = '|', [0x2c] = 'Z',
    [0x2d] = 'X', [0x2e] = 'C', [0x2f] = 'V', [0x30] = 'B',
    [0x31] = 'N', [0x32] = 'M', [0x33] = '<', [0x34] = '>',
    [0x35] = '?', [0x39] = ' ', [0x56] = '>',
};

static const char it_normal_map[128] = {
    [0x01] = 27,  [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
    [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0a] = '9',
    [0x0b] = '0', [0x0c] = '-', [0x0d] = '\'', [0x0e] = '\b',
    [0x0f] = '\t', [0x10] = 'q', [0x11] = 'w', [0x12] = 'e',
    [0x13] = 'r', [0x14] = 't', [0x15] = 'y', [0x16] = 'u',
    [0x17] = 'i', [0x18] = 'o', [0x19] = 'p', [0x1a] = 'e',
    [0x1b] = '+', [0x1c] = '\n', [0x1e] = 'a', [0x1f] = 's',
    [0x20] = 'd', [0x21] = 'f', [0x22] = 'g', [0x23] = 'h',
    [0x24] = 'j', [0x25] = 'k', [0x26] = 'l', [0x27] = 'o',
    [0x28] = 'a', [0x29] = '\\', [0x2b] = 'u', [0x2c] = 'z',
    [0x2d] = 'x', [0x2e] = 'c', [0x2f] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.',
    [0x35] = '-', [0x39] = ' ', [0x56] = '<',
};

static const char it_shift_map[128] = {
    [0x01] = 27,  [0x02] = '!', [0x03] = '"', [0x04] = '#', [0x05] = '$',
    [0x06] = '%', [0x07] = '&', [0x08] = '/', [0x09] = '(', [0x0a] = ')',
    [0x0b] = '=', [0x0c] = '_', [0x0d] = '?', [0x0e] = '\b',
    [0x0f] = '\t', [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E',
    [0x13] = 'R', [0x14] = 'T', [0x15] = 'Y', [0x16] = 'U',
    [0x17] = 'I', [0x18] = 'O', [0x19] = 'P', [0x1a] = 'E',
    [0x1b] = '*', [0x1c] = '\n', [0x1e] = 'A', [0x1f] = 'S',
    [0x20] = 'D', [0x21] = 'F', [0x22] = 'G', [0x23] = 'H',
    [0x24] = 'J', [0x25] = 'K', [0x26] = 'L', [0x27] = 'O',
    [0x28] = 'A', [0x29] = '|', [0x2b] = 'U', [0x2c] = 'Z',
    [0x2d] = 'X', [0x2e] = 'C', [0x2f] = 'V', [0x30] = 'B',
    [0x31] = 'N', [0x32] = 'M', [0x33] = ';', [0x34] = ':',
    [0x35] = '_', [0x39] = ' ', [0x56] = '>',
};

static const char it_altgr_map[128] = {
    [0x1a] = '[', [0x1b] = ']', [0x27] = '@', [0x28] = '#',
};

static const char it_altgr_shift_map[128] = {
    [0x1a] = '{', [0x1b] = '}', [0x27] = '@', [0x28] = '#',
};

#define IT_CP437_E_ACUTE  0x82
#define IT_CP437_A_GRAVE  0x85
#define IT_CP437_E_GRAVE  0x8a
#define IT_CP437_I_GRAVE  0x8d
#define IT_CP437_O_GRAVE  0x95
#define IT_CP437_U_GRAVE  0x97

static const unsigned char it_accent_map[128] = {
    [0x0d] = IT_CP437_I_GRAVE,
    [0x1a] = IT_CP437_E_GRAVE,
    [0x27] = IT_CP437_O_GRAVE,
    [0x28] = IT_CP437_A_GRAVE,
    [0x2b] = IT_CP437_U_GRAVE,
};

static const unsigned char it_accent_shift_map[128] = {
    [0x1a] = IT_CP437_E_ACUTE,
};

static const uint8_t set2_to_set1[128] = {
    [0x76] = 0x01, [0x16] = 0x02, [0x1e] = 0x03, [0x26] = 0x04,
    [0x25] = 0x05, [0x2e] = 0x06, [0x36] = 0x07, [0x3d] = 0x08,
    [0x3e] = 0x09, [0x46] = 0x0a, [0x45] = 0x0b, [0x4e] = 0x0c,
    [0x55] = 0x0d, [0x66] = 0x0e, [0x0d] = 0x0f, [0x15] = 0x10,
    [0x1d] = 0x11, [0x24] = 0x12, [0x2d] = 0x13, [0x2c] = 0x14,
    [0x35] = 0x15, [0x3c] = 0x16, [0x43] = 0x17, [0x44] = 0x18,
    [0x4d] = 0x19, [0x54] = 0x1a, [0x5b] = 0x1b, [0x5a] = 0x1c,
    [0x14] = 0x1d, [0x1c] = 0x1e, [0x1b] = 0x1f, [0x23] = 0x20,
    [0x2b] = 0x21, [0x34] = 0x22, [0x33] = 0x23, [0x3b] = 0x24,
    [0x42] = 0x25, [0x4b] = 0x26, [0x4c] = 0x27, [0x52] = 0x28,
    [0x0e] = 0x29, [0x12] = 0x2a, [0x5d] = 0x2b, [0x1a] = 0x2c,
    [0x22] = 0x2d, [0x21] = 0x2e, [0x2a] = 0x2f, [0x32] = 0x30,
    [0x31] = 0x31, [0x3a] = 0x32, [0x41] = 0x33, [0x49] = 0x34,
    [0x4a] = 0x35, [0x59] = 0x36, [0x11] = 0x38, [0x29] = 0x39,
    [0x58] = 0x3a, [0x61] = 0x56,
};

static const uint8_t set2_ext_to_set1[128] = {
    [0x14] = 0x1d, [0x11] = 0x38, [0x5a] = 0x1c,
    [0x6c] = 0x47, [0x75] = 0x48, [0x6b] = 0x4b, [0x74] = 0x4d,
    [0x69] = 0x4f, [0x72] = 0x50, [0x71] = 0x53,
};

static void push_key(int key)
{
    size_t next = (write_pos + 1) % KBD_BUFFER;
    if (next == read_pos) {
        return;
    }

    buffer[write_pos] = key;
    write_pos = next;
    /* Also push a make-event (no release flag) to the event buffer */
    push_event((uint16_t)(key & 0x7fff));
}

static void i8042_io_wait(void)
{
    for (volatile size_t i = 0; i < 64; i++) {
        cpu_pause();
    }
}

static int wait_input_clear(void)
{
    for (size_t i = 0; i < WAIT_ITERS; i++) {
        if (!(inb(I8042_STATUS) & I8042_STATUS_INPUT_FULL)) {
            return 0;
        }
        cpu_pause();
    }

    return -1;
}

static int wait_output_full(void)
{
    for (size_t i = 0; i < WAIT_ITERS; i++) {
        if (inb(I8042_STATUS) & I8042_STATUS_OUTPUT_FULL) {
            return (int)inb(I8042_DATA);
        }
        cpu_pause();
    }

    return -1;
}

static void drain_output(void)
{
    for (size_t i = 0; i < FLUSH_LIMIT; i++) {
        uint8_t status = inb(I8042_STATUS);
        if (!(status & I8042_STATUS_OUTPUT_FULL)) {
            return;
        }

        (void)inb(I8042_DATA);
        i8042_io_wait();
    }
}

static int controller_command(uint8_t command)
{
    if (wait_input_clear() < 0) {
        return -1;
    }

    outb(I8042_COMMAND, command);
    return 0;
}

static int controller_read_config(void)
{
    if (controller_command(I8042_CMD_READ_CONFIG) < 0) {
        return -1;
    }

    return wait_output_full();
}

static int controller_write_config(uint8_t config)
{
    if (controller_command(I8042_CMD_WRITE_CONFIG) < 0) {
        return -1;
    }

    if (wait_input_clear() < 0) {
        return -1;
    }

    outb(I8042_DATA, config);
    return 0;
}

static int ps2_wait_ack(void)
{
    /*
     * During early boot on real hardware there can be stale BAT bytes,
     * translated scancodes, or mouse bytes in the output buffer.
     * Ignore non-command bytes while waiting for the keyboard's response.
     * AUX/mouse bytes are explicitly discarded using status bit 5.
     */
    for (size_t i = 0; i < WAIT_ITERS; i++) {
        uint8_t status = inb(I8042_STATUS);

        if (!(status & I8042_STATUS_OUTPUT_FULL)) {
            cpu_pause();
            continue;
        }

        uint8_t value = inb(I8042_DATA);

        if (status & I8042_STATUS_AUX_DATA) {
            continue;
        }

        if (value == PS2_ACK) {
            return 0;
        }

        if (value == PS2_RESEND) {
            return 1;
        }

        /* Keyboard BAT/self-test success can arrive after reset/defaults. */
        if (value == PS2_BAT_OK) {
            continue;
        }
    }

    return -1;
}

static int ps2_write(uint8_t value)
{
    for (size_t tries = 0; tries < PS2_COMMAND_RETRIES; tries++) {
        if (wait_input_clear() < 0) {
            return -1;
        }

        outb(I8042_DATA, value);

        int ack = ps2_wait_ack();
        if (ack == 0) {
            return 0;
        }

        if (ack < 0) {
            return -1;
        }

        /* ack == 1 means RESEND: retry the same command byte. */
    }

    return -1;
}

static void ps2_write_ignore_ack(uint8_t value)
{
    (void)ps2_write(value);
}

static void keyboard_controller_setup(void)
{
    /*
     * Robust i8042 init for QEMU and bare-metal laptops.
     * Do not hard-fail if firmware returns weird values: many laptops expose
     * the internal keyboard through i8042 but have imperfect controller tests.
     */

    (void)controller_command(I8042_CMD_DISABLE_FIRST_PORT);
    (void)controller_command(I8042_CMD_DISABLE_SECOND_PORT);
    drain_output();

    int config_value = controller_read_config();
    uint8_t config = config_value >= 0 ? (uint8_t)config_value : 0;

    /* Disable IRQs and clocks during setup. */
    config &= (uint8_t)~(I8042_CONFIG_IRQ1 | I8042_CONFIG_IRQ12);
    config |= I8042_CONFIG_FIRST_PORT_CLOCK_DISABLE;
    config |= I8042_CONFIG_SECOND_PORT_CLOCK_DISABLE;

    /*
     * Keep translation ON so received keyboard bytes are set 1.
     * Never request set 2 while this bit is enabled.
     */
    config |= I8042_CONFIG_TRANSLATION;

    (void)controller_write_config(config);

    /*
     * Controller self-test.  I8042_SELF_TEST_OK is the documented success
     * value, but real firmware and virtual machines can be quirky here.
     * Continue even on timeout or an unexpected value because the firmware may
     * already have left a working laptop keyboard behind the first port.
     */
    (void)controller_command(I8042_CMD_SELF_TEST);
    (void)wait_output_full();

    /* Some controllers reset config after self-test. Restore our safe config. */
    (void)controller_write_config(config);

    /*
     * Test first PS/2 port.  I8042_FIRST_PORT_TEST_OK is success, but this is
     * deliberately non-fatal on laptops because several machines report odd
     * values while the internal keyboard still works.
     */
    (void)controller_command(I8042_CMD_TEST_FIRST_PORT);
    (void)wait_output_full();

    /* Enable first port, where laptop internal keyboards usually live. */
    (void)controller_command(I8042_CMD_ENABLE_FIRST_PORT);

    /* Enable IRQ1, enable first-port clock, keep second port disabled. */
    config &= (uint8_t)~I8042_CONFIG_FIRST_PORT_CLOCK_DISABLE;
    config |= I8042_CONFIG_SECOND_PORT_CLOCK_DISABLE;
    config &= (uint8_t)~I8042_CONFIG_IRQ12;
    config |= I8042_CONFIG_IRQ1;
    config |= I8042_CONFIG_TRANSLATION;

    (void)controller_write_config(config);
    int active_config = controller_read_config();
    if (active_config >= 0 && !(active_config & I8042_CONFIG_TRANSLATION)) {
        raw_set2_mode = 1;
        scancode_set = 2;
    } else {
        raw_set2_mode = 0;
        scancode_set = 1;
    }

    drain_output();

    /*
     * Default keyboard mode + scanning.
     * Do NOT use PS2_CMD_SET_SCANCODE_SET here because translation is ON.
     */
    ps2_write_ignore_ack(PS2_CMD_DISABLE_SCANNING);
    ps2_write_ignore_ack(PS2_CMD_SET_DEFAULTS);

    if (!raw_set2_mode) {
        scancode_set = 1;
    }

    ps2_write_ignore_ack(PS2_CMD_ENABLE_SCANNING);
    drain_output();
}

void keyboard_init(void)
{
    read_pos = 0;
    write_pos = 0;

    memset(key_down, 0, sizeof(key_down));
    memset(ext_key_down, 0, sizeof(ext_key_down));

    shift_down = 0;
    left_shift_down = 0;
    right_shift_down = 0;
    ctrl_down = 0;
    alt_down = 0;
    caps_lock = 0;
    extended_prefix = 0;
    set2_break_prefix = 0;
    raw_set2_mode = 0;
    backspace_last_make_tick = 0;
    scancode_set = 1;
    layout = "us";

    keyboard_controller_setup();
}

void keyboard_reset_console_state(void)
{
    read_pos = 0;
    write_pos = 0;
    evt_read_pos = 0;
    evt_write_pos = 0;

    memset(key_down, 0, sizeof(key_down));
    memset(ext_key_down, 0, sizeof(ext_key_down));

    shift_down = 0;
    left_shift_down = 0;
    right_shift_down = 0;
    ctrl_down = 0;
    alt_down = 0;
    altgr_down = 0;
    extended_prefix = 0;
    set2_break_prefix = 0;
    backspace_last_make_tick = 0;
}

static int is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static char shifted_with_caps(char normal, char shifted)
{
    if (!normal) {
        return 0;
    }

    if (caps_lock && is_alpha(normal) && is_alpha(shifted)) {
        return shift_down ? normal : shifted;
    }

    return shift_down ? shifted : normal;
}

static char remap_layout(char c)
{
    if (strcmp(layout, "de") == 0) {
        if (c == 'y') return 'z';
        if (c == 'Y') return 'Z';
        if (c == 'z') return 'y';
        if (c == 'Z') return 'Y';
    } else if (strcmp(layout, "fr") == 0) {
        if (c == 'q') return 'a';
        if (c == 'Q') return 'A';
        if (c == 'a') return 'q';
        if (c == 'A') return 'Q';
        if (c == 'w') return 'z';
        if (c == 'W') return 'Z';
        if (c == 'z') return 'w';
        if (c == 'Z') return 'W';
    } else if (strcmp(layout, "dvorak") == 0) {
        const char *qwerty = "qwertyuiop[]asdfghjkl;'zxcvbnm,./";
        const char *dvorak = "',.pyfgcrl/=aoeuidhtns-;qjkxbmwvz";
        const char *qwerty_shift = "QWERTYUIOP{}ASDFGHJKL:\"ZXCVBNM<>?";
        const char *dvorak_shift = "\"<>PYFGCRL?+AOEUIDHTNS_:QJKXBMWVZ";

        char *p = strchr(qwerty, c);
        if (p) return dvorak[p - qwerty];

        p = strchr(qwerty_shift, c);
        if (p) return dvorak_shift[p - qwerty_shift];
    } else if (strcmp(layout, "uk") == 0) {
        if (c == '#') return '~';
    } else if (strcmp(layout, "it") == 0 || strcmp(layout, "es") == 0) {
        if (c == '`') return '\'';
        if (c == '~') return '"';
    }

    return c;
}

static char layout_char(uint8_t scancode)
{
    if (scancode >= 128) {
        return 0;
    }

    if (strcmp(layout, "it") == 0) {
        if (altgr_down) {
            char c = shift_down ? it_altgr_shift_map[scancode] : it_altgr_map[scancode];
            if (c) {
                return c;
            }
        }
        if (shift_down && it_accent_shift_map[scancode]) {
            return (char)it_accent_shift_map[scancode];
        }
        if (!shift_down && it_accent_map[scancode]) {
            return (char)it_accent_map[scancode];
        }
        return shifted_with_caps(it_normal_map[scancode], it_shift_map[scancode]);
    }

    char c = shifted_with_caps(normal_map[scancode], shift_map[scancode]);
    return c ? remap_layout(c) : 0;
}

static void process_extended_scancode(uint8_t scancode)
{
    bool release = (scancode & 0x80) != 0;
    uint8_t code = scancode & 0x7f;

    if (code >= 128) {
        return;
    }

    if (release) {
        ext_key_down[code] = false;

        if (code == 0x1d) {
            ctrl_down = 0;
        } else if (code == 0x38) {
            alt_down = 0;
            altgr_down = 0;
        }

        /* Push release event for special keys */
        switch (code) {
        case 0x47: push_event((uint16_t)(KEY_HOME  | 0x8000)); break;
        case 0x48: push_event((uint16_t)(KEY_UP    | 0x8000)); break;
        case 0x4b: push_event((uint16_t)(KEY_LEFT  | 0x8000)); break;
        case 0x4d: push_event((uint16_t)(KEY_RIGHT | 0x8000)); break;
        case 0x4f: push_event((uint16_t)(KEY_END   | 0x8000)); break;
        case 0x50: push_event((uint16_t)(KEY_DOWN  | 0x8000)); break;
        case 0x53: push_event((uint16_t)(KEY_DELETE| 0x8000)); break;
        default: break;
        }

        return;
    }

    if (ext_key_down[code]) {
        return;
    }

    ext_key_down[code] = true;

    switch (code) {
    case 0x1c:
        /* Numpad Enter in translated set 1. */
        push_key('\n');
        break;
    case 0x1d:
        /* Right Ctrl.  Track it so E0 1D is not mistaken for another key. */
        ctrl_down = 1;
        break;
    case 0x38:
        /* Right Alt / AltGr. */
        alt_down = 1;
        altgr_down = 1;
        break;
    case 0x47:
        push_key(KEY_HOME);
        break;
    case 0x48:
        push_key(KEY_UP);
        break;
    case 0x4b:
        push_key(shift_down ? KEY_SHIFT_LEFT : KEY_LEFT);
        break;
    case 0x4d:
        push_key(shift_down ? KEY_SHIFT_RIGHT : KEY_RIGHT);
        break;
    case 0x4f:
        push_key(KEY_END);
        break;
    case 0x50:
        push_key(KEY_DOWN);
        break;
    case 0x53:
        push_key(KEY_DELETE);
        break;
    default:
        break;
    }
}

static void process_set1_scancode(uint8_t scancode)
{
    bool release = (scancode & 0x80) != 0;
    uint8_t code = scancode & 0x7f;

    if (code >= 128) {
        return;
    }

    if (release) {
        key_down[code] = false;

        if (code == 0x2a) {
            left_shift_down = 0;
            shift_down = left_shift_down || right_shift_down;
        } else if (code == 0x36) {
            right_shift_down = 0;
            shift_down = left_shift_down || right_shift_down;
        } else if (code == 0x1d) {
            ctrl_down = 0;
            push_event((uint16_t)(0x114 | 0x8000));  /* Release CTRL */
        } else if (code == 0x38) {
            alt_down = 0;
            altgr_down = 0;
            push_event((uint16_t)(0x115 | 0x8000));  /* Release ALT */
        } else if (code == 0x39) {
            push_event((uint16_t)(0x113 | 0x8000));  /* Release SPACE */
        }

        /* Push release event for regular keys */
        {
            char c = layout_char(code);
            if (c) push_event((uint16_t)((unsigned char)c | 0x8000));
        }

        return;
    }

    if (key_down[code]) {
        if (code != 0x0e) {
            return;
        }
        uint64_t now = pit_ticks();
        if (now - backspace_last_make_tick < 5) {
            return;
        }
        backspace_last_make_tick = now;
    }

    key_down[code] = true;
    if (code == 0x0e) {
        backspace_last_make_tick = pit_ticks();
    }

    if (code == 0x2a) {
        left_shift_down = 1;
        shift_down = 1;
        return;
    }

    if (code == 0x36) {
        right_shift_down = 1;
        shift_down = 1;
        return;
    }

    if (code == 0x1d) {
        ctrl_down = 1;
        push_key(KEY_CTRL);  /* 0x114 - for Doom fire */
        return;
    }

    if (code == 0x38) {
        alt_down = 1;
        push_key(KEY_ALT);   /* 0x115 - for Doom use */
        return;
    }

    /* Handle SPACE (0x39) - send both normal char and special code for Doom */
    if (code == 0x39) {
        push_key(' ');  /* Normal space character for applications */
        push_event(0x113); /* Also send special code for Doom */
        return;
    }

    if (ctrl_down && alt_down) {
        if (code == 0x3b) { push_key(KEY_TTY1); return; }
        if (code == 0x3c) { push_key(KEY_TTY2); return; }
        if (code == 0x3d) { push_key(KEY_TTY3); return; }
        if (code == 0x3e) { push_key(KEY_TTY4); return; }
        if (code == 0x3f) { push_key(KEY_TTY5); return; }
        if (code == 0x40) { push_key(KEY_TTY6); return; }
    }

    if (ctrl_down && shift_down && code == 0x2e) {
        push_key(KEY_COPY);
        return;
    }

    if (ctrl_down && shift_down && code == 0x2f) {
        push_key(KEY_PASTE);
        return;
    }

    if (ctrl_down && code == 0x2e) {
        interrupt_generation++;
        push_key(KEY_CTRL_C);
        return;
    }

    if (ctrl_down) {
        char c = layout_char(code);
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c >= 'a' && c <= 'z') {
            push_key((int)(c - 'a' + 1));
            return;
        }
    }

    if (code == 0x3a) {
        caps_lock = !caps_lock;

        /*
         * Do not send LED commands from inside the IRQ/poll path.
         * That can corrupt simple PS/2 input streams.
         */
        return;
    }

    char c = layout_char(code);
    if (c) {
        push_key(c);
    }
}

static void process_scancode(uint8_t scancode)
{
    if (scancode == PS2_ACK || scancode == PS2_RESEND || scancode == PS2_BAT_OK) {
        return;
    }

    if (scancode == 0xf0) {
        raw_set2_mode = 1;
        scancode_set = 2;
        set2_break_prefix = 1;
        return;
    }

    if (scancode == 0xe0) {
        extended_prefix = 1;
        return;
    }

    /*
     * Some bare-metal i8042 controllers report a translated config byte but
     * still deliver raw set-2 make codes until the first break sequence is
     * observed.  Backspace is especially visible on laptops: set 1 uses 0x0e,
     * set 2 uses 0x66.  0x66 is not a normal set-1 key we use, so mapping it
     * here is safe and keeps QEMU and real hardware behavior identical.
     */
    if (!raw_set2_mode && scancode == 0x66) {
        process_set1_scancode(0x0e);
        return;
    }

    if (raw_set2_mode || scancode_set == 2 || set2_break_prefix) {
        bool release = set2_break_prefix != 0;
        bool extended = extended_prefix != 0;
        uint8_t mapped = 0;

        if (scancode < 128) {
            mapped = extended ? set2_ext_to_set1[scancode] : set2_to_set1[scancode];
        }

        set2_break_prefix = 0;
        extended_prefix = 0;

        if (mapped) {
            if (release) {
                mapped |= 0x80;
            }
            if (extended) {
                process_extended_scancode(mapped);
            } else {
                process_set1_scancode(mapped);
            }
        }
        return;
    }

    if (extended_prefix) {
        extended_prefix = 0;
        process_extended_scancode(scancode);
        return;
    }

    process_set1_scancode(scancode);
}

void keyboard_poll(void)
{
    poll_count++;
    for (size_t i = 0; i < 64; i++) {
        uint8_t status = inb(I8042_STATUS);

        if (!(status & I8042_STATUS_OUTPUT_FULL)) {
            return;
        }

        uint8_t scancode = inb(I8042_DATA);

        if (status & I8042_STATUS_AUX_DATA) {
            continue;
        }

        process_scancode(scancode);
    }
}

void keyboard_handle_irq(void)
{
    irq_count++;
    keyboard_poll();
}

void keyboard_inject_set1_scancode(uint8_t scancode)
{
    process_set1_scancode(scancode);
}

void keyboard_inject_extended_set1_scancode(uint8_t scancode)
{
    process_extended_scancode(scancode);
}

int keyboard_getchar(void)
{
    int c;

    while ((c = keyboard_try_getchar()) < 0) {
        cpu_pause();
    }

    return c;
}

/* Return whether the left or right Ctrl key is currently held down.
 * Used by the tty layer to translate Ctrl+letter combos into control characters.
 */
int keyboard_is_ctrl_down(void)
{
    return ctrl_down;
}

int keyboard_try_getchar(void)
{
    /* Do NOT poll here - let IRQ handler fill the buffer */
    if (read_pos == write_pos) {
        return -1;
    }

    int c = buffer[read_pos];
    read_pos = (read_pos + 1) % KBD_BUFFER;

    return c;
}

void keyboard_debug_stats(void)
{
    extern void log_info(const char *tag, const char *fmt, ...);
    size_t avail = (write_pos >= read_pos) ? (write_pos - read_pos) : (KBD_BUFFER - read_pos + write_pos);
    log_info("kbd", "IRQ count: %llu, poll count: %llu, buffer avail: %lu", 
             (unsigned long long)irq_count, (unsigned long long)poll_count, (unsigned long)avail);
}

bool keyboard_input_available(void)
{
    keyboard_poll();
    return read_pos != write_pos;
}

int keyboard_set_layout(const char *name)
{
    const char *available[] = { "us", "uk", "de", "fr", "it", "es", "dvorak" };

    for (size_t i = 0; i < sizeof(available) / sizeof(available[0]); i++) {
        if (strcmp(name, available[i]) == 0) {
            layout = available[i];
            return 0;
        }
    }

    return -1;
}

const char *keyboard_current_layout(void)
{
    return layout;
}

const char *keyboard_available_layouts(void)
{
    return "us uk de fr it es dvorak";
}

bool keyboard_consume_interrupt(void)
{
    keyboard_poll();
    if (interrupt_consumed_generation == interrupt_generation) {
        return false;
    }
    interrupt_consumed_generation = interrupt_generation;
    if (read_pos != write_pos && buffer[read_pos] == KEY_CTRL_C) {
        read_pos = (read_pos + 1) % KBD_BUFFER;
    }
    return true;
}

void keyboard_ack_interrupt(void)
{
    interrupt_consumed_generation = interrupt_generation;
}
