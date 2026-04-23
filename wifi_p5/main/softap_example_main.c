#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t style_css_start[]  asm("_binary_style_css_start");
extern const uint8_t style_css_end[]    asm("_binary_style_css_end");

#define WIFI_SSID      "SudokuESP32"
#define WIFI_PASSWORD  "12345678"
#define WIFI_CHANNEL   1
#define MAX_STA_CONN   4
#define GPIO_BUTTON    4
#define INITIAL_CLUES  20
#define SUDOKU_SIZE    9

static const char *TAG = "SUDOKU";

typedef struct { uint8_t value; bool fixed; } Cell;
typedef Cell Board[SUDOKU_SIZE][SUDOKU_SIZE];
typedef enum { GAME_RUNNING, GAME_WON, GAME_QUIT } GameStatus;
typedef struct { Board board; GameStatus status; int64_t start_us; } GameState;



static GameState         g_game;
static SemaphoreHandle_t g_mutex;
static SemaphoreHandle_t g_btn_sem;

static char s_tbl[4096];
static char s_msg[160];
static char s_page[12288];
static char s_json[2048];

static uint32_t elapsed_seconds(void)
{
    return (uint32_t)((esp_timer_get_time() - g_game.start_us) / 1000000ULL);
}

/*
 * Banco de soluciones validas. Se elige una aleatoriamente para
 * evitar recursion profunda que desborda el stack del ESP32.
 */
static const uint8_t SOLUTIONS[3][9][9] = {
    {
        {5,3,4,6,7,8,9,1,2},
        {6,7,2,1,9,5,3,4,8},
        {1,9,8,3,4,2,5,6,7},
        {8,5,9,7,6,1,4,2,3},
        {4,2,6,8,5,3,7,9,1},
        {7,1,3,9,2,4,8,5,6},
        {9,6,1,5,3,7,2,8,4},
        {2,8,7,4,1,9,6,3,5},
        {3,4,5,2,8,6,1,7,9},
    },
    {
        {1,2,3,4,5,6,7,8,9},
        {4,5,6,7,8,9,1,2,3},
        {7,8,9,1,2,3,4,5,6},
        {2,1,4,3,6,5,8,9,7},
        {3,6,5,8,9,7,2,1,4},
        {8,9,7,2,1,4,3,6,5},
        {5,3,1,6,4,2,9,7,8},
        {6,4,2,9,7,8,5,3,1},
        {9,7,8,5,3,1,6,4,2},
    },
    {
        {8,2,7,1,5,4,3,9,6},
        {9,6,5,3,2,7,1,4,8},
        {3,4,1,6,8,9,7,5,2},
        {5,9,3,4,6,8,2,7,1},
        {4,7,2,5,1,3,6,8,9},
        {6,1,8,9,7,2,4,3,5},
        {7,8,6,2,3,5,9,1,4},
        {1,5,4,7,9,6,8,2,3},
        {2,3,9,8,4,1,5,6,7},
    },
};

static void sudoku_init(Board board, uint8_t clues)
{
    srand((unsigned)esp_timer_get_time());

    /* Elegir una solucion aleatoria del banco */
    int s = rand() % 3;
    const uint8_t (*sol)[9] = SOLUTIONS[s];

    /* Llenar tablero con la solucion */
    for (int r = 0; r < SUDOKU_SIZE; r++)
        for (int c = 0; c < SUDOKU_SIZE; c++) {
            board[r][c].value = sol[r][c];
            board[r][c].fixed = false;
        }

    /* Seleccionar celdas pista con Fisher-Yates */
    uint8_t idx[81];
    for (int i = 0; i < 81; i++) idx[i] = (uint8_t)i;
    for (int i = 80; i > 0; i--) {
        int j = rand() % (i + 1);
        uint8_t t = idx[i]; idx[i] = idx[j]; idx[j] = t;
    }

    for (int k = 0; k < clues; k++)
        board[idx[k] / SUDOKU_SIZE][idx[k] % SUDOKU_SIZE].fixed = true;

    for (int r = 0; r < SUDOKU_SIZE; r++)
        for (int c = 0; c < SUDOKU_SIZE; c++)
            if (!board[r][c].fixed)
                board[r][c].value = 0;
}

