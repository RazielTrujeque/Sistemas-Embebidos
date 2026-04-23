#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <inttypes.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_bt_device.h"
#include "esp_spp_api.h"

#define SPP_TAG         "SPP_ACCEPTOR_DEMO"
#define SPP_SERVER_NAME "SPP_SERVER"
#define MAX_LEN         256
#define MAX_SUB         16

static const char local_device_name[] = CONFIG_EXAMPLE_LOCAL_DEVICE_NAME;
static const esp_spp_mode_t esp_spp_mode = ESP_SPP_MODE_CB;
static const bool esp_spp_enable_l2cap_ertm = true;
static const esp_spp_sec_t sec_mask   = ESP_SPP_SEC_AUTHENTICATE;
static const esp_spp_role_t role_slave = ESP_SPP_ROLE_SLAVE;

static uint32_t spp_handle = 0;

typedef struct {
    char data[MAX_LEN];
    int  len;
} bt_msg_t;

static QueueHandle_t msg_queue = NULL;

static char *bda2str(uint8_t *bda, char *str, size_t size)
{
    if (!bda || !str || size < 18) return NULL;
    sprintf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
            bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    return str;
}

static void spp_send(const char *msg)
{
    if (!spp_handle) return;
    esp_spp_write(spp_handle, strlen(msg), (uint8_t *)msg);
}

/* Busca todas las subsecuencias palindrómicas de longitud >= 3
   usando bitmask. Deduplicación por hash FNV-1a. */
static void buscar_palindromos(const char *sub, int sub_len)
{
    if (sub_len > MAX_SUB) {
        spp_send("Error: subcadena muy larga (max 16 chars)\r\n");
        return;
    }

    char buf[64];
    char candidato[MAX_SUB + 1];
    uint32_t hashes[512];
    int count = 0;
    int total = 1 << sub_len;

    for (int mask = 1; mask < total; mask++) {

        /* Construye el candidato con los bits activos del mask */
        int clen = 0;
        for (int i = 0; i < sub_len; i++)
            if (mask & (1 << i))
                candidato[clen++] = sub[i];
        candidato[clen] = '\0';

        if (clen < 3) continue;

        /* Verifica si es palíndromo */
        bool ok = true;
        for (int l = 0, r = clen - 1; l < r; l++, r--)
            if (candidato[l] != candidato[r]) { ok = false; break; }
        if (!ok) continue;

        /* Hash FNV-1a para evitar duplicados sin usar memoria extra */
        uint32_t hash = 2166136261u;
        for (int i = 0; i < clen; i++) {
            hash ^= (uint8_t)candidato[i];
            hash *= 16777619u;
        }

        bool dup = false;
        for (int j = 0; j < count; j++)
            if (hashes[j] == hash) { dup = true; break; }
        if (dup) continue;

        if (count < 512) hashes[count++] = hash;

        memcpy(buf, candidato, clen);
        buf[clen]     = '\r';
        buf[clen + 1] = '\n';
        buf[clen + 2] = '\0';
        spp_send(buf);
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (count == 0)
        spp_send("No se encontraron palindromos.\r\n");
    else {
        snprintf(buf, sizeof(buf), "Total: %d palindromo(s)\r\n", count);
        spp_send(buf);
    }
}

/* Parsea el mensaje "<cadena>,<x>,<y>", extrae la subcadena y busca palíndromos */
static void procesar_mensaje(const char *raw, int raw_len)
{
    char msg[MAX_LEN];
    int len = (raw_len < MAX_LEN - 1) ? raw_len : MAX_LEN - 1;
    memcpy(msg, raw, len);
    msg[len] = '\0';

    /* Limpia saltos de línea y espacios */
    while (len > 0 && (msg[len-1] == '\r' || msg[len-1] == '\n' || msg[len-1] == ' '))
        msg[--len] = '\0';

    char *s = msg;
    while (*s == ' ') s++;

    /* Separa y,x,cadena buscando comas desde el final */
    char *c2 = strrchr(s, ',');
    if (!c2) { spp_send("Formato: cadena,x,y\r\n"); return; }
    *c2 = '\0';
    char *y_str = c2 + 1;

    char *c1 = strrchr(s, ',');
    if (!c1) { spp_send("Formato: cadena,x,y\r\n"); return; }
    *c1 = '\0';
    char *x_str = c1 + 1;
    char *cadena = s;

    int cadena_len = strlen(cadena);
    int x = atoi(x_str);
    int y = atoi(y_str);

    if (x < 0 || y < 0 || x >= cadena_len || y >= cadena_len || x > y) {
        char err[80];
        snprintf(err, sizeof(err), "Error: x=%d y=%d fuera de rango (len=%d)\r\n", x, y, cadena_len);
        spp_send(err);
        return;
    }

    int sub_len = y - x + 1;
    char sub[MAX_LEN];
    memcpy(sub, cadena + x, sub_len);
    sub[sub_len] = '\0';

    char info[MAX_LEN*2];
    snprintf(info, sizeof(info), "Subcadena [%d..%d]: %s\r\n", x, y, sub);
    spp_send(info);
    spp_send("Palindromos:\r\n");

    buscar_palindromos(sub, sub_len);
}

static void tarea_palindromos(void *pvParam)
{
    bt_msg_t msg;
    while (1)
        if (xQueueReceive(msg_queue, &msg, portMAX_DELAY) == pdTRUE)
            procesar_mensaje(msg.data, msg.len);
}

static void esp_spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
    char bda_str[18] = {0};

    switch (event) {
    case ESP_SPP_INIT_EVT:
        if (param->init.status == ESP_SPP_SUCCESS)
            esp_spp_start_srv(sec_mask, role_slave, 0, SPP_SERVER_NAME);
        break;
    case ESP_SPP_START_EVT:
        if (param->start.status == ESP_SPP_SUCCESS) {
            esp_bt_gap_set_device_name(local_device_name);
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
            ESP_LOGI(SPP_TAG, "Esperando conexion...");
        }
        break;
    case ESP_SPP_SRV_OPEN_EVT:
        spp_handle = param->srv_open.handle;
        ESP_LOGI(SPP_TAG, "Cliente conectado: [%s]",
                 bda2str(param->srv_open.rem_bda, bda_str, sizeof(bda_str)));
        spp_send("Conectado. Formato: cadena,x,y\r\n");
        break;
    case ESP_SPP_CLOSE_EVT:
        ESP_LOGI(SPP_TAG, "Cliente desconectado");
        spp_handle = 0;
        break;
    case ESP_SPP_DATA_IND_EVT: {
        int len = param->data_ind.len;
        if (len >= MAX_LEN) len = MAX_LEN - 1;
        bt_msg_t msg;
        memcpy(msg.data, param->data_ind.data, len);
        msg.data[len] = '\0';
        msg.len = len;
        if (xQueueSendFromISR(msg_queue, &msg, NULL) != pdTRUE)
            ESP_LOGW(SPP_TAG, "Cola llena");
        break;
    }
    default:
        break;
    }
}

