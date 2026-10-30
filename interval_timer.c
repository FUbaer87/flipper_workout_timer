/*
 * Workout Timer for the Flipper Zero
 *
 *  - Stopwatch (with lap times)
 *  - Countdown (minutes and seconds, set one field at a time)
 *  - Interval timer, Gymboss style: Work / Rest times, rounds (0 rounds = endless)
 *  - Alert: vibration, beep or both; beep volume: off, quiet or loud
 *  - LED: short colored flash per phase, red flash in the last three seconds
 *
 * Settings are stored on the SD card (apps_data/interval_timer).
 */

#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dolphin_bitmap.h"

#define TAG "WorkoutTimer"

#define SETTINGS_PATH  APP_DATA_PATH("settings.bin")
#define SETTINGS_MAGIC 0x49545634UL

#define PREP_MIN   0
#define PREP_MAX   60
#define WORK_MIN   2
#define WORK_MAX   5940 /* 99 minutes */
#define REST_MIN   0
#define REST_MAX   5940
#define ROUNDS_MIN 0 /* 0 = endless */
#define ROUNDS_MAX 99
#define TIMER_MIN  1
#define TIMER_MAX  5999 /* 99:59 */

/* ---------- Types ---------- */

typedef enum {
    AlarmVibro,
    AlarmSound,
    AlarmBoth,
    AlarmCount,
} AlarmMode;

static const char* alarm_names[AlarmCount] = {"Vibration", "Beep", "Both"};

typedef enum {
    VolumeOff,
    VolumeQuiet,
    VolumeLoud,
    VolumeCount,
} VolumeLevel;

static const char* volume_names[VolumeCount] = {"Off", "Quiet", "Loud"};
static const float volume_values[VolumeCount] = {0.0f, 0.1f, 1.0f};

typedef enum {
    MenuStopwatch,
    MenuCountdown,
    MenuInterval,
    MenuAlert,
    MenuVolume,
    MenuCount,
} MenuItem;

typedef enum {
    FieldPrep,
    FieldWork,
    FieldRest,
    FieldRounds,
    FieldCount,
} Field;

typedef enum {
    ScreenMenu,
    ScreenStopwatch,
    ScreenCountdownSetup,
    ScreenIntervalSetup,
    ScreenRun,
} Screen;

typedef enum {
    PhasePrep,
    PhaseWork,
    PhaseRest,
    PhaseDone,
} Phase;

typedef enum {
    SigPrep,
    SigWork,
    SigRest,
    SigTick,
    SigDone,
    SigTest,
    SigCount,
} Signal;

/* Signal variants: 0 = vibration, 1 = beep, 2 = both, 3 = LED only */
#define VARIANT_COUNT 4

typedef enum {
    EventTypeInput,
    EventTypeTick,
} EventType;

typedef struct {
    EventType type;
    InputEvent input;
} AppEvent;

typedef struct {
    uint32_t magic;
    uint16_t prep;
    uint16_t work;
    uint16_t rest;
    uint16_t rounds;
    uint16_t timer; /* countdown, total seconds */
    uint16_t alarm;
    uint16_t volume;
} Settings;

typedef struct {
    uint16_t prep;
    uint16_t work;
    uint16_t rest;
    uint16_t rounds; /* 0 = endless */
} RunParams;

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* queue;
    FuriMutex* mutex;
    FuriTimer* timer;
    NotificationApp* notifications;

    Settings settings;
    Screen screen;
    uint8_t menu_index;
    Field field;
    uint8_t cd_field; /* countdown setup: 0 = minutes, 1 = seconds */
    uint16_t repeat_count; /* counts repeats while a key is held (acceleration) */

    /* Run (countdown and interval) */
    RunParams run;
    bool is_timer;
    Phase phase;
    uint16_t remaining;
    uint16_t phase_total;
    uint16_t round;
    bool paused;

    /* Stopwatch */
    bool sw_running;
    uint32_t sw_start_tick;
    uint32_t sw_accum_ms;
    uint32_t sw_lap_total_ms;
    uint32_t sw_lap_last_ms;
    uint32_t sw_lap_prev_ms;
    uint16_t sw_lap_count;

    bool exit;
} App;

/* ---------- Signals ---------- */

/* Beep notes with a changeable volume (set via apply_volume) */
static NotificationMessage msg_c5 = {
    .type = NotificationMessageTypeSoundOn,
    .data.sound = {.frequency = 523.25f, .volume = 1.0f}};
static NotificationMessage msg_c6 = {
    .type = NotificationMessageTypeSoundOn,
    .data.sound = {.frequency = 1046.50f, .volume = 1.0f}};
static NotificationMessage msg_e6 = {
    .type = NotificationMessageTypeSoundOn,
    .data.sound = {.frequency = 1318.51f, .volume = 1.0f}};
static NotificationMessage msg_g6 = {
    .type = NotificationMessageTypeSoundOn,
    .data.sound = {.frequency = 1567.98f, .volume = 1.0f}};

