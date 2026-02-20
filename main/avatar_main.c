#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_http_server.h"

#include "cJSON.h"

typedef enum {
    FACE_HAPPY = 0,
    FACE_SAD,
    FACE_PUZZLED,
    FACE_ANGRY,
    FACE_NEUTRAL,
    FACE_COUNT
} face_expr_t;

static const char *TAG = "rigo_avatar";

static lv_obj_t *eye_l, *eye_r, *pupil_l, *pupil_r, *mouth;
static lv_obj_t *brow_l, *brow_r;
static lv_obj_t *label;

static lv_point_precise_t brow_l_pts[2];
static lv_point_precise_t brow_r_pts[2];
static lv_style_t brow_style;

static portMUX_TYPE state_mux = portMUX_INITIALIZER_UNLOCKED;
static face_expr_t desired_expr = FACE_HAPPY;
static bool desired_talk = false;
static int64_t talk_until_us = 0;
static int talk_speed = 6; // animation speed hint

static face_expr_t current_expr = FACE_HAPPY;
static bool eyes_closed = false;
static int talk_phase = 0;

static const char *expr_to_str(face_expr_t e)
{
    switch (e) {
        case FACE_HAPPY: return "happy";
        case FACE_SAD: return "sad";
        case FACE_PUZZLED: return "puzzled";
        case FACE_ANGRY: return "angry";
        case FACE_NEUTRAL: return "neutral";
        default: return "neutral";
    }
}

static face_expr_t str_to_expr(const char *s)
{
    if (!s) return FACE_NEUTRAL;
    if (strcasecmp(s, "happy") == 0) return FACE_HAPPY;
    if (strcasecmp(s, "sad") == 0) return FACE_SAD;
    if (strcasecmp(s, "puzzled") == 0) return FACE_PUZZLED;
    if (strcasecmp(s, "angry") == 0) return FACE_ANGRY;
    return FACE_NEUTRAL;
}

