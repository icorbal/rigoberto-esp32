#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
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
#include "esp_http_client.h"

#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_wn_models.h"
#include "model_path.h"

#include "cJSON.h"

typedef enum {
    FACE_HAPPY = 0,
    FACE_SAD,
    FACE_PUZZLED,
    FACE_ANGRY,
    FACE_NEUTRAL,
    FACE_COUNT
} face_expr_t;

static const char *TAG = "rigo_voice";

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

static face_expr_t current_expr = FACE_HAPPY;
static bool eyes_closed = false;
static int talk_phase = 0;

static esp_codec_dev_handle_t mic_dev;
static esp_codec_dev_handle_t spk_dev;

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
        lv_label_set_text(label, "rigo: listening");
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

static void avatar_set(face_expr_t expr, bool talk, int duration_ms)
{
    portENTER_CRITICAL(&state_mux);
    desired_expr = expr;
    desired_talk = talk;
    if (duration_ms > 0) {
        talk_until_us = esp_timer_get_time() + ((int64_t)duration_ms * 1000LL);
    }
    portEXIT_CRITICAL(&state_mux);
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

static esp_err_t perform_post_handler(httpd_req_t *req)
{
    char buf[384] = {0};
    int n = httpd_req_recv(req, buf, MIN((int)req->content_len, (int)sizeof(buf) - 1));
    if (n <= 0) return send_json(req, 400, "{\"ok\":false}");

    cJSON *root = cJSON_Parse(buf);
    if (!root) return send_json(req, 400, "{\"ok\":false}");

    const cJSON *emotion = cJSON_GetObjectItemCaseSensitive(root, "emotion");
    const cJSON *talk = cJSON_GetObjectItemCaseSensitive(root, "talk");
    const cJSON *duration_ms = cJSON_GetObjectItemCaseSensitive(root, "duration_ms");

    if (cJSON_IsString(emotion)) {
        desired_expr = str_to_expr(emotion->valuestring);
    }
    if (cJSON_IsBool(talk)) {
        desired_talk = cJSON_IsTrue(talk);
    }
    if (cJSON_IsNumber(duration_ms) && duration_ms->valuedouble > 0) {
        talk_until_us = esp_timer_get_time() + (int64_t)(duration_ms->valuedouble * 1000.0);
    }

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

    httpd_uri_t u_state = {.uri = "/v1/state", .method = HTTP_GET, .handler = state_get_handler};
    httpd_uri_t u_perform = {.uri = "/v1/perform", .method = HTTP_POST, .handler = perform_post_handler};

    httpd_register_uri_handler(server, &u_state);
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
            .password = CONFIG_RIGO_WIFI_AP_PASSWORD,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
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
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    if (has_sta) {
        esp_wifi_connect();
    }
}

typedef struct {
    uint8_t *data;
    int len;
} mem_resp_t;

static esp_err_t http_evt(esp_http_client_event_t *evt)
{
    mem_resp_t *m = (mem_resp_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0 && m) {
        uint8_t *n = realloc(m->data, m->len + evt->data_len + 1);
        if (!n) return ESP_FAIL;
        m->data = n;
        memcpy(m->data + m->len, evt->data, evt->data_len);
        m->len += evt->data_len;
        m->data[m->len] = 0;
    }
    return ESP_OK;
}

static void set_auth_header(esp_http_client_handle_t c)
{
    if (strlen(CONFIG_RIGO_API_BEARER) > 0) {
        char auth[256];
        snprintf(auth, sizeof(auth), "Bearer %s", CONFIG_RIGO_API_BEARER);
        esp_http_client_set_header(c, "Authorization", auth);
    }
}

static uint8_t *build_wav(const int16_t *pcm, int bytes, int sample_rate, int channels, int *out_len)
{
    const int total = 44 + bytes;
    uint8_t *wav = malloc(total);
    if (!wav) return NULL;

    memcpy(wav, "RIFF", 4);
    *(uint32_t *)(wav + 4) = total - 8;
    memcpy(wav + 8, "WAVEfmt ", 8);
    *(uint32_t *)(wav + 16) = 16;
    *(uint16_t *)(wav + 20) = 1;
    *(uint16_t *)(wav + 22) = channels;
    *(uint32_t *)(wav + 24) = sample_rate;
    *(uint32_t *)(wav + 28) = sample_rate * channels * 2;
    *(uint16_t *)(wav + 32) = channels * 2;
    *(uint16_t *)(wav + 34) = 16;
    memcpy(wav + 36, "data", 4);
    *(uint32_t *)(wav + 40) = bytes;
    memcpy(wav + 44, pcm, bytes);
    *out_len = total;
    return wav;
}

static char *cloud_stt(const int16_t *pcm, int bytes)
{
    if (strlen(CONFIG_RIGO_STT_URL) == 0) return NULL;
    int wav_len = 0;
    uint8_t *wav = build_wav(pcm, bytes, 16000, 1, &wav_len);
    if (!wav) return NULL;

    mem_resp_t out = {0};
    esp_http_client_config_t cfg = {
        .url = CONFIG_RIGO_STT_URL,
        .method = HTTP_METHOD_POST,
        .event_handler = http_evt,
        .user_data = &out,
        .timeout_ms = 20000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "Content-Type", "audio/wav");
    set_auth_header(c);
    esp_http_client_set_post_field(c, (const char *)wav, wav_len);
    esp_err_t err = esp_http_client_perform(c);
    free(wav);

    if (err != ESP_OK) {
        esp_http_client_cleanup(c);
        free(out.data);
        return NULL;
    }

    cJSON *r = cJSON_Parse((char *)out.data);
    char *txt = NULL;
    if (r) {
        cJSON *t = cJSON_GetObjectItemCaseSensitive(r, "text");
        if (cJSON_IsString(t) && t->valuestring && strlen(t->valuestring) > 0) {
            txt = strdup(t->valuestring);
        }
        cJSON_Delete(r);
    }

    esp_http_client_cleanup(c);
    free(out.data);
    return txt;
}

static char *cloud_assistant(const char *user_text)
{
    if (strlen(CONFIG_RIGO_ASSISTANT_URL) == 0 || !user_text) return NULL;
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "text", user_text);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);

    mem_resp_t out = {0};
    esp_http_client_config_t cfg = {
        .url = CONFIG_RIGO_ASSISTANT_URL,
        .method = HTTP_METHOD_POST,
        .event_handler = http_evt,
        .user_data = &out,
        .timeout_ms = 20000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    set_auth_header(c);
    esp_http_client_set_post_field(c, body, strlen(body));
    free(body);

    esp_err_t err = esp_http_client_perform(c);
    if (err != ESP_OK) {
        esp_http_client_cleanup(c);
        free(out.data);
        return NULL;
    }

    cJSON *r = cJSON_Parse((char *)out.data);
    char *txt = NULL;
    if (r) {
        cJSON *t = cJSON_GetObjectItemCaseSensitive(r, "reply");
        if (!cJSON_IsString(t)) t = cJSON_GetObjectItemCaseSensitive(r, "text");
        if (cJSON_IsString(t) && t->valuestring && strlen(t->valuestring) > 0) {
            txt = strdup(t->valuestring);
        }
        cJSON_Delete(r);
    }

    esp_http_client_cleanup(c);
    free(out.data);
    return txt;
}