static bool sudoku_is_solved(const Board board)
{
    // Verificar que no haya celdas vacias
    for (int r = 0; r < SUDOKU_SIZE; r++)
        for (int c = 0; c < SUDOKU_SIZE; c++)
            if (board[r][c].value == 0) return false;

    // Verificar filas
    for (int r = 0; r < SUDOKU_SIZE; r++) {
        uint16_t seen = 0;
        for (int c = 0; c < SUDOKU_SIZE; c++) {
            uint16_t bit = (uint16_t)(1 << board[r][c].value);
            if (seen & bit) return false;
            seen |= bit;
        }
    }
    // Verificar columnas
    for (int c = 0; c < SUDOKU_SIZE; c++) {
        uint16_t seen = 0;
        for (int r = 0; r < SUDOKU_SIZE; r++) {
            uint16_t bit = (uint16_t)(1 << board[r][c].value);
            if (seen & bit) return false;
            seen |= bit;
        }
    }
    // Verificar subcuadriculas 3x3
    for (int br = 0; br < 3; br++) {
        for (int bc = 0; bc < 3; bc++) {
            uint16_t seen = 0;
            for (int r = br*3; r < br*3+3; r++) {
                for (int c = bc*3; c < bc*3+3; c++) {
                    uint16_t bit = (uint16_t)(1 << board[r][c].value);
                    if (seen & bit) return false;
                    seen |= bit;
                }
            }
        }
    }
    return true;
}

static int sudoku_place(Board board, uint8_t r, uint8_t c, uint8_t v)
{
    if (r >= SUDOKU_SIZE || c >= SUDOKU_SIZE) return -2;
    if (board[r][c].fixed) return -1;
    board[r][c].value = v;
    return 0;
}

static void build_page(const GameState *gs, uint32_t elapsed)
{
    int tp = 0;
    tp += snprintf(s_tbl + tp, sizeof(s_tbl) - tp, "<table id=\"tablero\">");
    for (int r = 0; r < SUDOKU_SIZE; r++) {
        tp += snprintf(s_tbl + tp, sizeof(s_tbl) - tp, "<tr>");
        for (int c = 0; c < SUDOKU_SIZE; c++) {
            const Cell *cell = &gs->board[r][c];
            const char *cls = cell->fixed ? "fija" : (cell->value > 0 ? "user" : "vacia");
            if (cell->value > 0)
                tp += snprintf(s_tbl + tp, sizeof(s_tbl) - tp,
                               "<td class=\"%s\">%d</td>", cls, cell->value);
            else
                tp += snprintf(s_tbl + tp, sizeof(s_tbl) - tp,
                               "<td class=\"%s\"></td>", cls);
        }
        tp += snprintf(s_tbl + tp, sizeof(s_tbl) - tp, "</tr>");
    }
    snprintf(s_tbl + tp, sizeof(s_tbl) - tp, "</table>");

    s_msg[0] = '\0';
    if (gs->status == GAME_WON)
        snprintf(s_msg, sizeof(s_msg),
                 "<p id=\"mensaje\" style=\"display:block\">Felicidades, te tomo %lu segundos.</p>",
                 (unsigned long)elapsed);
    else if (gs->status == GAME_QUIT)
        snprintf(s_msg, sizeof(s_msg),
                 "<p id=\"mensaje\" style=\"display:block\">Nos vemos!</p>");

    const char  *src     = (const char *)index_html_start;
    size_t       src_len = (size_t)(index_html_end - index_html_start);
    const char  *M_TBL     = "<!--TABLERO-->";
    const char  *M_TIME    = ">0<";
    const size_t L_TBL     = 14;
    const size_t L_TIME    = 3;

    int bp = 0;
    size_t i = 0;
    while (i < src_len && bp < (int)sizeof(s_page) - 1) {
        if (i + L_TBL <= src_len && memcmp(src + i, M_TBL, L_TBL) == 0) {
            bp += snprintf(s_page + bp, sizeof(s_page) - bp, "%s%s", s_tbl, s_msg);
            i += L_TBL;
        } else if (i + L_TIME <= src_len && memcmp(src + i, M_TIME, L_TIME) == 0) {
            bp += snprintf(s_page + bp, sizeof(s_page) - bp, ">%lu<", (unsigned long)elapsed);
            i += L_TIME;
        } else {
            s_page[bp++] = src[i++];
        }
    }
    s_page[bp] = '\0';
}

static void build_json(uint32_t elapsed, const GameState *gs)
{
    const char *status_str = gs->status == GAME_WON  ? "won"  :
                             gs->status == GAME_QUIT ? "quit" : "running";
    int pos = 0;
    pos += snprintf(s_json + pos, sizeof(s_json) - pos,
                    "{\"elapsed\":%lu,\"status\":\"%s\",\"board\":[",
                    (unsigned long)elapsed, status_str);
    for (int r = 0; r < SUDOKU_SIZE; r++) {
        pos += snprintf(s_json + pos, sizeof(s_json) - pos, "[");
        for (int c = 0; c < SUDOKU_SIZE; c++) {
            pos += snprintf(s_json + pos, sizeof(s_json) - pos,
                            "{\"value\":%d,\"fixed\":%s}%s",
                            gs->board[r][c].value,
                            gs->board[r][c].fixed ? "true" : "false",
                            c < 8 ? "," : "");
        }
        pos += snprintf(s_json + pos, sizeof(s_json) - pos, "]%s", r < 8 ? "," : "");
    }
    snprintf(s_json + pos, sizeof(s_json) - pos, "]}");
}

