#pragma once
#include <stdio.h>
#define ESP_LOGI(tag, fmt, ...) printf("%s: " fmt "\n", tag, ##__VA_ARGS__)
