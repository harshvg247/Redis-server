#ifndef TIME_UTILS_H
#define TIME_UTILS_H

#include <sys/time.h>

long long current_time_ms(){
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return ((long long)tv.tv_sec * 1000) + (tv.tv_usec / 1000);
}

#endif