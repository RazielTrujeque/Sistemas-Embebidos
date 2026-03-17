#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/i2c_master.h"

#define ECHO_UART_PORT_NUM   UART_NUM_0
#define ECHO_TASK_STACK_SIZE 4096
#define BUF_SIZE             1024

#define MASTER_SDA  21
#define MASTER_SCL  22
#define SLAVE_ADDR  0x42

#define HEADER_REQ  0x1F
#define HEADER_RESP 0x2F
#define CMD         0x28
#define MAX_RETRIES 2

static i2c_master_bus_handle_t master_bus;
static i2c_master_dev_handle_t slave_dev;


void init_uart(void) {
    uart_config_t uart_config = {
        .baud_rate  = 115200,
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

void init_i2c_bus(void){  
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = I2C_NUM_0,
        .sda_io_num        = MASTER_SDA,
        .scl_io_num        = MASTER_SCL,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &master_bus));
}

void init_i2c_slave(void){
    
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = SLAVE_ADDR,
        .scl_speed_hz    = 100000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(master_bus, &dev_cfg, &slave_dev));

}


void putchar_uart(uart_port_t uart_num, char c) {
    uart_write_bytes(uart_num, &c, 1);
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

static void master_task(void *arg) {
    uint8_t tx[2] = {HEADER_REQ, CMD};
    uint8_t rx[6];
    char temp_str[16];

    puts_uart(ECHO_UART_PORT_NUM, "\r\nMaster listo\r\n");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        bool ok = false;

        for (int i = 0; i <= MAX_RETRIES; i++) {
            if (i > 0) {
                puts_uart(ECHO_UART_PORT_NUM, "\r\nReintento...\r\n");
                i2c_master_bus_reset(master_bus);
                vTaskDelay(pdMS_TO_TICKS(100));
            }

            // 1. Enviar solicitud
            if (i2c_master_transmit(slave_dev, tx, sizeof(tx), 500) != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }

            // 2. Esperar al slave
            vTaskDelay(pdMS_TO_TICKS(200));

            // 3. Leer respuesta
            if (i2c_master_receive(slave_dev, rx, sizeof(rx), 500) == ESP_OK
                && rx[0] == HEADER_RESP && rx[1] == CMD) {
                float temp;
                memcpy(&temp, &rx[2], sizeof(float));
                float_to_str(temp, temp_str);
                puts_uart(ECHO_UART_PORT_NUM, "\r\n(MASTER) Temperatura: ");
                puts_uart(ECHO_UART_PORT_NUM, temp_str);
                puts_uart(ECHO_UART_PORT_NUM, " C\r\n");
                ok = true;
                break;
            }

            puts_uart(ECHO_UART_PORT_NUM, "\r\nRespuesta invalida o timeout\r\n");
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        if (!ok) {
            puts_uart(ECHO_UART_PORT_NUM, "\r\nComunicacion terminada, el periferico no responde\r\n");
            while (1) vTaskDelay(pdMS_TO_TICKS(10000));
        }
    }
}

void app_main(void) {
    init_uart();
    init_i2c_bus();
    init_i2c_slave();
    xTaskCreate(master_task, "master_task", 115200, NULL, 10, NULL);
}