static uint8_t *cloud_tts(const char *text, int *audio_len)
{
    if (strlen(CONFIG_RIGO_TTS_URL) == 0 || !text) return NULL;
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "text", text);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);

    mem_resp_t out = {0};
    esp_http_client_config_t cfg = {
        .url = CONFIG_RIGO_TTS_URL,
        .method = HTTP_METHOD_POST,
        .event_handler = http_evt,
        .user_data = &out,
        .timeout_ms = 30000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    set_auth_header(c);
    esp_http_client_set_post_field(c, body, strlen(body));
    free(body);

    esp_err_t err = esp_http_client_perform(c);
    esp_http_client_cleanup(c);
    if (err != ESP_OK || out.len <= 0) {
        free(out.data);
        return NULL;
    }

    *audio_len = out.len;
    return out.data;
}

static void play_wav(uint8_t *wav, int len)
{
    if (!wav || len < 44 || memcmp(wav, "RIFF", 4) != 0 || memcmp(wav + 8, "WAVE", 4) != 0) return;

    uint16_t channels = *(uint16_t *)(wav + 22);
    uint32_t sample_rate = *(uint32_t *)(wav + 24);
    uint16_t bps = *(uint16_t *)(wav + 34);
    if (bps != 16) return;

    int data_len = *(uint32_t *)(wav + 40);
    if (44 + data_len > len) data_len = len - 44;

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = sample_rate,
        .channel = channels,
        .bits_per_sample = 16,
    };

    if (esp_codec_dev_open(spk_dev, &fs) != ESP_OK) return;
    avatar_set(FACE_HAPPY, true, 0);
    esp_codec_dev_write(spk_dev, wav + 44, data_len);
    avatar_set(FACE_HAPPY, false, 0);
    esp_codec_dev_close(spk_dev);
}

