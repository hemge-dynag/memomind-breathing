#include "gm_plugin_lvgl_api.h"
#include "gm_plugin_libc.h"

/* Paired with PhoneSDK/examples/breathing. A guided breathing / cardiac
 * coherence pacer. The glasses show an animated bar that grows on the inhale
 * and shrinks on the exhale, plus the current phase and the session progress.
 * The phone picks the pattern (preset phases + session length), starts or
 * pauses the session, and keeps a per-day log of completed sessions. */

#define SETTINGS_CHANNEL UINT16_C(0x5245) /* 'RE': phone -> glasses */
#define EVENT_CHANNEL UINT16_C(0x5246)    /* 'RF': glasses -> phone */
#define PROTOCOL_VERSION 1U

#define COMMAND_CONFIG 1U
#define COMMAND_START 2U
#define COMMAND_PAUSE 3U
#define COMMAND_STOP 4U

#define EVENT_SESSION_DONE UINT8_C(1)

#define MIN_CONFIG_PAYLOAD 8U /* version + command + preset + 4 phases + minutes */

#define PHASE_INHALE 0U
#define PHASE_HOLD_IN 1U
#define PHASE_EXHALE 2U
#define PHASE_HOLD_OUT 3U
#define PHASE_COUNT 4U

#define MIN_PHASE_SEC 0
#define MAX_PHASE_SEC 60
#define MIN_DURATION_MIN 1
#define MAX_DURATION_MIN 30

#define REFRESH_INTERVAL_MS 100U

#define SCREEN_MARGIN 20
#define TITLE_HEIGHT 24
#define PHASE_HEIGHT 44
#define TRACK_HEIGHT 30
#define FILL_MIN_WIDTH 12
#define HINT_HEIGHT 30

typedef struct {
    const gm_plugin_host_api_t *host;
    const gm_plugin_lvgl_api_t *ui;
    const gm_plugin_libc_extension_api_t *libc;
    gm_plugin_lvgl_obj_t *screen;
    gm_plugin_lvgl_obj_t *title_label;
    gm_plugin_lvgl_obj_t *phase_label;
    gm_plugin_lvgl_obj_t *countdown_label;
    gm_plugin_lvgl_obj_t *pacer_track;
    gm_plugin_lvgl_obj_t *pacer_fill;
    gm_plugin_lvgl_obj_t *info_label;
    gm_plugin_lvgl_obj_t *hint_label;

    gm_plugin_lvgl_coord_t track_x;
    gm_plugin_lvgl_coord_t track_width;

    uint8_t preset;
    uint16_t phase_sec[PHASE_COUNT];
    uint32_t duration_ms;

    uint8_t phase;
    bool running;
    bool paused;
    bool done;

    uint32_t phase_elapsed_ms;
    uint32_t session_elapsed_ms;
    uint32_t refresh_elapsed_ms;
    uint16_t cycles;
    uint8_t language; /* 0 = French (default), 1 = English */
} breathing_t;

/* Picks the French or English string depending on the detected UI locale. */
#define L(fr_str, en_str) (br.language != 0U ? (en_str) : (fr_str))

static breathing_t br;

#define number gm_plugin_lvgl_style_number
#define color gm_plugin_lvgl_style_color