static void set_brows(int y_left_in, int y_left_out, int y_right_in, int y_right_out)
{
    brow_l_pts[0].x = 95;  brow_l_pts[0].y = y_left_out;
    brow_l_pts[1].x = 145; brow_l_pts[1].y = y_left_in;

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

static void update_talk_mouth(bool talking)
{
    if (!talking) {
        set_expression(current_expr);
        return;
    }

    talk_phase = (talk_phase + 1) % 4;
    int y = 150;
    switch (talk_phase) {
        case 0: lv_arc_set_bg_angles(mouth, 20, 160); y = 148; break;
        case 1: lv_arc_set_bg_angles(mouth, 5, 175);  y = 144; break;
        case 2: lv_arc_set_bg_angles(mouth, 25, 155); y = 150; break;
        default: lv_arc_set_bg_angles(mouth, 10, 170); y = 146; break;
    }
    lv_obj_set_y(mouth, y);
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

static void avatar_tick_cb(lv_timer_t *t)
{
    (void)t;
    face_expr_t expr;
    bool talk;

    portENTER_CRITICAL(&state_mux);
    expr = desired_expr;
    int64_t now = esp_timer_get_time();
    talk = desired_talk || (talk_until_us > now);
    portEXIT_CRITICAL(&state_mux);

    if (expr != current_expr) {
        set_expression(expr);
    }
    update_talk_mouth(talk);
}

static esp_err_t send_json(httpd_req_t *req, int code, const char *json)
{
    httpd_resp_set_status(req, code == 200 ? "200 OK" : "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t state_get_handler(httpd_req_t *req)
{
    face_expr_t e;
    bool talk;

    portENTER_CRITICAL(&state_mux);
    e = desired_expr;
    talk = desired_talk || (talk_until_us > esp_timer_get_time());
    portEXIT_CRITICAL(&state_mux);

    char out[128];
    snprintf(out, sizeof(out), "{\"emotion\":\"%s\",\"talk\":%s}", expr_to_str(e), talk ? "true" : "false");
    return send_json(req, 200, out);
}

static esp_err_t emotion_post_handler(httpd_req_t *req)
{
    char buf[256] = {0};
    int n = httpd_req_recv(req, buf, MIN((int)req->content_len, (int)sizeof(buf) - 1));
    if (n <= 0) return send_json(req, 400, "{\"ok\":false,\"error\":\"empty body\"}");

    cJSON *root = cJSON_Parse(buf);
    if (!root) return send_json(req, 400, "{\"ok\":false,\"error\":\"invalid json\"}");

    const cJSON *emotion = cJSON_GetObjectItemCaseSensitive(root, "emotion");
    const cJSON *hold_ms = cJSON_GetObjectItemCaseSensitive(root, "hold_ms");

    if (!cJSON_IsString(emotion)) {
        cJSON_Delete(root);
        return send_json(req, 400, "{\"ok\":false,\"error\":\"emotion required\"}");
    }

    face_expr_t e = str_to_expr(emotion->valuestring);
    portENTER_CRITICAL(&state_mux);
    desired_expr = e;
    if (cJSON_IsNumber(hold_ms) && hold_ms->valuedouble > 0) {
        talk_until_us = esp_timer_get_time() + (int64_t)(hold_ms->valuedouble * 1000.0);
    }
    portEXIT_CRITICAL(&state_mux);

    cJSON_Delete(root);
    return send_json(req, 200, "{\"ok\":true}");
}

static esp_err_t talk_post_handler(httpd_req_t *req)
{
    char buf[256] = {0};
    int n = httpd_req_recv(req, buf, MIN((int)req->content_len, (int)sizeof(buf) - 1));
    if (n <= 0) return send_json(req, 400, "{\"ok\":false,\"error\":\"empty body\"}");

    cJSON *root = cJSON_Parse(buf);
    if (!root) return send_json(req, 400, "{\"ok\":false,\"error\":\"invalid json\"}");

    const cJSON *on = cJSON_GetObjectItemCaseSensitive(root, "on");
    const cJSON *duration_ms = cJSON_GetObjectItemCaseSensitive(root, "duration_ms");
    const cJSON *speed = cJSON_GetObjectItemCaseSensitive(root, "speed");

    portENTER_CRITICAL(&state_mux);
    if (cJSON_IsBool(on)) desired_talk = cJSON_IsTrue(on);
    if (cJSON_IsNumber(duration_ms) && duration_ms->valuedouble > 0) {
        talk_until_us = esp_timer_get_time() + (int64_t)(duration_ms->valuedouble * 1000.0);
    }
    if (cJSON_IsNumber(speed)) {
        int s = speed->valueint;
        if (s < 1) s = 1;
        if (s > 12) s = 12;
        talk_speed = s;
    }
    portEXIT_CRITICAL(&state_mux);

    cJSON_Delete(root);
    return send_json(req, 200, "{\"ok\":true}");
}

static esp_err_t perform_post_handler(httpd_req_t *req)
{
    char buf[384] = {0};
    int n = httpd_req_recv(req, buf, MIN((int)req->content_len, (int)sizeof(buf) - 1));
    if (n <= 0) return send_json(req, 400, "{\"ok\":false,\"error\":\"empty body\"}");

    cJSON *root = cJSON_Parse(buf);
    if (!root) return send_json(req, 400, "{\"ok\":false,\"error\":\"invalid json\"}");

    const cJSON *emotion = cJSON_GetObjectItemCaseSensitive(root, "emotion");
    const cJSON *talk = cJSON_GetObjectItemCaseSensitive(root, "talk");
    const cJSON *duration_ms = cJSON_GetObjectItemCaseSensitive(root, "duration_ms");
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");

    int auto_duration = 0;
    if (cJSON_IsString(text) && text->valuestring) {
        size_t l = strlen(text->valuestring);
        auto_duration = (int)l * 70; // ~70ms per char speech estimate
        if (auto_duration < 900) auto_duration = 900;
        if (auto_duration > 12000) auto_duration = 12000;
    }

    portENTER_CRITICAL(&state_mux);
    if (cJSON_IsString(emotion)) {
        desired_expr = str_to_expr(emotion->valuestring);
    }

    if (cJSON_IsBool(talk)) {
        desired_talk = cJSON_IsTrue(talk);
    }

    if (cJSON_IsNumber(duration_ms) && duration_ms->valuedouble > 0) {
        talk_until_us = esp_timer_get_time() + (int64_t)(duration_ms->valuedouble * 1000.0);
    } else if (auto_duration > 0) {
        talk_until_us = esp_timer_get_time() + (int64_t)auto_duration * 1000LL;
    }
    portEXIT_CRITICAL(&state_mux);

    cJSON_Delete(root);
    return send_json(req, 200, "{\"ok\":true}");
}

static httpd_handle_t start_http_service(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 8080;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed starting HTTP service");
        return NULL;
    }

    httpd_uri_t u_state = {
        .uri = "/v1/state",
        .method = HTTP_GET,
        .handler = state_get_handler,
    };
    httpd_uri_t u_emotion = {
        .uri = "/v1/emotion",
        .method = HTTP_POST,
        .handler = emotion_post_handler,
    };
    httpd_uri_t u_talk = {
        .uri = "/v1/talk",
        .method = HTTP_POST,
        .handler = talk_post_handler,
    };
    httpd_uri_t u_perform = {
        .uri = "/v1/perform",
        .method = HTTP_POST,
        .handler = perform_post_handler,
    };

    httpd_register_uri_handler(server, &u_state);
    httpd_register_uri_handler(server, &u_emotion);
    httpd_register_uri_handler(server, &u_talk);
    httpd_register_uri_handler(server, &u_perform);

    ESP_LOGI(TAG, "HTTP service ready on port 8080");
    return server;
}

static void start_network(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_ap();
    bool has_sta = strlen(CONFIG_RIGO_WIFI_STA_SSID) > 0;
    if (has_sta) {
        esp_netif_create_default_wifi_sta();
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap_config = {
        .ap = {
            .ssid = CONFIG_RIGO_WIFI_AP_SSID,
            .ssid_len = 0,
            .channel = 1,
            .password = CONFIG_RIGO_WIFI_AP_PASSWORD,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
            .pmf_cfg = {
                    .required = false,
            },
        },
    };

    if (strlen((char *)ap_config.ap.password) == 0) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    wifi_mode_t mode = has_sta ? WIFI_MODE_APSTA : WIFI_MODE_AP;
    ESP_ERROR_CHECK(esp_wifi_set_mode(mode));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    if (has_sta) {
        wifi_config_t sta_config = { 0 };
        strncpy((char *)sta_config.sta.ssid, CONFIG_RIGO_WIFI_STA_SSID, sizeof(sta_config.sta.ssid) - 1);
        strncpy((char *)sta_config.sta.password, CONFIG_RIGO_WIFI_STA_PASSWORD, sizeof(sta_config.sta.password) - 1);
        sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        sta_config.sta.pmf_cfg.capable = true;
        sta_config.sta.pmf_cfg.required = false;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    if (has_sta) {
        esp_wifi_connect();
    }

    ESP_LOGI(TAG, "AP started: SSID=%s", CONFIG_RIGO_WIFI_AP_SSID);
    if (has_sta) {
        ESP_LOGI(TAG, "STA connect requested: SSID=%s", CONFIG_RIGO_WIFI_STA_SSID);
    }
    ESP_LOGI(TAG, "Control API: http://192.168.4.1:8080/v1/* (AP)");
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    bsp_display_start();
    bsp_display_lock(0);

    lv_obj_t *scr = lv_disp_get_scr_act(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

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

    lv_style_init(&brow_style);
    lv_style_set_line_width(&brow_style, 6);
    lv_style_set_line_color(&brow_style, lv_color_hex(0xFFFFFF));
    lv_style_set_line_rounded(&brow_style, true);

    brow_l = lv_line_create(scr);
    brow_r = lv_line_create(scr);
    lv_obj_add_style(brow_l, &brow_style, 0);
    lv_obj_add_style(brow_r, &brow_style, 0);

    mouth = lv_arc_create(scr);
    lv_obj_set_size(mouth, 120, 70);
    lv_obj_remove_style(mouth, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(mouth, 7, LV_PART_MAIN);
    lv_obj_set_style_arc_width(mouth, 7, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(mouth, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_arc_color(mouth, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
    lv_obj_clear_flag(mouth, LV_OBJ_FLAG_CLICKABLE);

    label = lv_label_create(scr);
    lv_obj_set_style_text_color(label, lv_color_hex(0x7FDBFF), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -12);

    set_expression(FACE_HAPPY);

    lv_timer_create(blink_cb, 220, NULL);
    lv_timer_create(blink_cb, 2800, NULL);
    lv_timer_create(avatar_tick_cb, 130, NULL);

    bsp_display_unlock();
    bsp_display_backlight_on();

    start_network();
    start_http_service();

    ESP_LOGI(TAG, "Rigo avatar + emotion service ready");
}