#define LED_OFF    &message_red_0, &message_green_0, &message_blue_0
#define LED_BLUE   &message_red_0, &message_green_0, &message_blue_255
#define LED_GREEN  &message_red_0, &message_green_255, &message_blue_0
#define LED_YELLOW &message_red_255, &message_green_255, &message_blue_0
#define LED_RED    &message_red_255, &message_green_0, &message_blue_0
#define LED_CYAN   &message_red_0, &message_green_255, &message_blue_255

#define VIB_ON  &message_vibro_on
#define VIB_OFF &message_vibro_off
#define SND_OFF &message_sound_off
#define D100    &message_delay_100
#define D250    &message_delay_250
#define D500    &message_delay_500

/* Prep: short blue flash */
static const NotificationSequence seq_prep_v = {LED_BLUE, VIB_ON, D100, VIB_OFF, LED_OFF, NULL};
static const NotificationSequence seq_prep_s = {LED_BLUE, &msg_c5, D100, SND_OFF, LED_OFF, NULL};
static const NotificationSequence seq_prep_b = {
    LED_BLUE, VIB_ON, &msg_c5, D100, VIB_OFF, SND_OFF, LED_OFF, NULL};
static const NotificationSequence seq_prep_n = {LED_BLUE, D100, LED_OFF, NULL};

/* Work: long green flash */
static const NotificationSequence seq_work_v = {LED_GREEN, VIB_ON, D250, VIB_OFF, LED_OFF, NULL};
static const NotificationSequence seq_work_s = {LED_GREEN, &msg_g6, D250, SND_OFF, LED_OFF, NULL};
static const NotificationSequence seq_work_b = {
    LED_GREEN, VIB_ON, &msg_g6, D250, VIB_OFF, SND_OFF, LED_OFF, NULL};
static const NotificationSequence seq_work_n = {LED_GREEN, D250, LED_OFF, NULL};

/* Rest: double pulse, yellow flash */
static const NotificationSequence seq_rest_v = {
    LED_YELLOW, VIB_ON, D100, VIB_OFF, D100, VIB_ON, D100, VIB_OFF, LED_OFF, NULL};
static const NotificationSequence seq_rest_s = {
    LED_YELLOW, &msg_c5, D100, SND_OFF, D100, &msg_c5, D100, SND_OFF, LED_OFF, NULL};
static const NotificationSequence seq_rest_b = {
    LED_YELLOW, VIB_ON, &msg_c5, D100, VIB_OFF, SND_OFF, D100,
    VIB_ON, &msg_c5, D100, VIB_OFF, SND_OFF, LED_OFF, NULL};
static const NotificationSequence seq_rest_n = {LED_YELLOW, D250, LED_OFF, NULL};

/* Last three seconds: short red flash */
static const NotificationSequence seq_tick_v = {LED_RED, VIB_ON, D100, VIB_OFF, LED_OFF, NULL};
static const NotificationSequence seq_tick_s = {LED_RED, &msg_c6, D100, SND_OFF, LED_OFF, NULL};
static const NotificationSequence seq_tick_b = {
    LED_RED, VIB_ON, &msg_c6, D100, VIB_OFF, SND_OFF, LED_OFF, NULL};
static const NotificationSequence seq_tick_n = {LED_RED, D100, LED_OFF, NULL};

/* Done */
static const NotificationSequence seq_done_v = {
    LED_CYAN, VIB_ON, D250, VIB_OFF, D100, VIB_ON, D250, VIB_OFF, D100, VIB_ON, D500, VIB_OFF,
    LED_OFF, NULL};
static const NotificationSequence seq_done_s = {
    LED_CYAN, &msg_c6, D250, &msg_e6, D250, &msg_g6, D500, SND_OFF, LED_OFF, NULL};
static const NotificationSequence seq_done_b = {
    LED_CYAN, VIB_ON, &msg_c6, D250, &msg_e6, D250, &msg_g6, D500, VIB_OFF, SND_OFF, LED_OFF,
    NULL};
static const NotificationSequence seq_done_n = {LED_CYAN, D500, LED_OFF, NULL};

/* Preview when changing the alert or the volume (no LED) */
static const NotificationSequence seq_test_v = {VIB_ON, D250, VIB_OFF, NULL};
static const NotificationSequence seq_test_s = {&msg_c6, D250, SND_OFF, NULL};
static const NotificationSequence seq_test_b = {VIB_ON, &msg_c6, D250, VIB_OFF, SND_OFF, NULL};
static const NotificationSequence seq_test_n = {SND_OFF, NULL};

static const NotificationSequence seq_off = {LED_OFF, VIB_OFF, SND_OFF, NULL};

