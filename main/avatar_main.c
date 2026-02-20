#include <math.h>
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "esp_log.h"

static const char *TAG = "rigo_avatar";

typedef enum {
    FACE_HAPPY = 0,
    FACE_SAD,
    FACE_PUZZLED,
    FACE_ANGRY,
    FACE_NEUTRAL,
    FACE_COUNT
} face_expr_t;

static lv_obj_t *eye_l, *eye_r, *pupil_l, *pupil_r, *mouth;
static lv_obj_t *brow_l, *brow_r;
static lv_obj_t *label;
static face_expr_t current_expr = FACE_HAPPY;
static bool eyes_closed = false;

static lv_point_precise_t brow_l_pts[2];
static lv_point_precise_t brow_r_pts[2];
static lv_style_t brow_style;

static void set_brows(int y_left_in, int y_left_out, int y_right_in, int y_right_out)
{
    // Left brow from outer->inner
    brow_l_pts[0].x = 95;  brow_l_pts[0].y = y_left_out;
    brow_l_pts[1].x = 145; brow_l_pts[1].y = y_left_in;

    // Right brow from inner->outer
    brow_r_pts[0].x = 175; brow_r_pts[0].y = y_right_in;
    brow_r_pts[1].x = 225; brow_r_pts[1].y = y_right_out;

    lv_line_set_points(brow_l, brow_l_pts, 2);
    lv_line_set_points(brow_r, brow_r_pts, 2);
}

static void set_mouth_arc(int start, int end, int y)
{
    lv_obj_set_pos(mouth, 110, y);
    lv_arc_set_bg_angles(mouth, start, end);
    lv_arc_set_value(mouth, 0);
}

static void set_expression(face_expr_t expr)
{
    current_expr = expr;

    switch (expr) {
    case FACE_HAPPY:
        set_brows(82, 78, 82, 78);
        set_mouth_arc(25, 155, 145);
        lv_label_set_text(label, "rigo: happy");
        break;
    case FACE_SAD:
        set_brows(78, 82, 78, 82);
        set_mouth_arc(205, 335, 165);
        lv_label_set_text(label, "rigo: sad");
        break;
    case FACE_PUZZLED:
        set_brows(75, 90, 85, 72);
        set_mouth_arc(350, 30, 160);
        lv_label_set_text(label, "rigo: puzzled");
        break;
    case FACE_ANGRY:
        set_brows(95, 70, 95, 70);
        set_mouth_arc(350, 20, 165);
        lv_label_set_text(label, "rigo: angry");
        break;
    case FACE_NEUTRAL:
    default:
        set_brows(80, 80, 80, 80);
        set_mouth_arc(0, 180, 165);
        lv_label_set_text(label, "rigo: neutral");
        break;
    }
}

static void blink_cb(lv_timer_t *t)
{
    (void)t;

    int h = eyes_closed ? 44 : 5;
    int py = eyes_closed ? 10 : 0;

    lv_obj_set_size(eye_l, 44, h);
    lv_obj_set_size(eye_r, 44, h);
    lv_obj_align(eye_l, LV_ALIGN_TOP_LEFT, 85, 95 + py);
    lv_obj_align(eye_r, LV_ALIGN_TOP_LEFT, 190, 95 + py);

    lv_obj_set_style_opa(pupil_l, eyes_closed ? LV_OPA_0 : LV_OPA_100, 0);
    lv_obj_set_style_opa(pupil_r, eyes_closed ? LV_OPA_0 : LV_OPA_100, 0);

    eyes_closed = !eyes_closed;
}

static void expression_cycle_cb(lv_timer_t *t)
{
    (void)t;
    set_expression((current_expr + 1) % FACE_COUNT);
}

void app_main(void)
{
    bsp_display_start();
    bsp_display_backlight_on();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Eyes
    eye_l = lv_obj_create(scr);
    eye_r = lv_obj_create(scr);
    lv_obj_remove_style_all(eye_l);
    lv_obj_remove_style_all(eye_r);
    lv_obj_set_size(eye_l, 44, 44);
    lv_obj_set_size(eye_r, 44, 44);
    lv_obj_set_style_radius(eye_l, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_radius(eye_r, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(eye_l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(eye_r, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(eye_l, LV_ALIGN_TOP_LEFT, 85, 95);
    lv_obj_align(eye_r, LV_ALIGN_TOP_LEFT, 190, 95);

    pupil_l = lv_obj_create(eye_l);
    pupil_r = lv_obj_create(eye_r);
    lv_obj_remove_style_all(pupil_l);
    lv_obj_remove_style_all(pupil_r);
    lv_obj_set_size(pupil_l, 14, 14);
    lv_obj_set_size(pupil_r, 14, 14);
    lv_obj_set_style_radius(pupil_l, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_radius(pupil_r, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pupil_l, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_color(pupil_r, lv_color_hex(0x101820), 0);
    lv_obj_center(pupil_l);
    lv_obj_center(pupil_r);

    // Brows
    lv_style_init(&brow_style);
    lv_style_set_line_width(&brow_style, 6);
    lv_style_set_line_color(&brow_style, lv_color_hex(0xFFFFFF));
    lv_style_set_line_rounded(&brow_style, true);

    brow_l = lv_line_create(scr);
    brow_r = lv_line_create(scr);
    lv_obj_add_style(brow_l, &brow_style, 0);
    lv_obj_add_style(brow_r, &brow_style, 0);

    // Mouth
    mouth = lv_arc_create(scr);
    lv_obj_set_size(mouth, 120, 70);
    lv_obj_remove_style(mouth, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(mouth, 7, LV_PART_MAIN);
    lv_obj_set_style_arc_width(mouth, 7, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(mouth, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_arc_color(mouth, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
    lv_obj_clear_flag(mouth, LV_OBJ_FLAG_CLICKABLE);

    // Name tag
    label = lv_label_create(scr);
    lv_obj_set_style_text_color(label, lv_color_hex(0x7FDBFF), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -12);

    set_expression(FACE_HAPPY);

    // Basic movement timers
    lv_timer_create(blink_cb, 220, NULL);      // quick blink toggle
    lv_timer_create(blink_cb, 2800, NULL);     // reopen after pause cadence
    lv_timer_create(expression_cycle_cb, 5000, NULL);

    ESP_LOGI(TAG, "Rigo avatar running on display");
}
