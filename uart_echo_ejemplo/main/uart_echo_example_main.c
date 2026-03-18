#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "esp_log.h"

#define UART_PORT_NUM UART_NUM_0
#define BUF_SIZE 1024

// Pines por defecto (Wokwi)
#define ECHO_TEST_TXD (UART_PIN_NO_CHANGE)
#define ECHO_TEST_RXD (UART_PIN_NO_CHANGE)
#define ECHO_TEST_RTS (UART_PIN_NO_CHANGE)
#define ECHO_TEST_CTS (UART_PIN_NO_CHANGE)

//init UART
void init_uart(void){
  uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    int intr_alloc_flags = 0;

  uart_driver_install(UART_PORT_NUM, BUF_SIZE * 2, 0, 0, NULL, intr_alloc_flags);
  uart_param_config(UART_PORT_NUM, &uart_config);
  uart_set_pin(UART_PORT_NUM, ECHO_TEST_TXD, ECHO_TEST_RXD, ECHO_TEST_RTS, ECHO_TEST_CTS);

}

int gets_uart(char *buffer, int max_len){
  int pos = 0;

  while(pos < (max_len-1)){

  int len = uart_read_bytes(UART_PORT_NUM, &buffer[pos], 1, portMAX_DELAY);
    if(len > 0){

    uart_write_bytes(UART_PORT_NUM, &buffer[pos],1);

    if(buffer[pos] == '\n'){
      buffer[pos] = '\0';

      uart_write_bytes(UART_PORT_NUM, "\r\n",2);
      
      return pos;
    }
    pos++;
    }
  }
  return pos;
}

void puts_uart(char *arg){
  uart_write_bytes(UART_PORT_NUM, arg, strlen(arg));
}

void sort(uint8_t *arg, int n){
  for(int i = 0; i < n-1; i++){
    for(int j = 0; j < n-1; j++){
      if (arg[j] > arg[j+1]){
        uint8_t temp = arg[j];

        arg[j] = arg[j+1];
        arg[j+1] = temp;
      } 
    }
  }
}

void tarea_ordenar(void *arg){
  // 1. Biffer para leer lo que escribe el usuario
  char input[100];

  // Buffer auxiliar para usar sprintf y dar formato a mensajes de salida
  char msg[100];

  while (1){
    char* msg1 = ("\nIngrese el tamaño del arreglo: ");
    uart_write_bytes(UART_PORT_NUM, msg1, strlen(msg1));

    gets_uart(input, 100);
    
    int n = atoi(input);

    if( n <= 1 || n > 50){
      const char* err = "Error: Tamaño invaido (2-50)\n";
      uart_write_bytes(UART_PORT_NUM, err, strlen(err));
      continue;
    }

    uint8_t *numeros = (uint8_t *)malloc(n);

        
    for(int i = 0; i < n; i++){
      
      const char* msg2 = "\nIngresa un numero: ";
      uart_write_bytes(UART_PORT_NUM, msg2, strlen(msg2));

      gets_uart(input,100);
      numeros[i] = (uint8_t)atoi(input);
    }
    sort(numeros,n);

    for(int i = 0; i < n; i++){
      sprintf(msg, "[%d], ", numeros[i]);
      puts_uart(msg);
      

    }
    puts_uart("\n");
    // Liberamos para no quedarnos sin RAM en este bucle de prueba
    free(numeros);
  }
}


void tarea_echo_simple(void *arg) {
    uint8_t data[BUF_SIZE]; // Buffer para recibir datos
    
    const char* inicio_msg = "--- UART Echo Iniciado (Escribe algo) ---\r\n";
    uart_write_bytes(UART_PORT_NUM, inicio_msg, strlen(inicio_msg));

    while (1) {
        // Leemos datos del puerto UART (64ms de espera para no saturar el CPU)
        int len = uart_read_bytes(UART_PORT_NUM, data, BUF_SIZE, 20 / portTICK_PERIOD_MS);
        
        if (len > 0) {
            // Escribimos de vuelta exactamente lo que recibimos
            uart_write_bytes(UART_PORT_NUM, (const char *)data, len);
            
            // Opcional: Log en la consola de depuración de VS Code
            ESP_LOGI("UART", "Recibido y reenviado: %d bytes", len);
        }
    }
}

void app_main(void) {
    init_uart();
    // Creamos la tarea de echo con prioridad 10
    xTaskCreate(tarea_echo_simple, "tarea_echo", 4096, NULL, 10, NULL);
}