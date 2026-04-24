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

#define WIFI_SSID     "SudokuESP32"
#define WIFI_PASS     "12345678"
#define WIFI_CHANNEL  1
#define MAX_CONN      4
#define BTN_GPIO      4
#define NUM_PISTAS    20
#define TAM           9

static const char *TAG = "SUDOKU";

// estructura de cada celda
typedef struct { uint8_t valor; bool fija; } Celda;
typedef Celda Tablero[TAM][TAM];
typedef enum { JUGANDO, GANADO, SALIR } Estado;

typedef struct {
    Tablero tablero;
    Estado  estado;
    int64_t inicio;
} Juego;

static Juego             g_juego;
static SemaphoreHandle_t g_mutex;
static SemaphoreHandle_t g_btn_sem;

// buffers globales para armar las respuestas HTTP
static char s_tabla[4096];
static char s_msg[160];
static char s_pagina[12288];
static char s_json[2048];

// soluciones validas precargadas para no usar recursion en el ESP32
static const uint8_t SOLUCIONES[3][9][9] = {
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

static uint32_t tiempo_transcurrido(void)
{
    return (uint32_t)((esp_timer_get_time() - g_juego.inicio) / 1000000ULL);
}

static void iniciar_tablero(Tablero t, uint8_t pistas)
{
    srand((unsigned)esp_timer_get_time());
    int s = rand() % 3;

    for (int r = 0; r < TAM; r++)
        for (int c = 0; c < TAM; c++) {
            t[r][c].valor = SOLUCIONES[s][r][c];
            t[r][c].fija  = false;
        }

    // shuffle con Fisher-Yates para elegir celdas pista
    uint8_t idx[81];
    for (int i = 0; i < 81; i++) idx[i] = (uint8_t)i;
    for (int i = 80; i > 0; i--) {
        int j = rand() % (i + 1);
        uint8_t tmp = idx[i]; idx[i] = idx[j]; idx[j] = tmp;
    }

    for (int k = 0; k < pistas; k++)
        t[idx[k] / TAM][idx[k] % TAM].fija = true;

    for (int r = 0; r < TAM; r++)
        for (int c = 0; c < TAM; c++)
            if (!t[r][c].fija) t[r][c].valor = 0;
}

static bool esta_resuelto(const Tablero t)
{
    for (int r = 0; r < TAM; r++)
        for (int c = 0; c < TAM; c++)
            if (t[r][c].valor == 0) return false;

    for (int r = 0; r < TAM; r++) {
        uint16_t visto = 0;
        for (int c = 0; c < TAM; c++) {
            uint16_t bit = (uint16_t)(1 << t[r][c].valor);
            if (visto & bit) return false;
            visto |= bit;
        }
    }

    for (int c = 0; c < TAM; c++) {
        uint16_t visto = 0;
        for (int r = 0; r < TAM; r++) {
            uint16_t bit = (uint16_t)(1 << t[r][c].valor);
            if (visto & bit) return false;
            visto |= bit;
        }
    }

    for (int br = 0; br < 3; br++) {
        for (int bc = 0; bc < 3; bc++) {
            uint16_t visto = 0;
            for (int r = br*3; r < br*3+3; r++)
                for (int c = bc*3; c < bc*3+3; c++) {
                    uint16_t bit = (uint16_t)(1 << t[r][c].valor);
                    if (visto & bit) return false;
                    visto |= bit;
                }
        }
    }
    return true;
}

static int poner_numero(Tablero t, uint8_t r, uint8_t c, uint8_t v)
{
    if (r >= TAM || c >= TAM) return -2;
    if (t[r][c].fija) return -1;
    t[r][c].valor = v;
    return 0;
}

static void armar_pagina(const Juego *j, uint32_t seg)
{
    int tp = 0;
    tp += snprintf(s_tabla + tp, sizeof(s_tabla) - tp, "<table id=\"tablero\">");
    for (int r = 0; r < TAM; r++) {
        tp += snprintf(s_tabla + tp, sizeof(s_tabla) - tp, "<tr>");
        for (int c = 0; c < TAM; c++) {
            const Celda *cel = &j->tablero[r][c];
            const char  *cls = cel->fija ? "fija" : (cel->valor > 0 ? "user" : "vacia");
            if (cel->valor > 0)
                tp += snprintf(s_tabla + tp, sizeof(s_tabla) - tp,
                               "<td class=\"%s\">%d</td>", cls, cel->valor);
            else
                tp += snprintf(s_tabla + tp, sizeof(s_tabla) - tp,
                               "<td class=\"%s\"></td>", cls);
        }
        tp += snprintf(s_tabla + tp, sizeof(s_tabla) - tp, "</tr>");
    }
    snprintf(s_tabla + tp, sizeof(s_tabla) - tp, "</table>");

    s_msg[0] = '\0';
    if (j->estado == GANADO)
        snprintf(s_msg, sizeof(s_msg),
                 "<p id=\"mensaje\" style=\"display:block\">Felicidades, te tomo %lu segundos.</p>",
                 (unsigned long)seg);
    else if (j->estado == SALIR)
        snprintf(s_msg, sizeof(s_msg),
                 "<p id=\"mensaje\" style=\"display:block\">Nos vemos!</p>");

    const char *src     = (const char *)index_html_start;
    size_t      src_len = (size_t)(index_html_end - index_html_start);

    int bp = 0;
    size_t i = 0;
    while (i < src_len && bp < (int)sizeof(s_pagina) - 1) {
        if (i + 14 <= src_len && memcmp(src + i, "<!--TABLERO-->", 14) == 0) {
            bp += snprintf(s_pagina + bp, sizeof(s_pagina) - bp, "%s%s", s_tabla, s_msg);
            i += 14;
        } else if (i + 3 <= src_len && memcmp(src + i, ">0<", 3) == 0) {
            bp += snprintf(s_pagina + bp, sizeof(s_pagina) - bp, ">%lu<", (unsigned long)seg);
            i += 3;
        } else {
            s_pagina[bp++] = src[i++];
        }
    }
    s_pagina[bp] = '\0';
}

static void armar_json(uint32_t seg, const Juego *j)
{
    const char *est = j->estado == GANADO ? "won" :
                      j->estado == SALIR  ? "quit" : "running";
    int pos = 0;
    pos += snprintf(s_json + pos, sizeof(s_json) - pos,
                    "{\"elapsed\":%lu,\"status\":\"%s\",\"board\":[",
                    (unsigned long)seg, est);
    for (int r = 0; r < TAM; r++) {
        pos += snprintf(s_json + pos, sizeof(s_json) - pos, "[");
        for (int c = 0; c < TAM; c++)
            pos += snprintf(s_json + pos, sizeof(s_json) - pos,
                            "{\"value\":%d,\"fixed\":%s}%s",
                            j->tablero[r][c].valor,
                            j->tablero[r][c].fija ? "true" : "false",
                            c < 8 ? "," : "");
        pos += snprintf(s_json + pos, sizeof(s_json) - pos, "]%s", r < 8 ? "," : "");
    }
    snprintf(s_json + pos, sizeof(s_json) - pos, "]}");
}

static int leer_int(const char *body, const char *key)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%s=", key);
    const char *p = strstr(body, buf);
    if (!p) return -1;
    return atoi(p + strlen(buf));
}

