/* Independent full-trellis oracle: compare every survivor and relative metric
 * against the production chunk-normalized decoder, including startup and ties. */
#include <stdio.h>
#include "../components/psk_rtty/psk_rtty.c"
static int16_t oracle[2][2][VIT_MAX_STATES];
static uint8_t expected_decisions[4096];
static unsigned parity(unsigned x) { unsigned p=0; while(x){p^=x&1; x>>=1;} return p; }
int main(void)
{
    const psk_rtty_mode_t modes[] = {PSK_RTTY_MODE_QPSK125, PSK_RTTY_MODE_BPSK63F, PSK_RTTY_MODE_8PSK500F,
        PSK_RTTY_MODE_8PSK125F, PSK_RTTY_MODE_8PSK250F, PSK_RTTY_MODE_2X8PSK1200F};
    uint32_t random=0x71548aac;
    const unsigned depths[] = {1, 32, 384, 512, 640};
    for (unsigned d = 0; d < sizeof(depths)/sizeof(depths[0]); ++d) {
        psk_rtty_rx_t rx = {.inlv_depth=depths[d], .inlv_len=depths[d]*4};
        uint8_t cascade[2560], delay[2560];
        memset(cascade, 128, sizeof(cascade)); memset(delay,128,sizeof(delay));
        fec_branch_state_t branch = {.inlv_table=delay};
        for (unsigned step = 0; step < depths[d]*5+17; ++step) {
            random^=random<<13;random^=random>>17;random^=random<<5;
            uint8_t want[2]={(uint8_t)random,(uint8_t)(random>>8)}, actual[2];
            memcpy(actual,want,2);
            for (unsigned k=0;k<depths[d];++k) {
                cascade[k*4]=cascade[k*4+1];cascade[k*4+1]=want[0];
                cascade[k*4+2]=cascade[k*4+3];cascade[k*4+3]=want[1];
                want[0]=cascade[k*4];want[1]=cascade[k*4+3];
            }
            fec_deinterleave_pair(&rx,&branch,actual);
            if(memcmp(want,actual,2)){fprintf(stderr,"deinterleaver mismatch\n");return 1;}
        }
        printf("PASS deinterleaver depth=%u: startup and five wraps\n",depths[d]);
    }
    for (unsigned m=0;m<sizeof(modes)/sizeof(modes[0]);++m) {
        psk_rtty_rx_t rx;
        psk_rtty_config_t config={.mode=modes[m],.sample_rate_hz=48000,.rx_tone_hz=2000};
        psk_rtty_rx_init(&rx,&config);
        reset_fec_branch(&rx,0);
        const unsigned n=1U<<(rx.fec_k-1);
        memset(oracle,0,sizeof(oracle));
        for(unsigned step=0;step<1024;++step) {
          for(unsigned which=0;which<(rx.fec_dual_branch?2U:1U);++which) {
            if(step==512) {
                reset_fec_branch(&rx,which);
                memset(oracle[which],0,sizeof(oracle[which]));
            }
            random^=random<<13;random^=random>>17;random^=random<<5;
            const uint8_t b0=step<8?128:(uint8_t)random, b1=step<8?128:(uint8_t)(random>>8);
            const int a=(int)b0-128,b=(int)b1-128;
            const int16_t *prev=oracle[which][step&1];int16_t *curr=oracle[which][(step+1)&1];
            int best=-32768;
            memset(expected_decisions,0,sizeof(expected_decisions));
            for(unsigned state=0;state<n;++state) {
                int paths[2];
                for(unsigned p=0;p<2;++p) {
                    unsigned full=state+p*n;
                    paths[p]=prev[full>>1]+(parity(full&rx.fec_poly1)?a:-a)+
                        (parity(full&rx.fec_poly2)?b:-b);
                }
                unsigned take1=paths[1]>=paths[0];
                curr[state]=(int16_t)paths[take1];
                expected_decisions[state/8]|=take1<<(state%8);
                if(curr[state]>best)best=curr[state];
            }
            if(best>12000)for(unsigned i=0;i<n;++i)curr[i]-=best;
            uint8_t bits;int metric;
            fec_branch_state_t *branch=&s_fec_branch[which];
            const unsigned row=branch->vit_step;
            fec_decode_pair(&rx,branch,b0,b1,&bits,&metric);
            const int16_t *actual=branch->metrics_a;
            const int common=actual[0]-curr[0];
            for(unsigned i=0;i<n;++i) if(actual[i]-curr[i]!=common) {
                fprintf(stderr,"metric mismatch K=%u step=%u state=%u\n",rx.fec_k,step,i);return 1;
            }
            if(memcmp(branch->decisions + row * branch->decision_stride,expected_decisions,n/8)) {
                fprintf(stderr,"survivor mismatch K=%u step=%u\n",rx.fec_k,step);return 1;
            }
        }
          }
        printf("PASS K=%u mode=%u: 1024 steps, %u states/step, all metrics and decisions\n",rx.fec_k,modes[m],n);
    }
    return 0;
}
