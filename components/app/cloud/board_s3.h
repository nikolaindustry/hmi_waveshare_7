#pragma once

#include "hyperwisor_port.h"

/* Waveshare ESP32-S3-Touch-LCD-7 board port descriptor.
 * Defined in board_s3.c; call hyperwisor_set_port(&board_port_s3)
 * before hyperwisor_init(). */
extern const hyperwisor_port_t board_port_s3;
