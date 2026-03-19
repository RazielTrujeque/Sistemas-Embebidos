#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#define LED GPIO_NUM_16
#define BOTON GPIO_NUM_17

void init_gpio() {
    gpio_reset_pin(LED);
    gpio_set_direction(LED, GPIO_MODE_OUTPUT);

    gpio_reset_pin(BOTON);
    gpio_set_direction(BOTON, GPIO_MODE_INPUT);
    
    gpio_set_pull_mode(BOTON, GPIO_PULLDOWN_ONLY); // El pin será 0 por defecto
    // Tu if actual (estado == 1) funcionará perfecto aquí.
}

void task_led(void *arg) {
    while(1) {
        int estado = gpio_get_level(BOTON);

        // Si el estado es 1 (porque presionaste y entró 3.3V)
        if(estado == 1) { 
            gpio_set_level(LED, 1); // ENCIENDE
        } else {
            gpio_set_level(LED, 0); // APAGA
        }
        vTaskDelay(pdMS_TO_TICKS(20)); 
    }
}

void app_main() {
    init_gpio();
    // Prioridad 5 es suficiente para esta tarea sencilla
    xTaskCreate(task_led, "task_led", 2048, NULL, 5, NULL);

    while(1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}