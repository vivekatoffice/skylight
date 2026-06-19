// Copyright (C) 2026 Axis Communications AB, Lund, Sweden
// Licensed under the MIT License. See LICENSE file for details.

#include <assert.h>
#include <axoverlay2.h>
#include <cairo/cairo.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <glib.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>
#include <vdo-error.h>
#include <vdo-stream.h>

#define MAX_AIRCRAFT 32
#define OVERLAY_WIDTH 580
#define OVERLAY_HEIGHT 400
#define REFRESH_INTERVAL_TICKS 300

#define LOCALDATA "/usr/local/packages/axoverlay2/localdata"
#define FETCH_OUTPUT LOCALDATA "/adsb_response.json"

struct aircraft {
    char hex[12];
    char flight[16];
    char reg[12];
    char type[8];
    char desc[48];
    int alt_baro;
    double gs;
    double track;
    double lat;
    double lon;
    double dst;
    double dir;
    int baro_rate;
    char squawk[8];
};

struct radar_data {
    struct aircraft ac[MAX_AIRCRAFT];
    int count;
    int total;
};

struct overlay {
    int overlay_id;
    unsigned stream_id;

    unsigned used_width;
    unsigned used_height;

    unsigned full_width;
    unsigned full_height;

    cairo_surface_t* surface;
};

static struct radar_data radar = { .count = 0 };

static bool radar_dirty = true;

static VdoStream* vdo_event_stream;
static GHashTable* overlay_table;

static unsigned tick_period_us = 1000000;

static GMainLoop* main_loop;

static void overlay_record_deleter(void* overlay_void);

static int signal_callback(void* userdata);

static int animation_tick_callback(void* userdata);

static int stream_event_callback(GIOChannel* channel,
                                 GIOCondition condition,
                                 void* userdata);

static void create_overlay(unsigned stream_id,
                           unsigned stream_width,
                           unsigned stream_height);

static void remove_overlay(unsigned stream_id);

static void process_next_frame(struct overlay* overlay);

static void render_frame(struct overlay* overlay,
                         char* target_buffer);

static void update_radar_data(void);

static void setup_fontconfig_cache(void);

static void draw_text_colored(cairo_t* cr,
                              double x,
                              double y,
                              const char* text,
                              double r,
                              double g,
                              double b,
                              double a);

static void draw_aircraft_icon(cairo_t* cr,
                               double cx,
                               double cy,
                               double heading,
                               double scale);

static const char* altitude_band(int alt);

static void climb_indicator(int baro_rate,
                            char* out,
                            size_t size);

/*
 * Find the matching ']' for an opening '[',
 * respecting nested arrays like "mlat": []
 * and "tisb": [] inside aircraft objects.
 */

static const char* find_array_end(const char* start,
                                  const char* json_end) {

    int depth = 1;

    const char* p = start;

    while (p < json_end && depth > 0) {

        if (*p == '[')
            depth++;
        else if (*p == ']')
            depth--;

        if (depth > 0)
            p++;
    }

    return (depth == 0) ? p : json_end;
}

