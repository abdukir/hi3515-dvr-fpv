/* our_dvr.c — our OWN DVR recorder for the RR104P / Hi3515, built with the MPP SDK.
 *
 * Owns the whole pipeline (SYS/VB/VI/VENC) via the SDK, so we read our own encoder
 * frames directly (no MMZ-isolation problem) and record with OUR policy/format/naming.
 * Reuses the SDK sample setup helpers (sample_common.c) for hardware bring-up.
 *
 * Build (WSL): bash device/sdk/build.sh (see below) — links libmpi.a (OABI) + common.
 * Run: STOP app.out first (single MPP owner!), then ./our_dvr [seconds]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include "hi_type.h"
#include "hi_common.h"
#include "mpi_sys.h"
#include "mpi_venc.h"
#include "hi_comm_venc.h"
#include "sample_common.h"

#define CHN_TOTAL 4

static volatile int g_run = 1;
static void on_sig(int s){ (void)s; g_run = 0; }

/* Vendor capture bring-up, replicating app.out's tl_hslib_init (FUN_0013527c).
 * The tl_R9508 board driver configures the VIU video input; without this,
 * HI_MPI_VI_SetPubAttr returns SYS_NOTREADY. Keep the fds open (like app.out). */
#define TL_VIN_CFG 0xc00456d3      /* _IOWR('V',211,4) — value selects board video profile */
#define TW_QUERY   0xc00448d7      /* _IOWR('H',215,68) */
#define TW_CFG     0xc00448d8      /* _IOWR('H',216,68) */
static int g_ftw = -1, g_ftl = -1;
static int tw286x_vi_up(void)
{
    g_ftw = open("/dev/tw_286x", O_RDWR);
    g_ftl = open("/dev/tl_R9508", O_RDWR);
    printf("[our_dvr] open tw_286x=%d tl_R9508=%d\n", g_ftw, g_ftl);
    if (g_ftl < 0) { printf("[our_dvr] cannot open /dev/tl_R9508\n"); return -1; }
    /* THE critical call: configure VIU video input for the standard board (0x64). */
    int r = ioctl(g_ftl, TL_VIN_CFG, 0x64);
    printf("[our_dvr] tl_R9508 vin cfg (0xc00456d3,0x64) rc=%d\n", r);
    return 0;
}

int main(int argc, char **argv)
{
    int secs = (argc > 1) ? atoi(argv[1]) : 0;   /* 0 = record until Ctrl-C */
    int c; unsigned p;
    VB_CONF_S vb;
    PAYLOAD_TYPE_E types[2] = { PT_H264, PT_H264 };
    PIC_SIZE_E    sizes[2] = { PIC_D1,  PIC_CIF  };
    FILE *f[CHN_TOTAL];
    unsigned long frames = 0, bytes = 0;
    time_t t0;

    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);

    /* Take over the hardware watchdog from app.out (else the device reboots ~10s
     * after app.out stops petting it). We pet it every loop; on clean exit write
     * 'V' (magic close) to disarm. Open O_WRONLY — must be done AFTER app.out exits. */
    int wdt = open("/dev/watchdog", O_WRONLY);
    time_t wdt_last = 0;

    /* VB pools: D1 + CIF, 8 blocks per channel (same sizing as the SDK sample) */
    memset(&vb, 0, sizeof(vb));
    vb.astCommPool[0].u32BlkSize = 704 * 576 * 2; vb.astCommPool[0].u32BlkCnt = 8 * CHN_TOTAL;
    vb.astCommPool[1].u32BlkSize = 384 * 288 * 2; vb.astCommPool[1].u32BlkCnt = 8 * CHN_TOTAL;

    printf("[our_dvr] init MPP...\n");
    #define WDT_OFF() do{ if(wdt>=0){ write(wdt,"V",1); close(wdt); wdt=-1; } }while(0)
    if (SAMPLE_InitMPP(&vb) != HI_SUCCESS) { printf("InitMPP failed\n"); WDT_OFF(); return 1; }
    tw286x_vi_up();   /* configure VIU video input (tl_R9508) before VI setup */
    if (SAMPLE_StartViVo_SD(CHN_TOTAL, PIC_D1, VO_DEV_SD) != HI_SUCCESS) {
        printf("ViVo failed\n"); SAMPLE_ExitMPP(); WDT_OFF(); return 1;
    }
    if (SAMPLE_StartVenc(CHN_TOTAL, HI_FALSE, types, sizes) != HI_SUCCESS) {
        printf("Venc failed\n"); SAMPLE_StopViVo_SD(CHN_TOTAL, VO_DEV_SD); SAMPLE_ExitMPP(); WDT_OFF(); return 1;
    }

    /* OUR recording policy: one continuous .h264 per channel, our own naming */
    for (c = 0; c < CHN_TOTAL; c++) {
        char nm[64]; sprintf(nm, "/root/rec/a%d/fpv_ch%d.h264", c + 1, c);
        f[c] = fopen(nm, "wb");
        if (!f[c]) printf("[our_dvr] warn: cannot open %s\n", nm);
    }
    printf("[our_dvr] RECORDING %d ch (our own DVR)%s\n", CHN_TOTAL,
           secs ? "" : " — Ctrl-C to stop");

    t0 = time(0);
    while (g_run) {
        if (wdt >= 0 && time(0) != wdt_last) { write(wdt, "w", 1); wdt_last = time(0); } /* pet */
        for (c = 0; c < CHN_TOTAL; c++) {
            VENC_CHN_STAT_S stat;
            VENC_STREAM_S   stream;
            if (!f[c]) continue;
            if (HI_MPI_VENC_Query(c, &stat) != HI_SUCCESS || stat.u32CurPacks == 0) continue;
            stream.u32PackCount = stat.u32CurPacks;
            stream.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S) * stat.u32CurPacks);
            if (!stream.pstPack) continue;
            if (HI_MPI_VENC_GetStream(c, &stream, HI_IO_NOBLOCK) != HI_SUCCESS) {
                free(stream.pstPack); continue;
            }
            for (p = 0; p < stream.u32PackCount; p++) {
                VENC_PACK_S *pk = &stream.pstPack[p];
                fwrite(pk->pu8Addr[0], 1, pk->u32Len[0], f[c]); bytes += pk->u32Len[0];
                if (pk->u32Len[1] > 0) { fwrite(pk->pu8Addr[1], 1, pk->u32Len[1], f[c]); bytes += pk->u32Len[1]; }
                frames++;
            }
            HI_MPI_VENC_ReleaseStream(c, &stream);
            free(stream.pstPack);
        }
        if (secs && (time(0) - t0) >= secs) break;
        usleep(10000);
    }

    for (c = 0; c < CHN_TOTAL; c++) if (f[c]) fclose(f[c]);
    if (wdt >= 0) { write(wdt, "V", 1); close(wdt); }   /* disarm watchdog on clean exit */
    printf("[our_dvr] stopped. packs=%lu bytes=%lu\n", frames, bytes);

    SAMPLE_StopVenc(CHN_TOTAL, HI_FALSE);
    SAMPLE_StopViVo_SD(CHN_TOTAL, VO_DEV_SD);
    SAMPLE_ExitMPP();
    return 0;
}
