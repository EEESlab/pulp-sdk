/*
 * Copyright (C) 2019 GreenWaves Technologies
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Authors: Eric Flamand, GreenWaves Technologies (eric.flamand@greenwaves-technologies.com)
 *        Germain Haugou, GreenWaves Technologies (germain.haugou@greenwaves-technologies.com)
 */


#include "pmsis.h"


PI_FC_TINY int pos_fll_freq[ARCHI_NB_FLL];
static PI_FC_TINY char pos_fll_is_on[ARCHI_NB_FLL];

static unsigned int pos_fll_get_mult_div_from_freq(unsigned int freq, unsigned int *mult, unsigned int *div)
{
    unsigned int fref = ARCHI_REF_CLOCK;
    unsigned int Log2M = __FL1(freq) - __FL1(fref);
    unsigned int D = __MAX(1, (FLL_LOG2_MAXM - Log2M)>>1);
    unsigned int M = (freq<<D)/fref;
    unsigned int fres;

#if 0 //PULP_CHIP == CHIP_QUENTIN

  // TODO on quentin, the fll never converges (e.g. a mult factor of 15000 is
  // not working), check what is the maximum value we can use
  while (M >= 10000)
  {
    M >>= 1;
    D--;
  };

  if (D == 0) fres = fref*M;
  else fres = (fref*M + (1<<(D-1)))>>D;   /* Rounding */

#else

    fres = (fref*M + (1<<(D-1)))>>D;   /* Rounding */

#endif

    *mult = M; *div = D+1;

    return fres;
}

static unsigned int pos_fll_get_freq_from_mult_div(unsigned int mult, unsigned int div)
{
    // FreqOut = Fref * Mult/2^(Div-1)  With Mult on 16 bits and Div on 4 bits
    unsigned int fref = ARCHI_REF_CLOCK;
    unsigned int fres = (fref*mult)>>(div-1);
    return fres;
}



unsigned int pos_fll_set_freq(int fll, unsigned int frequency)
{
    FREQ_TRACE(POS_LOG_INFO, "Setting FLL frequency (fll: %d, frequency: %d)\n", fll, frequency);

    unsigned int real_freq, mult, div;
    real_freq = pos_fll_get_mult_div_from_freq(frequency, &mult, &div);
   
    pos_fll_freq[fll] = real_freq;
    if (pos_fll_is_on[fll])
    {
        hal_fll_update_mult_div(fll, mult, div);
    }

    return real_freq;
}



void pos_freq_wait_convergence(int fll)
{
    int irq = hal_irq_disable();
    int mult_factor_diff;
    fll_reg_conf1_t fll_conf1;
    fll_reg_conf2_t fll_conf2;
    fll_conf1.raw = hal_fll_conf_reg1_get(fll);
    fll_conf2.raw = hal_fll_conf_reg2_get(fll);
    int mult = fll_conf1.mult_factor;
    int tolerance = fll_conf2.lock_tolerance;

    do {
        mult_factor_diff = hal_fll_status_reg_get(fll) - mult;
        if (mult_factor_diff < 0)
            mult_factor_diff = -mult_factor_diff;

        if ( mult_factor_diff <= tolerance)
            break;

        pos_irq_mask_set(1<<ARCHI_FC_EVT_CLK_REF);
        pos_irq_wait_for_interrupt();
        pos_irq_mask_clr(1<<ARCHI_FC_EVT_CLK_REF);
    } while (1);

    hal_irq_restore(irq);
}



unsigned int pos_fll_init(int fll)
{
    FREQ_TRACE(POS_LOG_INFO, "Initializing FLL (fll: %d)\n", fll);
    # if __PLATFORM__ != ARCHI_PLATFORM_FPGA
    fll_reg_conf1_t reg1 = { .raw = hal_fll_conf_reg1_get(fll) };

    // Only lock the fll if it is not already done by the boot code
    if (reg1.mode == 0)
    {
        /* Set Clock Ref lock assert count */
        fll_reg_conf2_t reg2 = { .raw = hal_fll_conf_reg2_get(fll) };
        reg2.assert_cycles = 6;
        reg2.lock_tolerance = 0x50;
        hal_fll_conf_reg2_set(fll, reg2.raw);

        // In order to lock the fll faster, we first setup an approximated
        // frequency while we are in open loop as it is set immediately.
        // Then starting from this frequency, we should reach the target one
        // in lock mode faster.
        /* We are in open loop, prime the fll forcing dco input, approx 70 MHz */
        FREQ_TRACE(POS_LOG_INFO, "Priming FLL in open loop (fll: %d)\n", fll);
        fll_reg_integrator_t reg_int = { .raw = hal_fll_integrator_get(fll) };

        reg_int.state_int_part = 332;

        hal_fll_integrator_set(fll, reg_int.raw);

        /* Lock Fll */
        reg1.output_lock_enable = 1;
        reg1.mode = 1;
        hal_fll_conf_reg1_set(fll, reg1.raw);
    }

    // In case the FLL frequency was set while it was off, update it immediately
    unsigned int freq = pos_fll_freq[fll];
    if (freq)
    {
        pos_fll_set_freq(fll, freq);
    }
    else
    {
        freq = pos_fll_get_freq_from_mult_div(reg1.mult_factor, reg1.clock_out_divider);
        pos_fll_freq[fll] = freq;
    }

    pos_fll_is_on[fll] = 1;

    FREQ_TRACE(POS_LOG_INFO, "FLL is locked (fll: %d, freq: %d)\n", fll, freq);

    return freq;
    # else
    // on FPGA, we have a number of clock generators which are all configured to run at the same frequency
    pos_fll_is_on[fll] = 1;
    return ARCHI_FPGA_FREQUENCY;
    # endif
}



void pos_fll_deinit(int fll)
{
    // Just remember the FLL is off. Keep the same frequency so that we can restore the FLL
    // with the same frequency later on
    pos_fll_is_on[fll] = 0;

    // TODO this should shutdown FLL
}



void pos_fll_constructor()
{
    for (int i=0; i<ARCHI_NB_FLL; i++)
    {
        pos_fll_freq[i] = 0;
        pos_fll_is_on[i] = 0;
    }
}