int main(void) {

    GError* error = NULL;
    axo_err* axo_error = NULL;

    bool axo_running = false;

    VdoMap* stream_filter = NULL;

    GIOChannel* vdo_channel = NULL;
    unsigned vdo_watch_id = 0;

    openlog("axoverlay2",
            LOG_PID,
            LOG_USER);

    syslog(LOG_INFO, "Starting");

    setup_fontconfig_cache();

    if (!axo_start(NULL, &axo_error)) {

        syslog(LOG_ERR,
               "Axoverlay start failed: %s",
               axo_err_get_message(axo_error));

        axo_err_clear(&axo_error);

        return 1;
    }

    axo_running = true;

    overlay_table =
        g_hash_table_new_full(g_direct_hash,
                              g_direct_equal,
                              NULL,
                              overlay_record_deleter);

    main_loop = g_main_loop_new(NULL, FALSE);

    g_timeout_add(tick_period_us / 1000,
                  animation_tick_callback,
                  NULL);

    vdo_event_stream = vdo_stream_get(0, &error);

    if (!vdo_event_stream) {

        syslog(LOG_ERR,
               "VDO stream failed: %s",
               error ? error->message : "unknown");

        if (error)
            g_error_free(error);

        goto cleanup;
    }

    stream_filter = vdo_map_new();

    vdo_map_set_string(stream_filter,
                       "filter",
                       "overlay");

    if (!vdo_stream_attach(vdo_event_stream,
                           stream_filter,
                           &error)) {

        syslog(LOG_ERR,
               "VDO attach failed: %s",
               error ? error->message : "unknown");

        if (error)
            g_error_free(error);

        goto cleanup;
    }

    int stream_event_fd =
        vdo_stream_get_event_fd(vdo_event_stream,
                                &error);

    if (stream_event_fd < 0) {

        syslog(LOG_ERR,
               "Event fd failed: %s",
               error ? error->message : "unknown");

        if (error)
            g_error_free(error);

        goto cleanup;
    }

    vdo_channel =
        g_io_channel_unix_new(stream_event_fd);

    vdo_watch_id =
        g_io_add_watch(vdo_channel,
                       G_IO_IN |
                       G_IO_PRI |
                       G_IO_ERR |
                       G_IO_HUP,
                       stream_event_callback,
                       NULL);

    g_unix_signal_add(SIGINT,
                      signal_callback,
                      main_loop);

    g_unix_signal_add(SIGTERM,
                      signal_callback,
                      main_loop);

    update_radar_data();

    syslog(LOG_INFO, "Running");

    g_main_loop_run(main_loop);

cleanup:

    if (vdo_watch_id)
        g_source_remove(vdo_watch_id);

    if (vdo_channel)
        g_io_channel_unref(vdo_channel);

    if (axo_running)
        axo_stop(NULL);

    g_clear_object(&vdo_event_stream);
    g_clear_object(&stream_filter);

    if (main_loop)
        g_main_loop_unref(main_loop);

    if (overlay_table)
        g_hash_table_unref(overlay_table);

    unlink(FETCH_OUTPUT);

    syslog(LOG_INFO, "Stopped");

    closelog();

    return 0;
}

static void setup_fontconfig_cache(void) {

    mkdir("/tmp/.cache", 0755);
    mkdir("/tmp/.cache/fontconfig", 0755);

    setenv("HOME",
           "/tmp",
           1);

    setenv("XDG_CACHE_HOME",
           "/tmp/.cache",
           1);
}

static void overlay_record_deleter(void* overlay_void) {

    struct overlay* overlay = overlay_void;

    if (!overlay)
        return;

    if (overlay->surface)
        cairo_surface_destroy(overlay->surface);

    g_free(overlay);
}

static int signal_callback(void* userdata) {

    GMainLoop* loop = userdata;

    syslog(LOG_INFO, "Signal received");

    g_main_loop_quit(loop);

    return G_SOURCE_REMOVE;
}

static const char* find_key(const char* start,
                            const char* end,
                            const char* key) {

    size_t klen = strlen(key);

    const char* p = start;

    while (p && p < end) {

        p = strstr(p, key);

        if (!p || p >= end)
            return NULL;

        if (p > start) {

            char before = *(p - 1);

            if (before != '{' &&
                before != ',' &&
                before != ' ' &&
                before != '\t' &&
                before != '\n' &&
                before != '\r') {

                p += klen;

                continue;
            }
        }

        const char* after = p + klen;

        while (after < end &&
               (*after == ' ' || *after == '\t'))
            after++;

        if (after < end && *after == ':')
            return after + 1;

        p += klen;
    }

    return NULL;
}

static double json_get_double(const char* start,
                              const char* end,
                              const char* key,
                              double fallback) {

    const char* v =
        find_key(start, end, key);

    if (!v)
        return fallback;

    char* ep;

    double val = strtod(v, &ep);

    if (ep == v)
        return fallback;

    return val;
}

static int json_get_int(const char* start,
                        const char* end,
                        const char* key,
                        int fallback) {

    const char* v =
        find_key(start, end, key);

    if (!v)
        return fallback;

    char* ep;

    long val = strtol(v, &ep, 10);

    if (ep == v)
        return fallback;

    return (int)val;
}

static void json_get_string(const char* start,
                            const char* end,
                            const char* key,
                            char* out,
                            size_t out_size) {

    out[0] = '\0';

    const char* v =
        find_key(start, end, key);

    if (!v)
        return;

    while (v < end && (*v == ' ' || *v == '\t'))
        v++;

    if (v >= end || *v != '"')
        return;

    v++;

    size_t i = 0;

    while (v < end && *v != '"' && i < out_size - 1) {

        out[i++] = *v++;
    }

    out[i] = '\0';

    while (i > 0 && out[i - 1] == ' ')
        out[--i] = '\0';
}

