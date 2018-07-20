#include <FreeRTOS.h>
#include "colorLed.h"
#include "multipwm.h"

static uint8_t pins[] = {12, 15, 13};  //R G B pins
static pwm_info_t pwm_info;

bool started = true;

void blinkTest(void *pvParameters)
{
    for (uint8_t i = 0; i < 256; i++) {
    	set_led(0, i, i);
    	vTaskDelay( 100 / portTICK_PERIOD_MS );
    }
}

void init_led(void) {
	printf("init led...\r\n");
    pwm_info.channels = 3;
    multipwm_init(&pwm_info);
    for (uint8_t ii=0; ii<pwm_info.channels; ii++) {
        multipwm_set_pin(&pwm_info, ii, pins[ii]);
    }
    multipwm_start(&pwm_info);
    multipwm_set_duty_all(&pwm_info, MULTIPWM_MAX_PERIOD);
	printf("init led finished...\r\n");
}

void close_led(void) {
	started = false;
	multipwm_stop(&pwm_info);
	gpio_disable(12);
	gpio_disable(15);
	gpio_disable(13);
}	

void set_led(uint8_t r, uint8_t g, uint8_t b) {
	if (!started) {
		multipwm_init(&pwm_info);
	    for (uint8_t ii=0; ii<pwm_info.channels; ii++) {
	        multipwm_set_pin(&pwm_info, ii, pins[ii]);
	    }
	    multipwm_start(&pwm_info);
	    started = true;
	}
	//printf("set....r: %d g: %d b: %d\r\n", r, g, b);
	for (uint8_t i=0; i<pwm_info.channels; i++) {
		uint16_t value = MULTIPWM_MAX_PERIOD; 
		if (i == 0) {
			value -= r*MULTIPWM_MAX_PERIOD/255;
		} else if (i == 1) {
			value -= g*MULTIPWM_MAX_PERIOD/255;
		} else if (i == 2) {
			value -= b*MULTIPWM_MAX_PERIOD/255;
		}
	//	printf("channel: %d value: %d \r\n", i, value);
        multipwm_set_duty(&pwm_info, i, value);
    }
}