static int parse_form_int(const char *body, const char *key)
{
    char search[32];
    snprintf(search, sizeof(search), "%s=", key);
    const char *p = strstr(body, search);
    if (!p) return -1;
    return atoi(p + strlen(search));
}

static esp_err_t handler_root(httpd_req_t *req)
{
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    uint32_t elapsed = elapsed_seconds();
    GameState snap = g_game;
    xSemaphoreGive(g_mutex);

    build_page(&snap, elapsed);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, s_page);
    return ESP_OK;
}

static esp_err_t handler_state(httpd_req_t *req)
{
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    uint32_t elapsed = elapsed_seconds();
    GameState snap = g_game;
    xSemaphoreGive(g_mutex);

    build_json(elapsed, &snap);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, s_json);
    return ESP_OK;
}

static esp_err_t handler_css(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/css");
    httpd_resp_send(req, (const char *)style_css_start,
                    (ssize_t)(style_css_end - style_css_start));
    return ESP_OK;
}

static esp_err_t handler_place(httpd_req_t *req)
{
    char body[128] = {0};
    httpd_req_recv(req, body, sizeof(body) - 1);

    int r = parse_form_int(body, "row") - 1;
    int c = parse_form_int(body, "col") - 1;
    int v = parse_form_int(body, "num");

    xSemaphoreTake(g_mutex, portMAX_DELAY);
    if (g_game.status == GAME_RUNNING &&
        r >= 0 && r < 9 && c >= 0 && c < 9 && v >= 1 && v <= 9) {
        int rc = sudoku_place(g_game.board, (uint8_t)r, (uint8_t)c, (uint8_t)v);
        if (rc == 0 && sudoku_is_solved(g_game.board))
            g_game.status = GAME_WON;
    }
    xSemaphoreGive(g_mutex);

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handler_restart(httpd_req_t *req)
{
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    sudoku_init(g_game.board, INITIAL_CLUES);
    g_game.status   = GAME_RUNNING;
    g_game.start_us = esp_timer_get_time();
    xSemaphoreGive(g_mutex);

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static void start_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) return;

    const httpd_uri_t uris[] = {
        { .uri = "/",          .method = HTTP_GET,  .handler = handler_root    },
        { .uri = "/state",     .method = HTTP_GET,  .handler = handler_state   },
        { .uri = "/style.css", .method = HTTP_GET,  .handler = handler_css     },
        { .uri = "/place",     .method = HTTP_POST, .handler = handler_place   },
        { .uri = "/restart",   .method = HTTP_POST, .handler = handler_restart },
    };

    for (int i = 0; i < 5; i++)
        httpd_register_uri_handler(server, &uris[i]);

    ESP_LOGI(TAG, "HTTP server listo");
}

static void wifi_init_ap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid           = WIFI_SSID,
            .ssid_len       = strlen(WIFI_SSID),
            .channel        = WIFI_CHANNEL,
            .password       = WIFI_PASSWORD,
            .max_connection = MAX_STA_CONN,
            .authmode       = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP listo — SSID: %s  IP: 192.168.4.1", WIFI_SSID);
}

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(g_btn_sem, &woken);
    portYIELD_FROM_ISR(woken);
}

static void task_button(void *arg)
{
    while (1) {
        if (xSemaphoreTake(g_btn_sem, portMAX_DELAY) == pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(50));
            if (gpio_get_level(GPIO_BUTTON) == 0) {
                xSemaphoreTake(g_mutex, portMAX_DELAY);
                if (g_game.status == GAME_RUNNING)
                    g_game.status = GAME_QUIT;
                xSemaphoreGive(g_mutex);
            }
        }
    }
}

static void button_init(void)
{
    g_btn_sem = xSemaphoreCreateBinary();

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << GPIO_BUTTON),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(GPIO_BUTTON, gpio_isr_handler, NULL));
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    g_mutex = xSemaphoreCreateMutex();

    sudoku_init(g_game.board, INITIAL_CLUES);
    g_game.status   = GAME_RUNNING;
    g_game.start_us = esp_timer_get_time();

    wifi_init_ap();
    start_server();
    button_init();

    xTaskCreate(task_button, "btn", 2048, NULL, 5, NULL);

    ESP_LOGI(TAG, "Listo -> %s  http://192.168.4.1", WIFI_SSID);
}