static const char* next_ac_object(const char* p,
                                  const char* json_end,
                                  const char** obj_end) {

    int depth = 0;

    while (p < json_end) {

        if (*p == '{') {

            const char* start = p;

            depth = 1;
            p++;

            while (p < json_end && depth > 0) {

                if (*p == '{')
                    depth++;
                else if (*p == '}')
                    depth--;

                p++;
            }

            if (depth == 0) {

                *obj_end = p - 1;

                return start;
            }

            return NULL;
        }

        p++;
    }

    return NULL;
}

static void update_radar_data(void) {

    unlink(FETCH_OUTPUT);

    pid_t pid = fork();

    if (pid < 0) {

        syslog(LOG_WARNING,
               "Fetch: fork failed");

        return;
    }

    if (pid == 0) {

        int devnull =
            open("/dev/null", O_RDWR);

        if (devnull >= 0) {

            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);

            if (devnull > STDERR_FILENO)
                close(devnull);
        }

        execlp("curl",
               "curl",
               "-s",
               "-k",
               "--max-time", "10",
               "-o", FETCH_OUTPUT,
               "https://api.airplanes.live/v2/"
               "point/12.9716/77.5946/23",
               (char*)NULL);

        _exit(127);
    }

    int status = 0;

    waitpid(pid, &status, 0);

    if (WIFEXITED(status)) {

        int code = WEXITSTATUS(status);

        if (code != 0)
            syslog(LOG_WARNING,
                   "Fetch: curl exited %d",
                   code);

    } else {

        syslog(LOG_WARNING,
               "Fetch: curl killed by signal");
    }

    struct stat st;

    if (stat(FETCH_OUTPUT, &st) != 0) {

        syslog(LOG_WARNING,
               "Fetch: output file missing");

        return;
    }

    if (st.st_size < 10) {

        syslog(LOG_WARNING,
               "Fetch: file too small (%ld)",
               (long)st.st_size);

        unlink(FETCH_OUTPUT);

        return;
    }

    FILE* fp = fopen(FETCH_OUTPUT, "r");

    if (!fp) {

        syslog(LOG_WARNING,
               "Fetch: cannot open file");

        return;
    }

    char json[65536];

    size_t len =
        fread(json,
              1,
              sizeof(json) - 1,
              fp);

    fclose(fp);

    unlink(FETCH_OUTPUT);

    json[len] = '\0';

    if (len < 10) {

        syslog(LOG_WARNING,
               "Fetch: read too short (%zu)",
               len);

        return;
    }

    if (!strstr(json, "\"ac\"")) {

        char preview[201];

        size_t plen = len < 200 ? len : 200;

        memcpy(preview, json, plen);

        preview[plen] = '\0';

        syslog(LOG_WARNING,
               "Fetch: no 'ac'. Got: %s",
               preview);

        return;
    }

    const char* json_end = json + len;

    radar.total =
        json_get_int(json,
                     json_end,
                     "\"total\"",
                     0);

    const char* ac_start =
        strstr(json, "\"ac\"");

    ac_start = strchr(ac_start, '[');

    if (!ac_start) {

        radar.count = 0;
        radar_dirty = true;

        return;
    }

    ac_start++;

    /*
     * Find the matching ']' for the "ac" array.
     *
     * Cannot use simple strstr("]") because the
     * aircraft objects contain nested arrays
     * like "mlat": [] and "tisb": [] which
     * would be matched first.
     */

    const char* ac_end =
        find_array_end(ac_start, json_end);

    int count = 0;

    const char* cursor = ac_start;
    const char* obj_end = NULL;

    while (count < MAX_AIRCRAFT) {

        const char* obj =
            next_ac_object(cursor,
                           ac_end,
                           &obj_end);

        if (!obj)
            break;

        struct aircraft* a = &radar.ac[count];

        memset(a, 0, sizeof(*a));

        json_get_string(obj, obj_end,
                        "\"hex\"",
                        a->hex, sizeof(a->hex));

        json_get_string(obj, obj_end,
                        "\"flight\"",
                        a->flight, sizeof(a->flight));

        json_get_string(obj, obj_end,
                        "\"r\"",
                        a->reg, sizeof(a->reg));

        json_get_string(obj, obj_end,
                        "\"t\"",
                        a->type, sizeof(a->type));

        json_get_string(obj, obj_end,
                        "\"desc\"",
                        a->desc, sizeof(a->desc));

        json_get_string(obj, obj_end,
                        "\"squawk\"",
                        a->squawk, sizeof(a->squawk));

        a->alt_baro =
            json_get_int(obj, obj_end,
                         "\"alt_baro\"",
                         0);

        a->gs =
            json_get_double(obj, obj_end,
                            "\"gs\"",
                            0);

        a->track =
            json_get_double(obj, obj_end,
                            "\"track\"",
                            0);

        a->lat =
            json_get_double(obj, obj_end,
                            "\"lat\"",
                            0);

        a->lon =
            json_get_double(obj, obj_end,
                            "\"lon\"",
                            0);

        a->dst =
            json_get_double(obj, obj_end,
                            "\"dst\"",
                            0);

        a->dir =
            json_get_double(obj, obj_end,
                            "\"dir\"",
                            0);

        a->baro_rate =
            json_get_int(obj, obj_end,
                         "\"baro_rate\"",
                         0);

        count++;

        cursor = obj_end + 1;
    }

    radar.count = count;
    radar_dirty = true;

    syslog(LOG_INFO,
           "Radar: %d aircraft",
           radar.count);
}