static const NotificationSequence* const signals[SigCount][VARIANT_COUNT] = {
    [SigPrep] = {&seq_prep_v, &seq_prep_s, &seq_prep_b, &seq_prep_n},
    [SigWork] = {&seq_work_v, &seq_work_s, &seq_work_b, &seq_work_n},
    [SigRest] = {&seq_rest_v, &seq_rest_s, &seq_rest_b, &seq_rest_n},
    [SigTick] = {&seq_tick_v, &seq_tick_s, &seq_tick_b, &seq_tick_n},
    [SigDone] = {&seq_done_v, &seq_done_s, &seq_done_b, &seq_done_n},
    [SigTest] = {&seq_test_v, &seq_test_s, &seq_test_b, &seq_test_n},
};

static void apply_volume(const App* app) {
    uint16_t level = app->settings.volume < VolumeCount ? app->settings.volume : VolumeLoud;
    float v = volume_values[level];
    msg_c5.data.sound.volume = v;
    msg_c6.data.sound.volume = v;
    msg_e6.data.sound.volume = v;
    msg_g6.data.sound.volume = v;
}

static uint8_t signal_variant(const App* app) {
    uint16_t mode = app->settings.alarm < AlarmCount ? app->settings.alarm : AlarmBoth;
    bool vibro = (mode == AlarmVibro || mode == AlarmBoth);
    bool sound = (mode == AlarmSound || mode == AlarmBoth) && app->settings.volume != VolumeOff;
    if(vibro && sound) return 2;
    if(vibro) return 0;
    if(sound) return 1;
    return 3;
}

static void signal_play(App* app, Signal sig) {
    notification_message(app->notifications, signals[sig][signal_variant(app)]);
}

static void keep_awake(App* app, bool enable) {
    notification_message(
        app->notifications,
        enable ? &sequence_display_backlight_enforce_on :
                 &sequence_display_backlight_enforce_auto);
}

/* ---------- Settings ---------- */

static void settings_defaults(Settings* s) {
    s->magic = SETTINGS_MAGIC;
    s->prep = 5;
    s->work = 60;
    s->rest = 30;
    s->rounds = 5;
    s->timer = 180;
    s->alarm = AlarmBoth;
    s->volume = VolumeLoud;
}

static bool settings_valid(const Settings* s) {
    return s->magic == SETTINGS_MAGIC && s->prep <= PREP_MAX && s->work >= WORK_MIN &&
           s->work <= WORK_MAX && s->rest <= REST_MAX && s->rounds <= ROUNDS_MAX &&
           s->timer >= TIMER_MIN && s->timer <= TIMER_MAX && s->alarm < AlarmCount &&
           s->volume < VolumeCount;
}

