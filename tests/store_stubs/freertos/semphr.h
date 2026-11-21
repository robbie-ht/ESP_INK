#ifndef TEST_SEMPHR_H
#define TEST_SEMPHR_H
typedef void *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t lock, unsigned ticks);
int xSemaphoreGive(SemaphoreHandle_t lock);
#endif
