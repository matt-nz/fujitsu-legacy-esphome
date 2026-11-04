#pragma once
typedef int uart_port_t;
#define UART_PIN_NO_CHANGE (-1)
int uart_set_pin(uart_port_t port, int tx, int rx, int rts, int cts);
