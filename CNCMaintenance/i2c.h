#pragma once

#include <stdbool.h>
#include "string.h"
#include <stdint.h>

#define LSM6DSO_ID         0x6C   // register value
#define LSM6DSO_ADDRESS	   0x6A	  // I2C Address

int initI2c(void);
int readTemp(void);
float tempC;