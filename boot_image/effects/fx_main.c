/* 
 * This file is licensed under the terms of the GNU General Public License
 * version 2.  This program  is licensed "as is" without any warranty of any
 * kind, whether express or implied.
 */


#include "stdint.h"
#include "serial.h"
#include "lradc.h"
#include "system.h"
#include "audio_dma.h"


#define LRADC_CHANNEL 4

static uint8_t g_print_cnt;


static void modify_buffers(
    int out_buff[],
    int in_buff[],
    unsigned int num_samples,
    unsigned int num_channels
)
{
    int i;
    int min, max, curr;

    // TODO: remove this min-max computation and print

    max = -0x7fffffff;
    min = 0x7fffffff;

    for (i=0; i<num_samples; i+=2) {
        curr = in_buff[i];
        if (curr > max) max=curr;
        if (curr < min) min=curr;
    }
    g_print_cnt++;
    if ((g_print_cnt & 0x1f) == 0) {
        serial_puts("recording period elapsed. min=");
        serial_puthex(min);
        serial_puts(", max=");
        serial_puthex(max);
        serial_puts("\n");
    }

    // copy the buffer
    for (i=0; i < num_samples*num_channels; ++i) {
        out_buff[i] = in_buff[i];
    }
}

int fx_main()
{
    unsigned int tmp;

    system_init();
    lradc_setup_channel_for_polling(LRADC_CHANNEL);
    audio_setup();
    audio_dma_init(modify_buffers);

    g_print_cnt = 0;

    serial_puts("initialisations complete\n");

    audio_dma_start();

    serial_puts("\n");

    while(1) {
        tmp = lradc_read_channel(LRADC_CHANNEL);
        serial_puts("lradc data: ");
        serial_puthex(tmp);
        serial_puts("\n");

        udelay(500000);
    }
    return 0;
}