static int animation_tick_callback(void* userdata) {

    (void)userdata;

    static unsigned counter = 0;

    counter++;

    if (counter >= REFRESH_INTERVAL_TICKS) {

        update_radar_data();

        counter = 0;
    }

    if (!radar_dirty)
        return G_SOURCE_CONTINUE;

    radar_dirty = false;

    GHashTableIter iter;

    g_hash_table_iter_init(&iter,
                           overlay_table);

    void *key, *value;

    while (g_hash_table_iter_next(&iter,
                                  &key,
                                  &value)) {

        process_next_frame(value);
    }

    return G_SOURCE_CONTINUE;
}

static int stream_event_callback(GIOChannel* channel,
                                 GIOCondition condition,
                                 void* userdata) {

    (void)channel;
    (void)userdata;

    GError* error = NULL;

    VdoMap* vdo_event = NULL;
    VdoStream* vdo_stream = NULL;
    VdoMap* stream_info = NULL;

    if (condition & (G_IO_ERR | G_IO_HUP)) {

        syslog(LOG_ERR,
               "VDO channel error");

        g_main_loop_quit(main_loop);

        return G_SOURCE_REMOVE;
    }

    vdo_event =
        vdo_stream_get_event(vdo_event_stream,
                             &error);

    if (!vdo_event) {

        if (error)
            g_error_free(error);

        return G_SOURCE_CONTINUE;
    }

    unsigned event_type =
        vdo_map_get_uint32(vdo_event,
                           "event",
                           0);

    unsigned stream_id =
        vdo_map_get_uint32(vdo_event,
                           "id",
                           0);

    if (event_type == VDO_STREAM_EVENT_EXISTING ||
        event_type == VDO_STREAM_EVENT_CREATED) {

        vdo_stream =
            vdo_stream_get(stream_id,
                           &error);

        if (!vdo_stream) {

            if (error)
                g_error_free(error);

            g_clear_object(&vdo_event);

            return G_SOURCE_CONTINUE;
        }

        stream_info =
            vdo_stream_get_info(vdo_stream,
                                &error);

        if (!stream_info) {

            if (error)
                g_error_free(error);

            g_clear_object(&vdo_event);
            g_clear_object(&vdo_stream);

            return G_SOURCE_CONTINUE;
        }

        unsigned width =
            vdo_map_get_uint32(stream_info,
                               "width",
                               0);

        unsigned height =
            vdo_map_get_uint32(stream_info,
                               "height",
                               0);

        create_overlay(stream_id,
                       width,
                       height);

    } else if (event_type == VDO_STREAM_EVENT_CLOSED) {

        remove_overlay(stream_id);
    }

    g_clear_object(&vdo_event);
    g_clear_object(&vdo_stream);
    g_clear_object(&stream_info);

    return G_SOURCE_CONTINUE;
}