static void settings_load(Settings* s) {
    settings_defaults(s);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    Settings tmp;
    if(storage_file_open(file, SETTINGS_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        if(storage_file_read(file, &tmp, sizeof(tmp)) == sizeof(tmp) && settings_valid(&tmp)) {
            *s = tmp;
        }
        storage_file_close(file);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

static void settings_save(const Settings* s) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, SETTINGS_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_write(file, s, sizeof(*s));
        storage_file_close(file);
    } else {
        FURI_LOG_W(TAG, "Could not save settings");
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

/* ---------- Helpers ---------- */

/* Total duration; endless runs (0 rounds) return 0 */
static uint32_t total_seconds(const RunParams* r) {
    if(r->rounds == 0) return 0;
    uint32_t total = (uint32_t)r->prep + (uint32_t)r->rounds * r->work;
    if(r->rounds > 1) total += (uint32_t)(r->rounds - 1) * r->rest;
    return total;
}

/* mm:ss up to 99:59, then h:mm:ss */
static void format_time(char* buf, size_t size, uint32_t secs) {
    if(secs >= 6000) {
        snprintf(
            buf,
            size,
            "%lu:%02lu:%02lu",
            (unsigned long)(secs / 3600),
            (unsigned long)((secs / 60) % 60),
            (unsigned long)(secs % 60));
    } else {
        snprintf(buf, size, "%02lu:%02lu", (unsigned long)(secs / 60), (unsigned long)(secs % 60));
    }
}

static void format_stopwatch(char* buf, size_t size, uint32_t ms) {
    uint32_t total_s = ms / 1000;
    uint32_t tenth = (ms % 1000) / 100;
    uint32_t h = total_s / 3600;
    uint32_t m = (total_s / 60) % 60;
    uint32_t s = total_s % 60;
    if(h > 0) {
        snprintf(buf, size, "%lu:%02lu:%02lu", (unsigned long)h, (unsigned long)m, (unsigned long)s);
    } else {
        snprintf(
            buf,
            size,
            "%02lu:%02lu.%lu",
            (unsigned long)m,
            (unsigned long)s,
            (unsigned long)tenth);
    }
}

static uint16_t* field_value(App* app, Field field, int32_t* min, int32_t* max) {
    switch(field) {
    case FieldPrep:
        *min = PREP_MIN;
        *max = PREP_MAX;
        return &app->settings.prep;
    case FieldWork:
        *min = WORK_MIN;
        *max = WORK_MAX;
        return &app->settings.work;
    case FieldRest:
        *min = REST_MIN;
        *max = REST_MAX;
        return &app->settings.rest;
    default:
        *min = ROUNDS_MIN;
        *max = ROUNDS_MAX;
        return &app->settings.rounds;
    }
}

/* Step size: a short press changes by 1, holding the key accelerates */
static int32_t step_for(const App* app, bool repeat) {
    if(!repeat) return 1;
    if(app->field == FieldRounds) return app->repeat_count < 10 ? 1 : 5;
    if(app->repeat_count < 10) return 5;
    if(app->repeat_count < 30) return 15;
    return 60;
}

static void adjust_field(App* app, int32_t direction, bool repeat) {
    int32_t min, max;
    uint16_t* value = field_value(app, app->field, &min, &max);

    int32_t v = (int32_t)*value + direction * step_for(app, repeat);
    if(v < min) v = min;
    if(v > max) v = max;
    *value = (uint16_t)v;
}

/* Countdown: Up/Down change the active field (minutes or seconds) */
static void adjust_countdown(App* app, int32_t direction, bool repeat) {
    int32_t step = 1;
    if(repeat) {
        if(app->repeat_count >= 30) {
            step = 10;
        } else if(app->repeat_count >= 10) {
            step = 5;
        }
    }
    int32_t unit = (app->cd_field == 0) ? 60 : 1;
    int32_t v = (int32_t)app->settings.timer + direction * step * unit;
    if(v < TIMER_MIN) v = TIMER_MIN;
    if(v > TIMER_MAX) v = TIMER_MAX;
    app->settings.timer = (uint16_t)v;
}

static void change_alert(App* app, int32_t direction) {
    app->settings.alarm = (app->settings.alarm + AlarmCount + direction) % AlarmCount;
    signal_play(app, SigTest);
}

static void change_volume(App* app, int32_t direction) {
    app->settings.volume = (app->settings.volume + VolumeCount + direction) % VolumeCount;
    apply_volume(app);
    signal_play(app, SigTest);
}

static uint32_t stopwatch_elapsed(const App* app) {
    uint32_t ms = app->sw_accum_ms;
    if(app->sw_running) ms += furi_get_tick() - app->sw_start_tick;
    return ms;
}

/* ---------- Run control (countdown / interval) ---------- */

static void enter_phase(App* app, Phase phase) {
    app->phase = phase;
    app->paused = false;

    switch(phase) {
    case PhasePrep:
        app->remaining = app->run.prep;
        signal_play(app, SigPrep);
        break;
    case PhaseWork:
        app->remaining = app->run.work;
        if(!app->is_timer) signal_play(app, SigWork);
        break;
    case PhaseRest:
        app->remaining = app->run.rest;
        signal_play(app, SigRest);
        break;
    case PhaseDone:
        app->remaining = 0;
        furi_timer_stop(app->timer);
        signal_play(app, SigDone);
        break;
    }
    app->phase_total = app->remaining;
}

static void next_round(App* app) {
    if(app->round < UINT16_MAX) app->round++;
    enter_phase(app, PhaseWork);
}

static void advance_phase(App* app) {
    switch(app->phase) {
    case PhasePrep:
        app->round = 1;
        enter_phase(app, PhaseWork);
        break;
    case PhaseWork:
        if(app->run.rounds != 0 && app->round >= app->run.rounds) {
            enter_phase(app, PhaseDone);
        } else if(app->run.rest > 0) {
            enter_phase(app, PhaseRest);
        } else {
            next_round(app);
        }
        break;
    case PhaseRest:
        next_round(app);
        break;
    case PhaseDone:
        break;
    }
}

static void start_run(App* app, bool is_timer) {
    settings_save(&app->settings);

    app->is_timer = is_timer;
    if(is_timer) {
        app->run = (RunParams){.prep = 0, .work = app->settings.timer, .rest = 0, .rounds = 1};
    } else {
        app->run = (RunParams){
            .prep = app->settings.prep,
            .work = app->settings.work,
            .rest = app->settings.rest,
            .rounds = app->settings.rounds};
    }

    app->screen = ScreenRun;
    app->round = 1;
    keep_awake(app, true);

    if(app->run.prep > 0) {
        enter_phase(app, PhasePrep);
    } else {
        enter_phase(app, PhaseWork);
    }

    if(app->phase != PhaseDone) {
        furi_timer_start(app->timer, furi_kernel_get_tick_frequency());
    }
}

static void leave_to(App* app, Screen screen) {
    furi_timer_stop(app->timer);
    app->screen = screen;
    app->paused = false;
    app->sw_running = false;
    notification_message(app->notifications, &seq_off);
    keep_awake(app, false);
    settings_save(&app->settings);
}

static void handle_tick(App* app) {
    if(app->screen != ScreenRun || app->paused || app->phase == PhaseDone) return;

    if(app->remaining > 0) app->remaining--;

    if(app->remaining == 0) {
        advance_phase(app);
    } else if(app->remaining <= 3) {
        signal_play(app, SigTick);
    }
}

/* ---------- Stopwatch ---------- */

static void stopwatch_reset(App* app) {
    app->sw_running = false;
    app->sw_accum_ms = 0;
    app->sw_lap_total_ms = 0;
    app->sw_lap_last_ms = 0;
    app->sw_lap_prev_ms = 0;
    app->sw_lap_count = 0;
}

static void stopwatch_toggle(App* app) {
    if(app->sw_running) {
        app->sw_accum_ms = stopwatch_elapsed(app);
        app->sw_running = false;
        furi_timer_stop(app->timer);
        keep_awake(app, false);
    } else {
        app->sw_start_tick = furi_get_tick();
        app->sw_running = true;
        furi_timer_start(app->timer, furi_kernel_get_tick_frequency() / 10);
        keep_awake(app, true);
    }
}

static void stopwatch_lap(App* app) {
    if(!app->sw_running) return;
    uint32_t now = stopwatch_elapsed(app);
    app->sw_lap_prev_ms = app->sw_lap_last_ms;
    app->sw_lap_last_ms = now - app->sw_lap_total_ms;
    app->sw_lap_total_ms = now;
    if(app->sw_lap_count < 999) app->sw_lap_count++;
}

/* ---------- Input ---------- */

/* True for a short press or a held key (Long/Repeat); also maintains the repeat counter */
static bool is_action(App* app, const InputEvent* event, bool* repeat) {
    *repeat = false;
    if(event->type == InputTypeRelease) {
        app->repeat_count = 0;
        return false;
    }
    if(event->type == InputTypeShort) return true;
    if(event->type == InputTypeLong || event->type == InputTypeRepeat) {
        *repeat = true;
        if(app->repeat_count < UINT16_MAX) app->repeat_count++;
        return true;
    }
    return false;
}

static void handle_input_menu(App* app, const InputEvent* event) {
    bool repeat;
    if(!is_action(app, event, &repeat) || repeat) return;

    switch(event->key) {
    case InputKeyUp:
        app->menu_index = (app->menu_index + MenuCount - 1) % MenuCount;
        break;
    case InputKeyDown:
        app->menu_index = (app->menu_index + 1) % MenuCount;
        break;
    case InputKeyLeft:
        if(app->menu_index == MenuAlert) change_alert(app, -1);
        if(app->menu_index == MenuVolume) change_volume(app, -1);
        break;
    case InputKeyRight:
        if(app->menu_index == MenuAlert) change_alert(app, +1);
        if(app->menu_index == MenuVolume) change_volume(app, +1);
        break;
    case InputKeyOk:
        switch(app->menu_index) {
        case MenuStopwatch:
            stopwatch_reset(app);
            app->screen = ScreenStopwatch;
            break;
        case MenuCountdown:
            app->screen = ScreenCountdownSetup;
            break;
        case MenuInterval:
            app->screen = ScreenIntervalSetup;
            break;
        case MenuAlert:
            change_alert(app, +1);
            break;
        default:
            change_volume(app, +1);
            break;
        }
        break;
    case InputKeyBack:
        app->exit = true;
        break;
    default:
        break;
    }
}

static void handle_input_stopwatch(App* app, const InputEvent* event) {
    bool repeat;
    if(!is_action(app, event, &repeat) || repeat) return;

    switch(event->key) {
    case InputKeyOk:
        stopwatch_toggle(app);
        break;
    case InputKeyRight:
        stopwatch_lap(app);
        break;
    case InputKeyLeft:
        if(!app->sw_running) stopwatch_reset(app);
        break;
    case InputKeyBack:
        leave_to(app, ScreenMenu);
        break;
    default:
        break;
    }
}

static void handle_input_countdown_setup(App* app, const InputEvent* event) {
    bool repeat;
    if(!is_action(app, event, &repeat)) return;

    switch(event->key) {
    case InputKeyLeft:
        if(!repeat) app->cd_field = 0; /* minutes */
        break;
    case InputKeyRight:
        if(!repeat) app->cd_field = 1; /* seconds */
        break;
    case InputKeyUp:
        adjust_countdown(app, +1, repeat);
        break;
    case InputKeyDown:
        adjust_countdown(app, -1, repeat);
        break;
    case InputKeyOk:
        if(!repeat) start_run(app, true);
        break;
    case InputKeyBack:
        if(!repeat) leave_to(app, ScreenMenu);
        break;
    default:
        break;
    }
}

static void handle_input_interval_setup(App* app, const InputEvent* event) {
    bool repeat;
    if(!is_action(app, event, &repeat)) return;

    switch(event->key) {
    case InputKeyUp:
        if(!repeat) app->field = (app->field + FieldCount - 1) % FieldCount;
        break;
    case InputKeyDown:
        if(!repeat) app->field = (app->field + 1) % FieldCount;
        break;
    case InputKeyLeft:
        adjust_field(app, -1, repeat);
        break;
    case InputKeyRight:
        adjust_field(app, +1, repeat);
        break;
    case InputKeyOk:
        if(!repeat) start_run(app, false);
        break;
    case InputKeyBack:
        if(!repeat) leave_to(app, ScreenMenu);
        break;
    default:
        break;
    }
}

static void handle_input_run(App* app, const InputEvent* event) {
    bool repeat;
    if(!is_action(app, event, &repeat) || repeat) return;

    Screen back = app->is_timer ? ScreenCountdownSetup : ScreenIntervalSetup;

    switch(event->key) {
    case InputKeyOk:
        if(app->phase == PhaseDone) {
            leave_to(app, back);
        } else {
            app->paused = !app->paused;
        }
        break;
    case InputKeyRight: /* skip to the next phase */
        if(app->phase != PhaseDone && !app->is_timer) advance_phase(app);
        break;
    case InputKeyLeft: /* restart the current phase */
        if(app->phase != PhaseDone) app->remaining = app->phase_total;
        break;
    case InputKeyBack:
        leave_to(app, back);
        break;
    default:
        break;
    }
}

/* ---------- Drawing ---------- */

#define MENU_WIDTH 86

static void draw_menu(Canvas* canvas, App* app) {
    static const char* labels[MenuCount] = {
        "Stopwatch", "Countdown", "Interval Timer", "Alert", "Volume"};

    canvas_set_font(canvas, FontSecondary);
    for(int i = 0; i < MenuCount; i++) {
        int y = 2 + i * 12;
        bool selected = (i == (int)app->menu_index);

        if(selected) {
            canvas_draw_box(canvas, 0, y, MENU_WIDTH, 12);
            canvas_set_color(canvas, ColorWhite);
        }

        canvas_draw_str_aligned(canvas, 3, y + 10, AlignLeft, AlignBottom, labels[i]);
        if(i == MenuAlert) {
            uint16_t mode = app->settings.alarm < AlarmCount ? app->settings.alarm : AlarmBoth;
            canvas_draw_str_aligned(
                canvas, MENU_WIDTH - 3, y + 10, AlignRight, AlignBottom, alarm_names[mode]);
        } else if(i == MenuVolume) {
            uint16_t level = app->settings.volume < VolumeCount ? app->settings.volume : VolumeLoud;
            canvas_draw_str_aligned(
                canvas, MENU_WIDTH - 3, y + 10, AlignRight, AlignBottom, volume_names[level]);
        }
        if(selected) canvas_set_color(canvas, ColorBlack);
    }

    canvas_draw_xbm(canvas, 88, 12, DOLPHIN_WIDTH, DOLPHIN_HEIGHT, dolphin_bitmap);
}

static void draw_stopwatch(Canvas* canvas, App* app) {
    char buf[48];

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 0, AlignCenter, AlignTop, "STOPWATCH");

    format_stopwatch(buf, sizeof(buf), stopwatch_elapsed(app));
    canvas_set_font(canvas, FontBigNumbers);
    canvas_draw_str_aligned(canvas, 64, 27, AlignCenter, AlignCenter, buf);

    canvas_set_font(canvas, FontSecondary);
    if(app->sw_lap_count == 0) {
        canvas_draw_str_aligned(canvas, 64, 51, AlignCenter, AlignBottom, "OK start/stop   > lap");
        canvas_draw_str_aligned(canvas, 64, 62, AlignCenter, AlignBottom, "< reset (when stopped)");
    } else {
        char t[24];
        format_stopwatch(t, sizeof(t), app->sw_lap_last_ms);
        snprintf(buf, sizeof(buf), "L%u  %s", (unsigned)app->sw_lap_count, t);
        canvas_draw_str_aligned(canvas, 4, 51, AlignLeft, AlignBottom, buf);
        if(app->sw_lap_count > 1) {
            format_stopwatch(t, sizeof(t), app->sw_lap_prev_ms);
            snprintf(buf, sizeof(buf), "L%u  %s", (unsigned)(app->sw_lap_count - 1), t);
            canvas_draw_str_aligned(canvas, 4, 62, AlignLeft, AlignBottom, buf);
        }
    }
}

/* Two digits; the active field gets a dark box with white digits */
static void draw_countdown_field(Canvas* canvas, int center_x, uint32_t value, bool active) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%02lu", (unsigned long)value);

    if(active) {
        canvas_draw_box(canvas, center_x - 22, 15, 44, 24);
        canvas_set_color(canvas, ColorWhite);
    }
    canvas_set_font(canvas, FontBigNumbers);
    canvas_draw_str_aligned(canvas, center_x, 27, AlignCenter, AlignCenter, buf);
    canvas_set_color(canvas, ColorBlack);
}

static void draw_countdown_setup(Canvas* canvas, App* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 0, AlignCenter, AlignTop, "Countdown");

    draw_countdown_field(canvas, 36, app->settings.timer / 60, app->cd_field == 0);
    canvas_set_font(canvas, FontBigNumbers);
    canvas_draw_str_aligned(canvas, 64, 26, AlignCenter, AlignCenter, ":");
    draw_countdown_field(canvas, 92, app->settings.timer % 60, app->cd_field == 1);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 52, AlignCenter, AlignBottom, "< > min/sec   ^ v change");
    canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "OK = Start");
}