static esp_err_t handle_root(httpd_req_t *req)
{
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    uint32_t seg  = tiempo_transcurrido();
    Juego    snap = g_juego;
    xSemaphoreGive(g_mutex);

    armar_pagina(&snap, seg);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, s_pagina);
    return ESP_OK;
}

static esp_err_t handle_state(httpd_req_t *req)
{
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    uint32_t seg  = tiempo_transcurrido();
    Juego    snap = g_juego;
    xSemaphoreGive(g_mutex);

    armar_json(seg, &snap);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, s_json);
    return ESP_OK;
}

static esp_err_t handle_css(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/css");
    httpd_resp_send(req, (const char *)style_css_start,
                    (ssize_t)(style_css_end - style_css_start));
    return ESP_OK;
}

static esp_err_t handle_place(httpd_req_t *req)
{
    char body[128] = {0};
    httpd_req_recv(req, body, sizeof(body) - 1);

    int r = leer_int(body, "row") - 1;
    int c = leer_int(body, "col") - 1;
    int v = leer_int(body, "num");

    xSemaphoreTake(g_mutex, portMAX_DELAY);
    if (g_juego.estado == JUGANDO &&
        r >= 0 && r < 9 && c >= 0 && c < 9 && v >= 1 && v <= 9) {
        if (poner_numero(g_juego.tablero, r, c, v) == 0 && esta_resuelto(g_juego.tablero))
            g_juego.estado = GANADO;
    }
    xSemaphoreGive(g_mutex);

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handle_restart(httpd_req_t *req)
{
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    iniciar_tablero(g_juego.tablero, NUM_PISTAS);
    g_juego.estado = JUGANDO;
    g_juego.inicio = esp_timer_get_time();
    xSemaphoreGive(g_mutex);

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static void iniciar_servidor(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) return;

    const httpd_uri_t rutas[] = {
        { .uri = "/",          .method = HTTP_GET,  .handler = handle_root    },
        { .uri = "/state",     .method = HTTP_GET,  .handler = handle_state   },
        { .uri = "/style.css", .method = HTTP_GET,  .handler = handle_css     },
        { .uri = "/place",     .method = HTTP_POST, .handler = handle_place   },
        { .uri = "/restart",   .method = HTTP_POST, .handler = handle_restart },
    };

    for (int i = 0; i < 5; i++)
        httpd_register_uri_handler(server, &rutas[i]);

    ESP_LOGI(TAG, "servidor HTTP listo");
}

static void iniciar_wifi(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap = {
        .ap = {
            .ssid           = WIFI_SSID,
            .ssid_len       = strlen(WIFI_SSID),
            .channel        = WIFI_CHANNEL,
            .password       = WIFI_PASS,
            .max_connection = MAX_CONN,
            .authmode       = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi listo — SSID: %s  IP: 192.168.4.1", WIFI_SSID);
}

static void IRAM_ATTR isr_boton(void *arg)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(g_btn_sem, &woken);
    portYIELD_FROM_ISR(woken);
}

static void tarea_boton(void *arg)
{
    while (1) {
        if (xSemaphoreTake(g_btn_sem, portMAX_DELAY) == pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(50)); // debounce
            if (gpio_get_level(BTN_GPIO) == 0) {
                xSemaphoreTake(g_mutex, portMAX_DELAY);
                if (g_juego.estado == JUGANDO)
                    g_juego.estado = SALIR;
                xSemaphoreGive(g_mutex);
            }
        }
    }
}

static void iniciar_boton(void)
{
    g_btn_sem = xSemaphoreCreateBinary();

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BTN_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BTN_GPIO, isr_boton, NULL));
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

    iniciar_tablero(g_juego.tablero, NUM_PISTAS);
    g_juego.estado = JUGANDO;
    g_juego.inicio = esp_timer_get_time();

    iniciar_wifi();
    iniciar_servidor();
    iniciar_boton();

    xTaskCreate(tarea_boton, "boton", 2048, NULL, 5, NULL);

    ESP_LOGI(TAG, "listo -> %s  http://192.168.4.1", WIFI_SSID);
}