static void create_overlay(unsigned stream_id,
                           unsigned stream_width,
                           unsigned stream_height) {

    (void)stream_width;
    (void)stream_height;

    axo_err* axo_error = NULL;

    unsigned overlay_used_width = OVERLAY_WIDTH;
    unsigned overlay_used_height = OVERLAY_HEIGHT;

    unsigned overlay_full_width = 0;
    unsigned overlay_full_height = 0;

    axo_get_aligned_size(AXO_FORMAT_ARGB32,
                         overlay_used_width,
                         overlay_used_height,
                         &overlay_full_width,
                         &overlay_full_height,
                         &axo_error);

    if (axo_error)
        axo_err_clear(&axo_error);

    if (overlay_full_width == 0 ||
        overlay_full_height == 0) {

        syslog(LOG_ERR,
               "Aligned size zero for stream %u",
               stream_id);

        return;
    }

    axo_props* props = axo_props_new();

    axo_props_set_format(props,
                         AXO_FORMAT_ARGB32);

    axo_props_set_size(props,
                       overlay_full_width,
                       overlay_full_height);

    axo_match* match = axo_match_new();

    axo_match_stream_id(match,
                        stream_id);

    int overlay_id =
        axo_create_overlay(props,
                           match,
                           &axo_error);

    if (overlay_id < 0) {

        syslog(LOG_ERR,
               "Create overlay failed for stream %u",
               stream_id);

        if (axo_error)
            axo_err_clear(&axo_error);

        return;
    }

    cairo_surface_t* surface =
        cairo_image_surface_create(
            CAIRO_FORMAT_ARGB32,
            overlay_full_width,
            overlay_full_height);

    if (cairo_surface_status(surface) !=
        CAIRO_STATUS_SUCCESS) {

        syslog(LOG_ERR,
               "Cairo surface failed for stream %u",
               stream_id);

        cairo_surface_destroy(surface);

        axo_remove_overlay(overlay_id, NULL);

        return;
    }

    struct overlay* overlay =
        g_malloc0(sizeof(*overlay));

    overlay->overlay_id = overlay_id;
    overlay->stream_id = stream_id;
    overlay->used_width = overlay_used_width;
    overlay->used_height = overlay_used_height;
    overlay->full_width = overlay_full_width;
    overlay->full_height = overlay_full_height;
    overlay->surface = surface;

    g_hash_table_insert(overlay_table,
                        GINT_TO_POINTER(stream_id),
                        overlay);

    radar_dirty = true;
}

static void remove_overlay(unsigned stream_id) {

    const struct overlay* overlay =
        g_hash_table_lookup(overlay_table,
                            GINT_TO_POINTER(stream_id));

    if (!overlay)
        return;

    axo_remove_overlay(overlay->overlay_id,
                       NULL);

    g_hash_table_remove(overlay_table,
                        GINT_TO_POINTER(stream_id));
}

static void process_next_frame(struct overlay* overlay) {

    axo_err* axo_error = NULL;

    axo_buffer* buffer =
        axo_get_buffer(overlay->overlay_id,
                       NULL,
                       &axo_error);

    if (!buffer) {

        if (axo_error)
            axo_err_clear(&axo_error);

        return;
    }

    char* target_buffer =
        axo_buffer_get_data(buffer,
                            &axo_error);

    if (!target_buffer) {

        if (axo_error)
            axo_err_clear(&axo_error);

        return;
    }

    render_frame(overlay,
                 target_buffer);

    axo_submit_buffer(buffer,
                      NULL,
                      &axo_error);

    if (axo_error)
        axo_err_clear(&axo_error);
}

static void draw_text_colored(cairo_t* cr,
                              double x,
                              double y,
                              const char* text,
                              double r,
                              double g,
                              double b,
                              double a) {

    cairo_set_source_rgba(cr,
                          0,
                          0,
                          0,
                          0.40);

    cairo_move_to(cr,
                  x + 1,
                  y + 1);

    cairo_show_text(cr,
                    text);

    cairo_set_source_rgba(cr,
                          r,
                          g,
                          b,
                          a);

    cairo_move_to(cr,
                  x,
                  y);

    cairo_show_text(cr,
                    text);
}

static void draw_aircraft_icon(cairo_t* cr,
                               double cx,
                               double cy,
                               double heading,
                               double scale) {

    double rad =
        heading * M_PI / 180.0;

    cairo_save(cr);

    cairo_translate(cr,
                    cx,
                    cy);

    cairo_rotate(cr,
                 rad);

    cairo_scale(cr,
                scale,
                scale);

    cairo_move_to(cr, 0, -10);
    cairo_line_to(cr, 0, 10);

    cairo_move_to(cr, -8, 0);
    cairo_line_to(cr, 8, 0);

    cairo_move_to(cr, -4, 8);
    cairo_line_to(cr, 4, 8);

    cairo_set_line_width(cr,
                         2.0);

    cairo_set_source_rgba(cr,
                          0.2,
                          0.85,
                          1.0,
                          0.95);

    cairo_stroke(cr);

    cairo_restore(cr);
}