static void voice_task(void *arg)
{
    (void)arg;

    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models) {
        ESP_LOGE(TAG, "No SR models. Enable Hi ESP model in menuconfig.");
        vTaskDelete(NULL);
        return;
    }

    char *wn = esp_srmodel_filter(models, ESP_WN_PREFIX, "hiesp");
    if (!wn) wn = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
    if (!wn) {
        ESP_LOGE(TAG, "No WakeNet model found in srmodels (need Hi ESP model)");
        esp_srmodel_deinit(models);
        vTaskDelete(NULL);
        return;
    }

    const esp_wn_iface_t *wakenet = esp_wn_handle_from_name(wn);
    if (!wakenet) {
        ESP_LOGE(TAG, "Failed to get WakeNet interface for model: %s", wn);
        esp_srmodel_deinit(models);
        vTaskDelete(NULL);
        return;
    }

    model_iface_data_t *wn_data = wakenet->create(wn, DET_MODE_90);
    if (!wn_data) {
        ESP_LOGE(TAG, "Failed to create WakeNet model data");
        esp_srmodel_deinit(models);
        vTaskDelete(NULL);
        return;
    }

    int feed_n = wakenet->get_samp_chunksize(wn_data);
    int16_t *feed = heap_caps_calloc(feed_n, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    esp_codec_dev_sample_info_t mic_fmt = {
        .sample_rate = 16000,
        .channel = 1,
        .bits_per_sample = 16,
    };
    ESP_ERROR_CHECK(esp_codec_dev_open(mic_dev, &mic_fmt));

    const int max_bytes = CONFIG_RIGO_CAPTURE_MS * 16 * 2;
    int16_t *capt = heap_caps_malloc(max_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    ESP_LOGI(TAG, "WakeNet ready (%s). Say: Hi ESP", wn);

    while (1) {
        esp_codec_dev_read(mic_dev, feed, feed_n * sizeof(int16_t));
        if (wakenet->detect(wn_data, feed) == WAKENET_DETECTED) {
            avatar_set(FACE_PUZZLED, false, 0);
            ESP_LOGI(TAG, "Wake detected");

            int cap_bytes = 0;
            while (cap_bytes < max_bytes) {
                int rd = feed_n * sizeof(int16_t);
                if (cap_bytes + rd > max_bytes) rd = max_bytes - cap_bytes;
                esp_codec_dev_read(mic_dev, ((uint8_t *)capt) + cap_bytes, rd);
                cap_bytes += rd;
            }

            avatar_set(FACE_NEUTRAL, false, 0);
            char *text = cloud_stt(capt, cap_bytes);
            ESP_LOGI(TAG, "STT: %s", text ? text : "(null)");
            if (!text || strlen(text) == 0) {
                free(text);
                continue;
            }

            char *reply = cloud_assistant(text);
            free(text);
            ESP_LOGI(TAG, "Assistant: %s", reply ? reply : "(null)");
            if (!reply || strlen(reply) == 0) {
                free(reply);
                continue;
            }

            int tts_len = 0;
            uint8_t *tts = cloud_tts(reply, &tts_len);
            free(reply);
            if (tts && tts_len > 44) {
                play_wav(tts, tts_len);
            }
            free(tts);
        }
    }
}

static void init_ui(void)
{
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
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    init_ui();

    ESP_ERROR_CHECK(bsp_audio_init(NULL));
    spk_dev = bsp_audio_codec_speaker_init();
    mic_dev = bsp_audio_codec_microphone_init();
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(spk_dev, 55));
    ESP_ERROR_CHECK(esp_codec_dev_set_in_gain(mic_dev, 35));

    start_network();
    start_http_service();

    xTaskCreatePinnedToCore(voice_task, "voice", 12 * 1024, NULL, 5, NULL, 1);
    ESP_LOGI(TAG, "Rigo voice pipeline ready");
}