static void draw_interval_setup(Canvas* canvas, App* app) {
    static const char* labels[FieldCount] = {"Prep", "Work", "Rest", "Rounds"};
    char buf[24];

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 0, AlignCenter, AlignTop, "Interval Timer");

    canvas_set_font(canvas, FontSecondary);
    for(int i = 0; i < FieldCount; i++) {
        int y = 13 + i * 10;
        bool selected = (i == (int)app->field);

        int32_t min, max;
        uint16_t value = *field_value(app, (Field)i, &min, &max);
        if(i == FieldRounds) {
            if(value == 0) {
                snprintf(buf, sizeof(buf), "Endless");
            } else {
                snprintf(buf, sizeof(buf), "%u", (unsigned)value);
            }
        } else {
            format_time(buf, sizeof(buf), value);
        }

        if(selected) {
            canvas_draw_box(canvas, 0, y, 128, 10);
            canvas_set_color(canvas, ColorWhite);
        }

        canvas_draw_str_aligned(canvas, 4, y + 9, AlignLeft, AlignBottom, labels[i]);
        canvas_draw_str_aligned(canvas, 100, y + 9, AlignCenter, AlignBottom, buf);
        if(selected) {
            canvas_draw_str_aligned(canvas, 76, y + 9, AlignLeft, AlignBottom, "<");
            canvas_draw_str_aligned(canvas, 124, y + 9, AlignRight, AlignBottom, ">");
            canvas_set_color(canvas, ColorBlack);
        }
    }

    RunParams r = {
        .prep = app->settings.prep,
        .work = app->settings.work,
        .rest = app->settings.rest,
        .rounds = app->settings.rounds};
    canvas_draw_str_aligned(canvas, 2, 63, AlignLeft, AlignBottom, "OK=Start");
    char total[32];
    if(r.rounds == 0) {
        snprintf(total, sizeof(total), "Total endless");
    } else {
        format_time(buf, sizeof(buf), total_seconds(&r));
        snprintf(total, sizeof(total), "Total %s", buf);
    }
    canvas_draw_str_aligned(canvas, 126, 63, AlignRight, AlignBottom, total);
}

