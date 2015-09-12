#

#include "general.h"

#include "assert.h"
#include <string.h>
#include <comedi.h>

#include "lut.h"

/*
 * Construct a look up table to map the USBDUXfast ADC outputs into 1V
 * pk s16 representation.
 *
 * The raw data goes from 000 to FFF, or-ed with 0x1000 if an overflow occurs.
 * The table is indexed with the raw data value to generate the s16 value.
 *
 * In the case of overflow, the value converted is 1 more than the
 * maximum raw value returnable by the ADC.
 *
 * There is one table for when the USBduxFAST is in 0.5V scale mode
 * and one for 0.75V scale.
 */

#define	CONVERT_BY_LUT		/* Use a lookup table to do the complete conversion from raw to normalised */

#define ADC_BITS	12

#define	USBDUXFAST_OOR	(1<<ADC_BITS)
#define USBDUXFAST_SIGN	(1 << (ADC_BITS-1))
#define USBDUXRAW_MIN	0
#define USBDUXRAW_MAX	(~((~0)<<ADC_BITS))

#define	RAW_500mV_TO_OUT_500mV(raw)	(((short)(((raw)<<4) ^ 0x8000)) >> 1)       /* Shift up and correct sign bit, then arithmetic shift back 1 */
#define	OUT_500mV_TO_OUT_750mV(raw)	((raw)+(short)((raw) >> 1))                 /* Add 0.5 times value you first thought of... */

#define USBDUXFAST_OOR_POS_500mV	(RAW_500mV_TO_OUT_500mV(USBDUXRAW_MAX)+1)
#define USBDUXFAST_OOR_NEG_500mV	(RAW_500mV_TO_OUT_500mV(USBDUXRAW_MIN)-1)
#define USBDUXFAST_OOR_POS_750mV	(OUT_500mV_TO_OUT_750mV(RAW_500mV_TO_OUT_500mV(USBDUXRAW_MAX))+1)
#define USBDUXFAST_OOR_NEG_750mV	(OUT_500mV_TO_OUT_750mV(RAW_500mV_TO_OUT_500mV(USBDUXRAW_MIN))-1)

/* Define the look-up tables for the conversion */
/* Using LUT only doubles the table size (but probably saves some time) */
#ifdef	CONVERT_BY_LUT
#define	TABLE_SIZE	(2*(1<<ADC_BITS))
#else
#define	TABLE_SIZE	(1<<ADC_BITS)
#endif

private sampl_t lut_raw_to_1Vpk_500mV[TABLE_SIZE];
private sampl_t lut_raw_to_1Vpk_750mV[TABLE_SIZE];

private int lut_not_ready = 1;

public void populate_conversion_luts() {
  short raw;

  assertv(sizeof(sampl_t) == 2, "sizeof(sampl_t) is %d not 2\n", sizeof(sampl_t));	/* Check type definitions on this architecture */
  assertv(RAW_500mV_TO_OUT_500mV(USBDUXRAW_MAX) > 0, "ADC mapped max not positive\n");	/* Should work if sampl_t is signed short */
  assertv(RAW_500mV_TO_OUT_500mV(USBDUXRAW_MIN) < 0, "ADC mapped min not negative\n");

  if( !lut_not_ready )		/* i.e. the tables are already ready */
    return;

  for(raw=0; raw<=0xFFF; raw++) {
    short conv = RAW_500mV_TO_OUT_500mV(raw);

    lut_raw_to_1Vpk_500mV[raw] = conv;			   /* Raw value maps to itself x 8 with sign corrected */
    lut_raw_to_1Vpk_750mV[raw] = OUT_500mV_TO_OUT_750mV(conv);  /* Values in 0.75pk range are scaled by 1.5 */
#ifdef CONVERT_BY_LUT
    lut_raw_to_1Vpk_500mV[raw+0x1000] = (raw&0x800)? USBDUXFAST_OOR_POS_500mV : USBDUXFAST_OOR_NEG_500mV;
    lut_raw_to_1Vpk_750mV[raw+0x1000] = (raw&0x800)? USBDUXFAST_OOR_POS_750mV : USBDUXFAST_OOR_NEG_750mV;
#endif
  }
  lut_not_ready = 0;		/* The tables are ready now... */
}

public void convert_raw_500mV(sampl_t *dst, sampl_t *src, int nsamples) {
  if(lut_not_ready)
    populate_conversion_luts();

  while(nsamples-- > 0) {
#ifdef	CONVERT_BY_LUT
    *dst++ = lut_raw_to_1Vpk_500mV[*src++ & (USBDUXFAST_OOR | USBDUXRAW_MAX)];
#else
    sampl_t s = *src++ & (USBDUXFAST_OOR | USBDUXRAW_MAX);
    if(s&USBDUXFAST_OOR) {
      *dst++ = (s&0x800)? USBDUXFAST_OOR_POS_500mV : USBDUXFAST_OOR_NEG_500mV;
      continue;
    }
    else
      *dst++ = lut_raw_to_1Vpk_500mV[*src++];
#endif
  }
}

public void convert_raw_750mV(sampl_t *dst, sampl_t *src, int nsamples) {
  if(lut_not_ready)
    populate_conversion_luts();

  while(nsamples-- > 0) {
#ifdef	CONVERT_BY_LUT
    *dst++ = lut_raw_to_1Vpk_750mV[*src++ & (USBDUXFAST_OOR | USBDUXRAW_MAX)];
#else
    sampl_t s = *src++ & (USBDUXFAST_OOR | USBDUXRAW_MAX);
    if(s&USBDUXFAST_OOR) {
      *dst++ = (s&0x800)? USBDUXFAST_OOR_POS_750mV : USBDUXFAST_OOR_NEG_750mV;
      continue;
    }
    else
      *dst++ = lut_raw_to_1Vpk_500mV[*src++];
#endif
  }
}

public void convert_raw_raw(sampl_t *dst, sampl_t *src, int nsamples) {
  if(dst == src)
    return;
  memcpy(dst, src, nsamples*sizeof(sampl_t));
}