static int32_t clamp_i32(int32_t value, int32_t minimum, int32_t maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static void set_style(gm_plugin_lvgl_obj_t *object,
                       gm_plugin_lvgl_style_prop_t property,
                       gm_plugin_lvgl_style_value_t value)
{
    br.ui->style_set(object, property, value, GM_PLUGIN_LVGL_SELECTOR_MAIN);
}

static void style_panel(gm_plugin_lvgl_obj_t *object, uint8_t fill,
                         uint8_t border, uint8_t radius)
{
    set_style(object, GM_PLUGIN_LVGL_STYLE_BG_COLOR, color(fill));
    set_style(object, GM_PLUGIN_LVGL_STYLE_BG_OPA, number(255));
    set_style(object, GM_PLUGIN_LVGL_STYLE_BORDER_COLOR, color(0xA0));
    set_style(object, GM_PLUGIN_LVGL_STYLE_BORDER_OPA, number(255));
    set_style(object, GM_PLUGIN_LVGL_STYLE_BORDER_WIDTH, number(border));
    set_style(object, GM_PLUGIN_LVGL_STYLE_RADIUS, number(radius));
    br.ui->obj_clear_flag(object, GM_PLUGIN_LVGL_FLAG_SCROLLABLE);
}

static gm_plugin_lvgl_obj_t *make_label(gm_plugin_lvgl_obj_t *parent,
                                         const char *text, uint8_t shade)
{
    gm_plugin_lvgl_obj_t *label = br.ui->label_create(parent);
    if (label == 0) return 0;
    br.ui->label_set_text(label, text);
    br.ui->label_set_long_mode(label, GM_PLUGIN_LVGL_LABEL_DOT);
    set_style(label, GM_PLUGIN_LVGL_STYLE_TEXT_COLOR, color(shade));
    set_style(label, GM_PLUGIN_LVGL_STYLE_TEXT_ALIGN,
              number(GM_PLUGIN_LVGL_TEXT_ALIGN_CENTER));
    return label;
}

static uint8_t next_phase(uint8_t phase)
{
    return (uint8_t)((phase + 1U) % PHASE_COUNT);
}

/* True when the configured pattern has at least one non-zero phase. */
static bool pattern_valid(void)
{
    uint8_t i;
    for (i = 0U; i < PHASE_COUNT; i++) {
        if (br.phase_sec[i] > 0U) return true;
    }
    return false;
}

/* 0..1000 progress of the animated bar for the current phase. Inhale grows,
 * exhale shrinks, holds stay flat. */
static int32_t phase_progress(void)
{
    uint32_t duration = (uint32_t)br.phase_sec[br.phase] * 1000U;
    int32_t progress;

    if (br.phase == PHASE_HOLD_IN) return 1000;
    if (br.phase == PHASE_HOLD_OUT) return 0;
    if (duration == 0U) return 0;

    if (br.phase_elapsed_ms >= duration) {
        progress = 1000;
    } else {
        /* 32-bit safe: phase_elapsed_ms < duration <= 60000, so the scaled
         * product stays well within uint32_t. */
        progress = (int32_t)((br.phase_elapsed_ms * 1000U) / duration);
    }
    if (br.phase != PHASE_INHALE)
        progress = 1000 - progress;

    return clamp_i32(progress, 0, 1000);
}

static void update_pacer(void)
{
    int32_t progress = phase_progress();
    int32_t min_width = FILL_MIN_WIDTH;
    int32_t max_width = (int32_t)br.track_width;
    int32_t width = min_width + (max_width - min_width) * progress / 1000;

    if (width < min_width) width = min_width;
    if (width > max_width) width = max_width;

    br.ui->obj_set_size(br.pacer_fill, (gm_plugin_lvgl_coord_t)width,
                        TRACK_HEIGHT - 8);
    br.ui->obj_align(br.pacer_fill, GM_PLUGIN_LVGL_ALIGN_CENTER, 0, 0);
}

static void refresh_display(void)
{
    char text[48];
    uint32_t remaining_sec;
    uint32_t phase_left;
    uint32_t phase_duration = (uint32_t)br.phase_sec[br.phase] * 1000U;

    if (!br.running && !br.done) {
        br.ui->label_set_text(br.phase_label, L("Pret", "Ready"));
        set_style(br.phase_label, GM_PLUGIN_LVGL_STYLE_TEXT_COLOR, color(0xC0));
        br.ui->label_set_text(br.countdown_label, "");
        br.ui->label_set_text(br.info_label,
            L("Choisis un rythme sur le telephone",
              "Pick a pattern on the phone"));
        br.ui->label_set_text(br.hint_label,
            L("Bouton : demarrer", "Button: start"));
        update_pacer();
        return;
    }

    if (br.done) {
        br.ui->label_set_text(br.phase_label, L("Termine", "Done"));
        set_style(br.phase_label, GM_PLUGIN_LVGL_STYLE_TEXT_COLOR, color(0xF0));
        br.ui->label_set_text(br.countdown_label, "");
        br.libc->snprintf(text, sizeof(text), L("Cycles : %u", "Cycles: %u"),
                           (unsigned int)br.cycles);
        br.ui->label_set_text(br.info_label, text);
        br.ui->label_set_text(br.hint_label,
            L("Bouton : recommencer", "Button: restart"));
        update_pacer();
        return;
    }

    if (br.paused) {
        br.ui->label_set_text(br.phase_label, L("En pause", "Paused"));
        set_style(br.phase_label, GM_PLUGIN_LVGL_STYLE_TEXT_COLOR, color(0xD0));
    } else {
        switch (br.phase) {
        case PHASE_INHALE:
            br.ui->label_set_text(br.phase_label, L("Inspire", "Inhale"));
            break;
        case PHASE_EXHALE:
            br.ui->label_set_text(br.phase_label, L("Expire", "Exhale"));
            break;
        default:
            br.ui->label_set_text(br.phase_label, L("Retiens", "Hold"));
            break;
        }
        set_style(br.phase_label, GM_PLUGIN_LVGL_STYLE_TEXT_COLOR, color(0xF0));
    }

    if (phase_duration == 0U || br.phase_elapsed_ms >= phase_duration)
        phase_left = 0U;
    else
        phase_left = (phase_duration - br.phase_elapsed_ms + 999U) / 1000U;
    br.libc->snprintf(text, sizeof(text), "%u", (unsigned int)phase_left);
    br.ui->label_set_text(br.countdown_label, text);

    remaining_sec = br.session_elapsed_ms >= br.duration_ms
        ? 0U : (br.duration_ms - br.session_elapsed_ms + 999U) / 1000U;
    br.libc->snprintf(text, sizeof(text),
        L("Cycles : %u   Reste %u:%02u", "Cycles: %u   Left %u:%02u"),
        (unsigned int)br.cycles,
        (unsigned int)(remaining_sec / 60U), (unsigned int)(remaining_sec % 60U));
    br.ui->label_set_text(br.info_label, text);

    br.ui->label_set_text(br.hint_label,
        L("Bouton : pause / reprendre", "Button: pause / resume"));
    update_pacer();
}

static void send_done_event(void)
{
    uint8_t payload[7];
    uint32_t seconds = (br.duration_ms + 999U) / 1000U;
    payload[0] = PROTOCOL_VERSION;
    payload[1] = EVENT_SESSION_DONE;
    payload[2] = (uint8_t)(br.cycles & 0xFFU);
    payload[3] = (uint8_t)((br.cycles >> 8) & 0xFFU);
    payload[4] = (uint8_t)(seconds & 0xFFU);
    payload[5] = (uint8_t)((seconds >> 8) & 0xFFU);
    payload[6] = br.preset;
    (void)br.host->bt_send(EVENT_CHANNEL, payload, sizeof(payload));
}

static void reset_session(void)
{
    br.phase = PHASE_INHALE;
    br.phase_elapsed_ms = 0U;
    br.session_elapsed_ms = 0U;
    br.cycles = 0U;
    br.paused = false;
    br.done = false;
    br.running = false;
}

static void start_session(void)
{
    if (!pattern_valid()) return;
    reset_session();
    br.running = true;
    refresh_display();
}

static void finish_session(void)
{
    br.running = false;
    br.paused = false;
    br.done = true;
    br.phase = PHASE_INHALE;
    br.phase_elapsed_ms = 0U;
    br.session_elapsed_ms = br.duration_ms;
    send_done_event();
    refresh_display();
}

static void advance_phases(uint32_t elapsed_ms)
{
    uint8_t guard = 0U;

    br.phase_elapsed_ms += elapsed_ms;
    while (guard < 8U) {
        uint32_t duration = (uint32_t)br.phase_sec[br.phase] * 1000U;
        if (duration == 0U || br.phase_elapsed_ms < duration) break;
        br.phase_elapsed_ms -= duration;
        br.phase = next_phase(br.phase);
        if (br.phase == PHASE_INHALE && br.cycles < 0xFFFFU) br.cycles++;
        guard++;
    }
}

static void handle_settings_message(const uint8_t *data, uint32_t length)
{
    if (length < MIN_CONFIG_PAYLOAD || data[0] != PROTOCOL_VERSION) return;

    switch (data[1]) {
    case COMMAND_CONFIG: {
        uint32_t minutes;
        br.preset = data[2];
        br.phase_sec[PHASE_INHALE] = (uint16_t)clamp_i32((int32_t)data[3],
                                                         MIN_PHASE_SEC, MAX_PHASE_SEC);
        br.phase_sec[PHASE_HOLD_IN] = (uint16_t)clamp_i32((int32_t)data[4],
                                                          MIN_PHASE_SEC, MAX_PHASE_SEC);
        br.phase_sec[PHASE_EXHALE] = (uint16_t)clamp_i32((int32_t)data[5],
                                                         MIN_PHASE_SEC, MAX_PHASE_SEC);
        br.phase_sec[PHASE_HOLD_OUT] = (uint16_t)clamp_i32((int32_t)data[6],
                                                           MIN_PHASE_SEC, MAX_PHASE_SEC);
        minutes = (uint32_t)clamp_i32((int32_t)data[7],
                                      MIN_DURATION_MIN, MAX_DURATION_MIN);
        br.duration_ms = minutes * 60000U;
        reset_session();
        refresh_display();
        break;
    }
    case COMMAND_START:
        start_session();
        break;
    case COMMAND_PAUSE:
        if (br.running) {
            br.paused = !br.paused;
            refresh_display();
        }
        break;
    case COMMAND_STOP:
        reset_session();
        refresh_display();
        break;
    default:
        break;
    }
}

static gm_plugin_result_t breathing_start(void *context)
{
    gm_plugin_display_info_t display;
    gm_plugin_lvgl_obj_t *root;
    char locale[GM_PLUGIN_LOCALE_TAG_MAX];
    (void)context;

    br.language = 0U;
    if (br.host->locale_get != 0 &&
        br.host->locale_get(locale) == GM_PLUGIN_OK &&
        locale[0] == 'e' && locale[1] == 'n')
        br.language = 1U;

    if (br.host->display_get_info(&display) != GM_PLUGIN_OK ||
        display.width <= SCREEN_MARGIN * 2U ||
        display.height < TITLE_HEIGHT + PHASE_HEIGHT + TRACK_HEIGHT + HINT_HEIGHT + 70)
        return GM_PLUGIN_ESTATE;

    root = br.ui->root_get();
    if (root == 0) return GM_PLUGIN_ESTATE;
    br.ui->obj_clean(root);

    br.screen = br.ui->obj_create(root);
    if (br.screen == 0) return GM_PLUGIN_ENOMEM;
    br.ui->obj_set_size(br.screen, display.width, display.height);
    br.ui->obj_align(br.screen, GM_PLUGIN_LVGL_ALIGN_CENTER, 0, 0);
    style_panel(br.screen, 0x00, 0, 0);

    br.title_label = make_label(br.screen, "Respiration", 0x80);
    if (br.title_label == 0) goto no_memory;
    br.ui->obj_set_size(br.title_label, display.width - SCREEN_MARGIN * 2U, TITLE_HEIGHT);
    br.ui->obj_align(br.title_label, GM_PLUGIN_LVGL_ALIGN_TOP_MID, 0, 8);

    br.phase_label = make_label(br.screen, "", 0xF0);
    if (br.phase_label == 0) goto no_memory;
    {
        gm_plugin_lvgl_style_value_t large_font = {0};
        large_font.ptr = br.ui->font_large;
        set_style(br.phase_label, GM_PLUGIN_LVGL_STYLE_TEXT_FONT, large_font);
    }
    br.ui->obj_set_size(br.phase_label, display.width - SCREEN_MARGIN * 2U, PHASE_HEIGHT);
    br.ui->obj_align(br.phase_label, GM_PLUGIN_LVGL_ALIGN_CENTER, 0, -34);

    br.countdown_label = make_label(br.screen, "", 0xC0);
    if (br.countdown_label == 0) goto no_memory;
    br.ui->obj_set_size(br.countdown_label, display.width - SCREEN_MARGIN * 2U, 30);
    br.ui->obj_align(br.countdown_label, GM_PLUGIN_LVGL_ALIGN_CENTER, 0, 14);

    br.track_x = SCREEN_MARGIN;
    br.track_width = (gm_plugin_lvgl_coord_t)(display.width - SCREEN_MARGIN * 2U);

    br.pacer_track = br.ui->obj_create(br.screen);
    if (br.pacer_track == 0) goto no_memory;
    br.ui->obj_set_size(br.pacer_track, br.track_width, TRACK_HEIGHT);
    br.ui->obj_align(br.pacer_track, GM_PLUGIN_LVGL_ALIGN_CENTER, 0, 58);
    style_panel(br.pacer_track, 0x18, 1, TRACK_HEIGHT / 2);

    br.pacer_fill = br.ui->obj_create(br.pacer_track);
    if (br.pacer_fill == 0) goto no_memory;
    br.ui->obj_set_size(br.pacer_fill, FILL_MIN_WIDTH, TRACK_HEIGHT - 8);
    br.ui->obj_align(br.pacer_fill, GM_PLUGIN_LVGL_ALIGN_CENTER, 0, 0);
    style_panel(br.pacer_fill, 0xE0, 0, (TRACK_HEIGHT - 8) / 2);

    br.info_label = make_label(br.screen, "", 0x90);
    if (br.info_label == 0) goto no_memory;
    br.ui->obj_set_size(br.info_label, display.width - SCREEN_MARGIN * 2U, 24);
    br.ui->obj_align(br.info_label, GM_PLUGIN_LVGL_ALIGN_CENTER, 0, 88);

    br.hint_label = make_label(br.screen, "", 0x70);
    if (br.hint_label == 0) goto no_memory;
    br.ui->obj_set_size(br.hint_label, display.width - SCREEN_MARGIN * 2U, HINT_HEIGHT);
    br.ui->obj_align(br.hint_label, GM_PLUGIN_LVGL_ALIGN_BOTTOM_MID, 0, -8);

    br.preset = 0U;
    br.phase_sec[PHASE_INHALE] = 5U;
    br.phase_sec[PHASE_HOLD_IN] = 0U;
    br.phase_sec[PHASE_EXHALE] = 5U;
    br.phase_sec[PHASE_HOLD_OUT] = 0U;
    br.duration_ms = 5U * 60000U;
    br.refresh_elapsed_ms = REFRESH_INTERVAL_MS;
    reset_session();

    refresh_display();
    return GM_PLUGIN_OK;

no_memory:
    br.ui->obj_clean(root);
    br.screen = 0;
    br.title_label = 0;
    br.phase_label = 0;
    br.countdown_label = 0;
    br.pacer_track = 0;
    br.pacer_fill = 0;
    br.info_label = 0;
    br.hint_label = 0;
    return GM_PLUGIN_ENOMEM;
}

static void breathing_loop(void *context, uint32_t elapsed_ms)
{
    (void)context;

    /* Guard against long stalls (e.g. after a resume) so timers do not jump. */
    if (elapsed_ms > 500U) elapsed_ms = 500U;

    if (br.running && !br.paused) {
        br.session_elapsed_ms += elapsed_ms;
        if (br.session_elapsed_ms >= br.duration_ms) {
            finish_session();
        } else {
            advance_phases(elapsed_ms);
        }
    }

    if (br.refresh_elapsed_ms < REFRESH_INTERVAL_MS) {
        uint32_t remaining = REFRESH_INTERVAL_MS - br.refresh_elapsed_ms;
        br.refresh_elapsed_ms = elapsed_ms >= remaining
            ? REFRESH_INTERVAL_MS : br.refresh_elapsed_ms + elapsed_ms;
    }

    if (br.refresh_elapsed_ms >= REFRESH_INTERVAL_MS) {
        br.refresh_elapsed_ms = 0U;
        refresh_display();
    }
}

static bool breathing_event(void *context, const gm_plugin_event_t *event)
{
    (void)context;
    if (event == 0) return false;

    if (event->type == GM_PLUGIN_EVENT_BT_MESSAGE) {
        if (event->data.bt.channel != SETTINGS_CHANNEL || event->data.bt.data == 0)
            return false;
        handle_settings_message(event->data.bt.data, event->data.bt.length);
        return true;
    }

    if (event->type == GM_PLUGIN_EVENT_BUTTON &&
        event->data.button.button == GM_PLUGIN_BUTTON_PRIMARY) {
        if (event->data.button.action == GM_PLUGIN_BUTTON_ACTION_SINGLE) {
            if (!br.running)
                start_session();
            else
                br.paused = !br.paused;
            refresh_display();
            return true;
        }
        if (event->data.button.action == GM_PLUGIN_BUTTON_ACTION_LONG) {
            reset_session();
            refresh_display();
            return true;
        }
    }
    return false;
}

static void breathing_stop(void *context)
{
    gm_plugin_lvgl_obj_t *root;
    (void)context;
    root = br.ui->root_get();
    if (root != 0) br.ui->obj_clean(root);
    br.screen = 0;
    br.title_label = 0;
    br.phase_label = 0;
    br.countdown_label = 0;
    br.pacer_track = 0;
    br.pacer_fill = 0;
    br.info_label = 0;
    br.hint_label = 0;
    br.running = false;
    br.paused = false;
}

gm_plugin_result_t gm_plugin_entry(const gm_plugin_host_api_t *host,
                                    gm_plugin_descriptor_t *plugin)
{
    const gm_plugin_capabilities_t required =
        GM_PLUGIN_CAP_BLUETOOTH | GM_PLUGIN_CAP_BUTTON;

    if (host == 0 || plugin == 0 || host->log == 0 || host->bt_send == 0 ||
        host->display_get_info == 0 || host->graphics.lvgl == 0 ||
        !GM_PLUGIN_VERSION_COMPATIBLE(host->abi_version,
                                      GM_PLUGIN_ABI_MIN_VERSION) ||
        host->struct_size < GM_PLUGIN_HOST_API_MIN_SIZE ||
        plugin->struct_size < GM_PLUGIN_DESCRIPTOR_MIN_SIZE ||
        (host->capabilities & required) != required)
        return GM_PLUGIN_ENOTSUP;

    br.host = host;
    br.ui = host->graphics.lvgl;
    if (gm_plugin_libc_get(host, &br.libc) != GM_PLUGIN_OK)
        return GM_PLUGIN_ENOTSUP;
    if (br.ui->struct_size < GM_PLUGIN_LVGL_API_MIN_SIZE ||
        !GM_PLUGIN_VERSION_COMPATIBLE(br.ui->api_version,
                                      GM_PLUGIN_LVGL_API_MIN_VERSION))
        return GM_PLUGIN_EVERSION;

    plugin->abi_version = GM_PLUGIN_ABI_MIN_VERSION;
    plugin->context = &br;
    plugin->on_start = breathing_start;
    plugin->on_loop = breathing_loop;
    plugin->on_event = breathing_event;
    plugin->on_stop = breathing_stop;
    return GM_PLUGIN_OK;
}