/* Cell at the top with label and time; the active one is inverted */
static void draw_cell(Canvas* canvas, int x, const char* label, const char* value, bool active) {
    if(active) {
        canvas_draw_box(canvas, x, 0, 63, 20);
        canvas_set_color(canvas, ColorWhite);
    } else {
        canvas_draw_frame(canvas, x, 0, 63, 20);
    }
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, x + 31, 2, AlignCenter, AlignTop, label);
    canvas_draw_str_aligned(canvas, x + 31, 11, AlignCenter, AlignTop, value);
    canvas_set_color(canvas, ColorBlack);
}

static void draw_done(Canvas* canvas, App* app) {
    char buf[48];

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 14, AlignCenter, AlignCenter, "DONE!");
    canvas_set_font(canvas, FontSecondary);
    if(app->is_timer) {
        canvas_draw_str_aligned(canvas, 64, 30, AlignCenter, AlignCenter, "Time's up");
        format_time(buf, sizeof(buf), app->run.work);
    } else {
        snprintf(buf, sizeof(buf), "%u rounds", (unsigned)app->run.rounds);
        canvas_draw_str_aligned(canvas, 64, 30, AlignCenter, AlignCenter, buf);
        format_time(buf, sizeof(buf), total_seconds(&app->run));
    }
    canvas_draw_str_aligned(canvas, 64, 42, AlignCenter, AlignCenter, buf);
    canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "OK = Back");
}

