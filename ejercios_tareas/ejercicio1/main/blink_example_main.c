#include <stdio.h>
#include "driver/i2c.h"
#include "esp_log.h"

#define I2C_MASTER_SCL_IO 22
#define I2C_MASTER_SDA_IO 21
#define I2C_MASTER_NUM I2C_NUM_0

void app_main(void) {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SCL_IO,
        .scl_io_num = I2C_MASTER_SCL_IO, // Error común: ¡asegúrate que sean 21 y 22!
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    conf.scl_io_num = I2C_MASTER_SCL_IO;
    conf.sda_io_num = I2C_MASTER_SDA_IO;
    
    i2c_param_config(I2C_MASTER_NUM, &conf);
    i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);

    printf("Escaneando bus I2C...\n");
    for (int i = 3; i < 127; i++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (i << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        if (i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(10)) == ESP_OK) {
            printf("¡Pantalla encontrada en dirección: 0x%02X!\n", i);
        }
        i2c_cmd_link_delete(cmd);
    }
}