static const char* altitude_band(int alt) {

    if (alt <= 0)
        return "GND";

    if (alt < 5000)
        return "LOW";

    if (alt < 15000)
        return "MID";

    if (alt < 30000)
        return "HIGH";

    return "UPR";
}

static void climb_indicator(int baro_rate,
                            char* out,
                            size_t size) {

    if (baro_rate > 200)
        snprintf(out, size, "▲");

    else if (baro_rate < -200)
        snprintf(out, size, "▼");

    else
        snprintf(out, size, "—");
}

static void render_frame(struct overlay* overlay,
                         char* target_buffer) {

    cairo_t* cr =
        cairo_create(overlay->surface);

    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {

        syslog(LOG_ERR, "Cairo context failed");

        cairo_destroy(cr);

        return;
    }

    cairo_set_operator(cr,
                       CAIRO_OPERATOR_SOURCE);

    cairo_set_source_rgba(cr,
                          0,
                          0,
                          0,
                          0);

    cairo_paint(cr);

    cairo_set_operator(cr,
                       CAIRO_OPERATOR_OVER);

    double w = overlay->used_width;
    double h = overlay->used_height;

    double radius = 20;

    cairo_new_path(cr);

    cairo_arc(cr,
              w - radius,
              radius,
              radius,
              -M_PI / 2,
              0);

    cairo_arc(cr,
              w - radius,
              h - radius,
              radius,
              0,
              M_PI / 2);

    cairo_arc(cr,
              radius,
              h - radius,
              radius,
              M_PI / 2,
              M_PI);

    cairo_arc(cr,
              radius,
              radius,
              radius,
              M_PI,
              3 * M_PI / 2);

    cairo_close_path(cr);

    cairo_set_source_rgba(cr,
                          0.02,
                          0.04,
                          0.08,
                          0.78);

    cairo_fill_preserve(cr);

    cairo_set_source_rgba(cr,
                          0.2,
                          0.85,
                          1.0,
                          0.35);

    cairo_set_line_width(cr,
                         1.0);

    cairo_stroke(cr);

    cairo_select_font_face(cr,
                           "DejaVu Sans",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);

    cairo_set_font_size(cr,
                        18);

    draw_aircraft_icon(cr,
                       32,
                       26,
                       0,
                       1.0);

    draw_text_colored(cr,
                      50,
                      32,
                      "ADS-B RADAR",
                      0.2,
                      0.85,
                      1.0,
                      0.95);

    char count_str[32];

    snprintf(count_str,
             sizeof(count_str),
             "%d AIRCRAFT",
             radar.total);

    cairo_set_font_size(cr,
                        12);

    draw_text_colored(cr,
                      w - 130,
                      32,
                      count_str,
                      0.6,
                      0.9,
                      0.6,
                      0.90);

    cairo_set_source_rgba(cr,
                          0.2,
                          0.85,
                          1.0,
                          0.20);

    cairo_move_to(cr,
                  16,
                  48);

    cairo_line_to(cr,
                  w - 16,
                  48);

    cairo_set_line_width(cr,
                         0.8);

    cairo_stroke(cr);

    cairo_set_font_size(cr,
                        10);

    cairo_select_font_face(cr,
                           "DejaVu Sans",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);

    draw_text_colored(cr, 20, 62, "FLIGHT",
                      0.5, 0.7, 0.8, 0.70);

    draw_text_colored(cr, 110, 62, "TYPE",
                      0.5, 0.7, 0.8, 0.70);

    draw_text_colored(cr, 190, 62, "ALT",
                      0.5, 0.7, 0.8, 0.70);

    draw_text_colored(cr, 260, 62, "SPD",
                      0.5, 0.7, 0.8, 0.70);

    draw_text_colored(cr, 320, 62, "V/S",
                      0.5, 0.7, 0.8, 0.70);

    draw_text_colored(cr, 370, 62, "DIST",
                      0.5, 0.7, 0.8, 0.70);

    draw_text_colored(cr, 430, 62, "BRG",
                      0.5, 0.7, 0.8, 0.70);

    draw_text_colored(cr, 490, 62, "BAND",
                      0.5, 0.7, 0.8, 0.70);

    cairo_select_font_face(cr,
                           "DejaVu Sans",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);

    cairo_set_font_size(cr, 12);

    double row_y = 82;
    double row_h = 26;

    int max_rows = (int)((h - row_y - 50) / row_h);

    if (max_rows > radar.count)
        max_rows = radar.count;

    for (int i = 0; i < max_rows; i++) {

        const struct aircraft* a =
            &radar.ac[i];

        double y = row_y + i * row_h;

        if (i % 2 == 0) {

            cairo_set_source_rgba(cr,
                                  1, 1, 1, 0.03);

            cairo_rectangle(cr,
                            16, y - 14,
                            w - 32, row_h);

            cairo_fill(cr);
        }

        draw_aircraft_icon(cr,
                           12, y - 4,
                           a->track, 0.5);

        const char* label =
            a->flight[0] ? a->flight :
            a->reg[0]    ? a->reg    :
                           a->hex;

        draw_text_colored(cr, 20, y, label,
                          0.95, 0.95, 0.95, 0.95);

        draw_text_colored(cr, 110, y, a->type,
                          0.7, 0.8, 0.9, 0.85);

        char buf[32];

        snprintf(buf, sizeof(buf),
                 "%d", a->alt_baro);

        draw_text_colored(cr, 190, y, buf,
                          0.9, 0.9, 0.5, 0.90);

        snprintf(buf, sizeof(buf),
                 "%.0f", a->gs);

        draw_text_colored(cr, 260, y, buf,
                          0.8, 0.8, 0.8, 0.85);

        char vs[8];

        climb_indicator(a->baro_rate,
                        vs, sizeof(vs));

        double vr = 0.7, vg = 0.7, vb = 0.7;

        if (a->baro_rate > 200) {
            vr = 0.3; vg = 0.95; vb = 0.5;
        } else if (a->baro_rate < -200) {
            vr = 1.0; vg = 0.5; vb = 0.3;
        }

        draw_text_colored(cr, 330, y, vs,
                          vr, vg, vb, 0.90);

        snprintf(buf, sizeof(buf),
                 "%.1f", a->dst);

        draw_text_colored(cr, 370, y, buf,
                          0.8, 0.8, 0.8, 0.85);

        snprintf(buf, sizeof(buf),
                 "%.0f", a->dir);

        draw_text_colored(cr, 430, y, buf,
                          0.8, 0.8, 0.8, 0.85);

        const char* band =
            altitude_band(a->alt_baro);

        double br = 0.5, bg = 0.8, bb = 0.5;

        if (strcmp(band, "LOW") == 0) {
            br = 1.0; bg = 0.8; bb = 0.3;
        } else if (strcmp(band, "HIGH") == 0 ||
                   strcmp(band, "UPR") == 0) {
            br = 0.4; bg = 0.7; bb = 1.0;
        }

        draw_text_colored(cr, 490, y, band,
                          br, bg, bb, 0.85);
    }

    if (radar.count == 0) {

        cairo_set_font_size(cr, 14);

        draw_text_colored(cr,
                          w / 2 - 80, h / 2,
                          "NO AIRCRAFT DETECTED",
                          0.5, 0.6, 0.7, 0.60);
    }

    cairo_set_source_rgba(cr,
                          0.2, 0.85, 1.0, 0.15);

    cairo_move_to(cr, 16, h - 30);
    cairo_line_to(cr, w - 16, h - 30);

    cairo_set_line_width(cr, 0.6);

    cairo_stroke(cr);

    cairo_set_font_size(cr, 10);

    draw_text_colored(cr, 20, h - 14,
                      "SRC: AIRPLANES.LIVE",
                      0.4, 0.6, 0.7, 0.50);

    draw_text_colored(cr, 200, h - 14,
                      "23 NM RADIUS",
                      0.4, 0.6, 0.7, 0.50);

    draw_text_colored(cr, 380, h - 14,
                      "12.97N  77.59E",
                      0.4, 0.6, 0.7, 0.50);

    cairo_destroy(cr);

    cairo_surface_flush(overlay->surface);

    unsigned byte_size =
        overlay->full_width *
        overlay->full_height *
        sizeof(uint32_t);

    memcpy(target_buffer,
           cairo_image_surface_get_data(
               overlay->surface),
           byte_size);
}