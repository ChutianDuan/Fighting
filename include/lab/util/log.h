#pragma once
#include <cstdio>

#define LOGI(fmt, ...) std::printf("[I] " fmt "\n" __VA_OPT__(,) __VA_ARGS__)
#define LOGW(fmt, ...) std::printf("[W] " fmt "\n" __VA_OPT__(,) __VA_ARGS__)
#define LOGE(fmt, ...) std::printf("[E] " fmt "\n" __VA_OPT__(,) __VA_ARGS__)