void esp_bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS)
            ESP_LOGI(SPP_TAG, "Autenticacion exitosa: %s", param->auth_cmpl.device_name);
        else
            ESP_LOGE(SPP_TAG, "Autenticacion fallida: %d", param->auth_cmpl.stat);
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
        esp_bt_pin_code_t pin_code;
        if (param->pin_req.min_16_digit) {
            memset(pin_code, 0, sizeof(pin_code));
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 16, pin_code);
        } else {
            pin_code[0]='1'; pin_code[1]='2'; pin_code[2]='3'; pin_code[3]='4';
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
        }
        break;
    }
#if (CONFIG_EXAMPLE_SSP_ENABLED == true)
    case ESP_BT_GAP_CFM_REQ_EVT:
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
#endif
    default:
        break;
    }
}

void app_main(void)
{
    char bda_str[18] = {0};

    msg_queue = xQueueCreate(5, sizeof(bt_msg_t));
    xTaskCreatePinnedToCore(tarea_palindromos, "tarea_palindromos", 8192, NULL, 5, NULL, 0);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
#if (CONFIG_EXAMPLE_SSP_ENABLED == false)
    bluedroid_cfg.ssp_en = false;
#endif
    ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&bluedroid_cfg));
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    ESP_ERROR_CHECK(esp_bt_gap_register_callback(esp_bt_gap_cb));
    ESP_ERROR_CHECK(esp_spp_register_callback(esp_spp_cb));

    esp_spp_cfg_t bt_spp_cfg = {
        .mode = esp_spp_mode,
        .enable_l2cap_ertm = esp_spp_enable_l2cap_ertm,
        .tx_buffer_size = 0,
    };
    ESP_ERROR_CHECK(esp_spp_enhanced_init(&bt_spp_cfg));

#if (CONFIG_EXAMPLE_SSP_ENABLED == true)
    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));
#endif

    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
    esp_bt_pin_code_t pin_code;
    esp_bt_gap_set_pin(pin_type, 0, pin_code);

    ESP_LOGI(SPP_TAG, "Direccion: [%s]",
             bda2str((uint8_t *)esp_bt_dev_get_address(), bda_str, sizeof(bda_str)));
}