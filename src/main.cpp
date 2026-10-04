#include <vita2d.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <string.h>

int selected = 0;
int chosen = -1;
int state;
char netmem[4 * 1024 * 1024];
int dnsResult = 0;
double speed = 0;
int failStage = 0;
int avgLatency = 0, minLatency = 0, maxLatency = 0;
volatile int testState = 0;
volatile int stopRequested = 0;
volatile uint64_t bytesSoFar;
volatile uint64_t testStart = 0;

int resolveHost(const char *name, SceNetInAddr *out) {
    int rid = sceNetResolverCreate("resolver", NULL, 0);
    if (rid < 0) return -1;

    int r = sceNetResolverStartNtoa(rid, name, out, 2 * 1000 * 1000, 3, 0);
    dnsResult = r;
    sceNetResolverDestroy(rid);

    if (r < 0) return -1;
    return 0;
}

double downloadTest() {
    SceNetInAddr ip;
    if (resolveHost("speedtest.belwue.net", &ip) != 0) { failStage = 1; return -1; }

    int sock = sceNetSocket("latency", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (sock < 0) { failStage = 2; return -1; }

    SceNetSockaddrIn addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = SCE_NET_AF_INET;
    addr.sin_port = sceNetHtons(80);
    addr.sin_addr = ip;

    if (sceNetConnect(sock, (SceNetSockaddr *)&addr, sizeof(addr)) < 0) {
        failStage = 3;
        sceNetSocketClose(sock);
        return -1;
    }

    const char *req = "GET /100M HTTP/1.1\r\nHost: speedtest.belwue.net\r\nConnection: close\r\n\r\n";
    if (sceNetSend(sock, req, strlen(req), 0) < 0) {
        failStage = 4;
        sceNetSocketClose(sock);
        return -1;
    }

    static char buf[64 * 1024];
    uint64_t total = 0;
    uint64_t start = sceKernelGetProcessTimeWide();
    testStart = start;
    uint64_t now = start;

    while (now - start < 10000000 && !stopRequested) {
        int n = sceNetRecv(sock, buf, sizeof(buf), 0);
        if (n <= 0) break;
        total += n;
        bytesSoFar += n;
        now = sceKernelGetProcessTimeWide();
    }

    sceNetSocketClose(sock);
    double seconds = (now - start) / 1000000.0;

    if (total == 0 || seconds <= 0) { failStage = 5; return -1; }
    return total * 8 / seconds / 1000000.0;
}

int measureLatency() {
    int sock = sceNetSocket("latency", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (sock < 0) return -1;

    SceNetSockaddrIn addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = SCE_NET_AF_INET;
    addr.sin_port = sceNetHtons(80);
    sceNetInetPton(SCE_NET_AF_INET, "1.1.1.1", &addr.sin_addr);

    uint64_t start = sceKernelGetProcessTimeWide();
    int ret = sceNetConnect(sock, (SceNetSockaddr *)&addr, sizeof(addr));
    uint64_t end = sceKernelGetProcessTimeWide();

    sceNetSocketClose(sock);

    if (ret < 0) return -1;
    return (end - start) / 1000;
}

void getAvgLatency(int testcount, int &min, int &avg, int &max) {
    int latency;
    int latencysum = 0;
    min = 1000000000;
    max = 0;
    for (int i = 0; i<testcount; i++) {
        if (stopRequested) break;
        latency = measureLatency();
        latencysum += latency;
        if (latency > max) max = latency;
        if (latency < min) min = latency;

    }
    avg = latencysum/testcount;
}

int testThread(SceSize args, void *argp) {
    bytesSoFar = 0;
    getAvgLatency(5, minLatency, avgLatency, maxLatency);
    speed = downloadTest();
    testState = 2;
    return sceKernelExitDeleteThread(0);
}

void drawMenu(vita2d_pgf *font) {
        if (selected == 1) {
            vita2d_pgf_draw_text(font, 50, 100, RGBA8(255, 255, 255, 255), 1,
                            "1 - Internet");
            vita2d_pgf_draw_text(font, 50, 140, RGBA8(0, 255, 0, 255), 1,
                            "2 - iperf3");
        }
        else {
            vita2d_pgf_draw_text(font, 50, 100, RGBA8(0, 255, 0, 255), 1,
                            "1 - Internet");
            vita2d_pgf_draw_text(font, 50, 140, RGBA8(255, 255, 255, 255), 1,
                            "2 - iperf3");
        }
}

void drawInternet(vita2d_pgf *font) {
    vita2d_pgf_draw_text(font, 50, 100, RGBA8(255, 255, 255, 255), 1, "Internet Speedtest");
    if (testState == 1) {
        vita2d_pgf_draw_text(font, 50, 140, RGBA8(255, 255, 255, 255), 1, "Running...");

        uint64_t bytes = bytesSoFar;
        if (bytes > 0) {
            double secs = (sceKernelGetProcessTimeWide() - testStart) / 1000000.0;
            if (secs > 0) vita2d_pgf_draw_textf(font, 50, 180, RGBA8(255, 255, 255, 255), 1, "Measuring: %.1f Mbps", bytes * 8 / secs / 1000000.0);
        }
    }
    else if (testState == 2) {
        if (minLatency == -1) {
            vita2d_pgf_draw_text(font, 50, 140, RGBA8(255, 255, 255, 255), 1, "Latency: Failed");
        }
        else {
            vita2d_pgf_draw_textf(font, 50, 140, RGBA8(255, 255, 255, 255), 1, "Latency: avg %d ms, min %d ms, max %d ms", avgLatency, minLatency, maxLatency);
        }

        if (speed == -1) {
            vita2d_pgf_draw_text(font, 50, 180, RGBA8(255, 255, 255, 255), 1, "Download speed: Failed");
        }
        else {
            vita2d_pgf_draw_textf(font, 50, 180, RGBA8(255, 255, 255, 255), 1, "Download speed: %.1f Mbps", speed);
        }
        vita2d_pgf_draw_textf(font, 50, 220, RGBA8(255,255,255,255), 1,
                        "Fail stage: %d, DNS code: 0x%08X", failStage, (unsigned int)dnsResult);
    }
    else {
        vita2d_pgf_draw_text(font, 50, 140, RGBA8(255, 255, 255, 255), 1, "Test is idle.");
    }
}

void drawIperf(vita2d_pgf *font) {
    vita2d_pgf_draw_text(font, 50, 100, RGBA8(255, 255, 255, 255), 1, "iperf3 Speedtest");
    sceNetCtlInetGetState(&state);
}

int main() {
    vita2d_init();
    vita2d_set_clear_color(RGBA8(0, 0, 0, 255));
    vita2d_pgf *font = vita2d_load_default_pgf();

    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    SceNetInitParam param;
    param.memory = netmem;
    param.size = sizeof(netmem);
    param.flags = 0;
    sceNetInit(&param);
    sceNetCtlInit();

    sceNetCtlInetGetState(&state);

    SceNetCtlInfo info;
    if (state == SCE_NETCTL_STATE_CONNECTED) sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info);

    SceCtrlData pad;
    while (true) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        if (pad.buttons & SCE_CTRL_START) break;
        if (chosen == -1) {
            if (pad.buttons & SCE_CTRL_UP) selected = 0;
            if (pad.buttons & SCE_CTRL_DOWN) selected = 1;
            if (pad.buttons & SCE_CTRL_CROSS) {
                chosen = selected;
                if (chosen == 0) {
                    sceNetCtlInetGetState(&state);
                    if (testState != 1) {
                        testState = 1;
                        SceUID thid = sceKernelCreateThread("test", testThread, 0x10000100, 0x10000, 0, 0, NULL);
                        if (thid >= 0) sceKernelStartThread(thid, 0, NULL);
                        else testState = 0;
                    }
                }
            }
        }
        else {
            if (pad.buttons & SCE_CTRL_CIRCLE) chosen = -1;
        }
        vita2d_start_drawing();
        vita2d_clear_screen();
        if (state == SCE_NETCTL_STATE_CONNECTED) {
            vita2d_pgf_draw_textf(font, 10, 540, RGBA8(255, 255, 255, 255), 1, "Vita IP: %s", info.ip_address);
        }
        else vita2d_pgf_draw_text(font, 10, 540, RGBA8(255, 255, 255, 255), 1, "Not connected to network");
        if (chosen == -1) drawMenu(font);
        else if (chosen == 1) drawIperf(font);
        else drawInternet(font);
        vita2d_end_drawing();
        vita2d_swap_buffers();
    }
    stopRequested = 1;
    while (testState == 1) {
        sceKernelDelayThread(10 * 1000); // 10 ms
    }
    sceNetCtlTerm();
    sceNetTerm();
    sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
    vita2d_wait_rendering_done();
    vita2d_free_pgf(font);
    vita2d_fini();
    sceKernelExitProcess(0);
    return 0;
}