#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/i2c_master.h"
#include "driver/i2c_slave.h"
#include "driver/gpio.h"

#define ECHO_UART_PORT_NUM   UART_NUM_0
#define ECHO_UART_BAUD_RATE  115200
#define ECHO_TASK_STACK_SIZE 4096
#define BUF_SIZE             1024

#define BMP_SDA     21
#define BMP_SCL     22
#define BMP280_ADDR 0x77

#define SLAVE_SDA   18
#define SLAVE_SCL   19
#define SLAVE_ADDR  0x42

#define HEADER_REQ  0x1F
#define HEADER_RESP 0x2F
#define CMD         0x28

static i2c_master_bus_handle_t bmp_bus;
static i2c_master_dev_handle_t bmp_sensor;
static i2c_slave_dev_handle_t  slave_handle;
static QueueHandle_t           rx_queue;
static uint8_t                 rx_buf[8];
static uint16_t T1;
static int16_t  T2, T3;

void init_uart(void) {
    uart_config_t uart_config = {
        .baud_rate  = ECHO_UART_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    int intr_alloc_flags = 0;
#if CONFIG_UART_ISR_IN_IRAM
    intr_alloc_flags = ESP_INTR_FLAG_IRAM;
#endif
    ESP_ERROR_CHECK(uart_driver_install(ECHO_UART_PORT_NUM, BUF_SIZE * 2, 0, 0, NULL, intr_alloc_flags));
    ESP_ERROR_CHECK(uart_param_config(ECHO_UART_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(ECHO_UART_PORT_NUM, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

void puts_uart(uart_port_t uart_num, char *str) {
    uart_write_bytes(uart_num, str, strlen(str));
}

void float_to_str(float val, char *buf) {
    int entero  = (int)val;
    int decimal = (int)((val - entero) * 100);
    if (decimal < 0) decimal = -decimal;

    int i = 0;
    if (entero < 0) { buf[i++] = '-'; entero = -entero; }
    if (entero == 0) {
        buf[i++] = '0';
    } else {
        char tmp[8]; int t = 0;
        while (entero > 0) { tmp[t++] = '0' + (entero % 10); entero /= 10; }
        while (t > 0) buf[i++] = tmp[--t];
    }
    buf[i++] = '.';
    buf[i++] = '0' + (decimal / 10);
    buf[i++] = '0' + (decimal % 10);
    buf[i]   = '\0';
}

static bool IRAM_ATTR on_recv(i2c_slave_dev_handle_t handle,const i2c_slave_rx_done_event_data_t *edata,void *arg) {
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR((QueueHandle_t)arg, edata, &woken);
    return woken == pdTRUE;
}

static void bmp280_init(void) {
    i2c_master_bus_config_t b = {
        .i2c_port = I2C_NUM_0, 
        .sda_io_num = BMP_SDA, 
        .scl_io_num = BMP_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, 
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&b, &bmp_bus));
    i2c_device_config_t d = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BMP280_ADDR, .scl_speed_hz = 100000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bmp_bus, &d, &bmp_sensor));
    uint8_t reg = 0x88, cal[6];
    ESP_ERROR_CHECK(i2c_master_transmit_receive(bmp_sensor, &reg, 1, cal, 6, 1000));
    T1 = (cal[1]<<8)|cal[0]; T2 = (cal[3]<<8)|cal[2]; T3 = (cal[5]<<8)|cal[4];
    uint8_t cmd[2] = {0xF4, 0x27};
    ESP_ERROR_CHECK(i2c_master_transmit(bmp_sensor, cmd, 2, 1000));
    puts_uart(ECHO_UART_PORT_NUM, "\r\nBMP280 OK\r\n");
}

static float bmp280_read(void) {
    uint8_t reg = 0xFA, raw[3];
    if (i2c_master_transmit_receive(bmp_sensor, &reg, 1, raw, 3, 1000) != ESP_OK)
        return -999.0f;
    int32_t adc = ((int32_t)raw[0]<<12)|((int32_t)raw[1]<<4)|(raw[2]>>4);
    int32_t v1  = ((((adc>>3)-((int32_t)T1<<1)))*(int32_t)T2)>>11;
    int32_t v2  = (((((adc>>4)-(int32_t)T1)*((adc>>4)-(int32_t)T1))>>12)*(int32_t)T3)>>14;
    return (float)(((v1+v2)*5+128)>>8) / 100.0f;
}

static void slave_init_device(void) {
    i2c_slave_config_t cfg = {
        .i2c_port = I2C_NUM_1, 
        .sda_io_num = SLAVE_SDA, 
        .scl_io_num = SLAVE_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, 
        .send_buf_depth = 128,
        .slave_addr = SLAVE_ADDR, 
        .addr_bit_len = I2C_ADDR_BIT_LEN_7,
    };
    ESP_ERROR_CHECK(i2c_new_slave_device(&cfg, &slave_handle));
    gpio_set_pull_mode(SLAVE_SDA, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(SLAVE_SCL, GPIO_PULLUP_ONLY);
    i2c_slave_event_callbacks_t cbs = { .on_recv_done = on_recv };
    ESP_ERROR_CHECK(i2c_slave_register_event_callbacks(slave_handle, &cbs, rx_queue));
    ESP_ERROR_CHECK(i2c_slave_receive(slave_handle, rx_buf, 2));
}

static void tarea_slave(void *arg){
    puts_uart(ECHO_UART_PORT_NUM, "\r\nSlave listo\r\n");
    char temp_str[16];
    i2c_slave_rx_done_event_data_t evt;

    while (1) {
        xQueueReceive(rx_queue, &evt, portMAX_DELAY);

        i2c_slave_rx_done_event_data_t dummy;
        while (xQueueReceive(rx_queue, &dummy, 0) == pdTRUE) {}

        if (evt.buffer[0] == HEADER_REQ && evt.buffer[1] == CMD) {
            float temp = bmp280_read();
            float_to_str(temp, temp_str);
            puts_uart(ECHO_UART_PORT_NUM, "\r\n(SLAVE) Temperatura: ");
            puts_uart(ECHO_UART_PORT_NUM, temp_str);
            puts_uart(ECHO_UART_PORT_NUM, " C\r\n");

            uint8_t tx[6] = {HEADER_RESP, CMD};
            memcpy(&tx[2], &temp, sizeof(float));

            vTaskDelay(pdMS_TO_TICKS(300));
            esp_err_t err = i2c_slave_transmit(slave_handle, tx, sizeof(tx), 1000);

            if (err != ESP_OK) {
                puts_uart(ECHO_UART_PORT_NUM, "\r\nRecreando slave...\r\n");
                i2c_del_slave_device(slave_handle);
                vTaskDelay(pdMS_TO_TICKS(10));
                slave_init_device();
                continue;
            }
        }

        i2c_slave_receive(slave_handle, rx_buf, 2);
    }
}

void app_main(void) {
    init_uart();
    bmp280_init();

    rx_queue = xQueueCreate(4, sizeof(i2c_slave_rx_done_event_data_t));
    slave_init_device();
    xTaskCreate(tarea_slave, "tarea_slave", ECHO_TASK_STACK_SIZE, NULL, 10, NULL);
}