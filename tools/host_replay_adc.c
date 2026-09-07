/* Replay the CRC-verified, already filtered mono PCM produced by joint_capture.
 * No second DC/FIR filter: these are the exact samples fed to the board modem. */
#include "psk_rx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char **argv)
{
    if(argc<4){fprintf(stderr,"usage: MODE adc.wav decoded.raw [tone]\n");return 2;}
    psk_mode_t mode=PSK_MODE_COUNT;
    for(size_t i=0;i<psk_mode_count();++i) {
        const psk_mode_info_t *info=psk_mode_info_at(i);
        if(!strcmp(argv[1],info->name))mode=info->id;
    }
    if(mode==PSK_MODE_COUNT)return 2;
    FILE *input=fopen(argv[2],"rb"),*output=fopen(argv[3],"wb");
    if(!input||!output)return 2;
    unsigned char header[44];
    if(fread(header,1,44,input)!=44||memcmp(header,"RIFF",4)||memcmp(header+36,"data",4)||
       header[22]!=1||header[24]!=0x80||header[25]!=0xbb||header[34]!=16)return 2;
    psk_rx_t rx;
    psk_rx_config_t config={.mode=mode,.sample_rate_hz=48000,.rx_tone_hz=argc>4?atoi(argv[4]):2000,
        .squelch=35,.afc_enabled=1,.squelch_enabled=1};
    psk_rx_init(&rx,&config);
    int16_t pcm[256];char chunk[256];size_t total=0,n,samples=0;
    while((n=fread(pcm,sizeof(*pcm),256,input))) {
        const size_t len=psk_rx_process(&rx,pcm,n,chunk,sizeof(chunk));
        fwrite(chunk,1,len,output);total+=len;samples+=n;
    }
    psk_rx_debug_t d;psk_rx_debug_snapshot(&rx,&d);
    printf("mode=%s samples=%zu bytes=%zu\n",argv[1],samples,total);
    fclose(input);fclose(output);return 0;
}
