/* Host test: run lhbs params.c/modes.c against a real EEPROM image. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../src/params.h"
#include "../src/modes.h"
#include "../src/iap.h"

static uint8_t ee[4096];
uint32_t iap_eeprom_read(uint32_t a, void *d, uint32_t n) { memcpy(d, ee + a, n); return 0; }
uint32_t iap_eeprom_write(uint32_t a, const void *s, uint32_t n) { memcpy(ee + a, s, n); return 0; }

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb"); fread(ee, 1, sizeof ee, f); fclose(f);
    printf("payload size %u (stock 551)\n", params_payload_size());
    int part; cp_result_t rc = params_load(&part);
    printf("load rc=%d partition=%d name=%s mode.current=%u count=%u label=%c matches=%d\n",
           rc, part, P.sys_name, P.mode_current, P.mode_count, mode_label(), mode_matches_live());
    printf("fcal.0.phase=%g rotor.0.pwm=%u ootx.j0=%u\n", P.fcal_0_phase, P.rotor_0_pwm, P.ootx_j0);
    /* save unchanged params -> must land in 0x300 (oldest user) as version 128 */
    rc = params_save(&part);
    printf("save rc=%d partition=%d\n", rc, part);
    f = fopen(argv[2], "wb"); fwrite(ee, 1, 4032, f); fclose(f);
    /* press button: c -> A */
    mode_next();
    printf("after mode_next: current=%u label=%c timebase.mode=%u offset=%d\n",
           P.mode_current, mode_label(), P.timebase_mode, P.timebase_offset);
    f = fopen(argv[3], "wb"); fwrite(ee, 1, 4032, f); fclose(f);
    return 0;
}