static void draw_progress(Canvas* canvas, App* app) {
    canvas_draw_frame(canvas, 4, 45, 120, 6);
    if(app->phase_total > 0) {
        uint32_t elapsed = app->phase_total - app->remaining;
        uint32_t width = (116UL * elapsed) / app->phase_total;
        if(width > 0) canvas_draw_box(canvas, 6, 47, width, 2);
    }
}

static void draw_run_countdown(Canvas* canvas, App* app) {
    char buf[24];

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 1, AlignCenter, AlignTop, "COUNTDOWN");

    format_time(buf, sizeof(buf), app->remaining);
    canvas_set_font(canvas, FontBigNumbers);
    canvas_draw_str_aligned(canvas, 64, 29, AlignCenter, AlignCenter, buf);

    draw_progress(canvas, app);

    if(app->paused) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 126, 63, AlignRight, AlignBottom, "PAUSED");
    }
}

/* Interval view, Gymboss style: Work, Rest and Round at a glance */
static void draw_run_interval(Canvas* canvas, App* app) {
    char buf[48];

    format_time(buf, sizeof(buf), app->run.work);
    draw_cell(canvas, 0, "WORK", buf, app->phase == PhaseWork);

    if(app->run.rest > 0) {
        format_time(buf, sizeof(buf), app->run.rest);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    draw_cell(canvas, 65, "REST", buf, app->phase == PhaseRest);

    /* remaining time of the current phase */
    format_time(buf, sizeof(buf), app->remaining);
    canvas_set_font(canvas, FontBigNumbers);
    canvas_draw_str_aligned(canvas, 64, 33, AlignCenter, AlignCenter, buf);

    draw_progress(canvas, app);

    /* round at the bottom left, status at the bottom right */
    canvas_set_font(canvas, FontPrimary);
    if(app->phase == PhasePrep) {
        snprintf(buf, sizeof(buf), "Get ready");
    } else if(app->run.rounds == 0) {
        snprintf(buf, sizeof(buf), "Round %u", (unsigned)app->round);
    } else {
        snprintf(buf, sizeof(buf), "Round %u/%u", (unsigned)app->round, (unsigned)app->run.rounds);
    }
    canvas_draw_str_aligned(canvas, 2, 63, AlignLeft, AlignBottom, buf);

    if(app->paused) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 126, 63, AlignRight, AlignBottom, "PAUSED");
    }
}

