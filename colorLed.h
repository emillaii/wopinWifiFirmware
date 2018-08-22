#include <espressif/esp_common.h>

void init_led(void); 
void close_led(void);
void close_key_led(void);
void set_led(uint8_t r, uint8_t g, uint8_t b);
void set_key_led(uint8_t r, uint8_t g, uint8_t b);