static void draw_run(Canvas* canvas, App* app) {
    if(app->phase == PhaseDone) {
        draw_done(canvas, app);
    } else if(app->is_timer) {
        draw_run_countdown(canvas, app);
    } else {
        draw_run_interval(canvas, app);
    }
}

static void draw_callback(Canvas* canvas, void* context) {
    App* app = context;
    if(furi_mutex_acquire(app->mutex, 200) != FuriStatusOk) return;

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    switch(app->screen) {
    case ScreenMenu:
        draw_menu(canvas, app);
        break;
    case ScreenStopwatch:
        draw_stopwatch(canvas, app);
        break;
    case ScreenCountdownSetup:
        draw_countdown_setup(canvas, app);
        break;
    case ScreenIntervalSetup:
        draw_interval_setup(canvas, app);
        break;
    case ScreenRun:
        draw_run(canvas, app);
        break;
    }

    furi_mutex_release(app->mutex);
}

/* ---------- Callbacks for input and timer ---------- */

static void input_callback(InputEvent* input_event, void* context) {
    App* app = context;
    AppEvent event = {.type = EventTypeInput, .input = *input_event};
    furi_message_queue_put(app->queue, &event, FuriWaitForever);
}

static void timer_callback(void* context) {
    App* app = context;
    AppEvent event = {.type = EventTypeTick};
    furi_message_queue_put(app->queue, &event, 0);
}

/* ---------- Entry point ---------- */

int32_t interval_timer_app(void* p) {
    UNUSED(p);

    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));

    settings_load(&app->settings);
    apply_volume(app);
    app->screen = ScreenMenu;
    app->field = FieldWork;
    app->cd_field = 0;
    app->phase = PhaseDone;

    app->queue = furi_message_queue_alloc(8, sizeof(AppEvent));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->timer = furi_timer_alloc(timer_callback, FuriTimerTypePeriodic, app);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, draw_callback, app);
    view_port_input_callback_set(app->view_port, input_callback, app);

    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    AppEvent event;
    while(!app->exit) {
        if(furi_message_queue_get(app->queue, &event, FuriWaitForever) != FuriStatusOk) continue;

        furi_mutex_acquire(app->mutex, FuriWaitForever);
        if(event.type == EventTypeTick) {
            handle_tick(app);
        } else if(event.type == EventTypeInput) {
            switch(app->screen) {
            case ScreenMenu:
                handle_input_menu(app, &event.input);
                break;
            case ScreenStopwatch:
                handle_input_stopwatch(app, &event.input);
                break;
            case ScreenCountdownSetup:
                handle_input_countdown_setup(app, &event.input);
                break;
            case ScreenIntervalSetup:
                handle_input_interval_setup(app, &event.input);
                break;
            case ScreenRun:
                handle_input_run(app, &event.input);
                break;
            }
        }
        furi_mutex_release(app->mutex);

        view_port_update(app->view_port);
    }

    /* Clean up */
    furi_timer_stop(app->timer);
    settings_save(&app->settings);
    notification_message(app->notifications, &seq_off);
    keep_awake(app, false);

    gui_remove_view_port(app->gui, app->view_port);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    view_port_free(app->view_port);
    furi_timer_free(app->timer);
    furi_mutex_free(app->mutex);
    furi_message_queue_free(app->queue);
    free(app);

    return 0;
}
