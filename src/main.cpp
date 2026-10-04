#include <vita2d.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

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

#define IPERF_SERVER_IP "192.168.1.11"
#define IPERF_PORT 5201
#define COOKIE_SIZE_BYTES 37
#define IPERF_LOG_MAX 8
#define IPERF_TIME_SEC 10
#define IPERF_BLKSIZE 131072

volatile int iperfState = 0;
volatile int iperfStage = 0;
volatile int iperfPhase = 0;   // 1 = handshaking, 2 = sending, 3 = finishing

volatile uint64_t iperfBytes = 0;
volatile uint64_t iperfStart = 0;
volatile double iperfSpeed = 0;
volatile double iperfRecvSpeed = 0;   // receiver's Mbps from the server's results

volatile int iperfLog[IPERF_LOG_MAX];
volatile int iperfLogCount = 0;

static int iperfCtrl = -1;
static int iperfData = -1;

static char iperfCookie[COOKIE_SIZE_BYTES];

static char sendBuf[IPERF_BLKSIZE];

// Holds the server's results JSON (never trust a network length past this).
static char resultBuf[4096];

static void iperfLogPush(int b) {
    if (iperfLogCount < IPERF_LOG_MAX) iperfLog[iperfLogCount++] = b;
}

static void makeCookie(char *out) {
    static const char tbl[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    uint64_t seed = sceKernelGetProcessTimeWide();
    for (int i = 0; i < 36; i++) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        out[i] = tbl[(seed >> 33) % 36];
    }
    out[36] = '\0';
}

static int recvStateByte(int sock) {
    unsigned char b = 0;
    int n = sceNetRecv(sock, &b, 1, 0);
    if (n != 1) return -1000;
    return (int)(signed char)b;
}

static int recvAll(int sock, void *buf, int n) {
    char *p = (char *)buf;
    int got = 0;
    while (got < n) {
        int r = sceNetRecv(sock, p + got, n - got, 0);
        if (r <= 0) return -1;
        got += r;
    }
    return 0;
}

static int sendJson(int sock, const char *json) {
    uint32_t len = sceNetHtonl((uint32_t)strlen(json));
    if (sceNetSend(sock, &len, sizeof(len), 0) != (int)sizeof(len)) return -1;
    if (sceNetSend(sock, json, strlen(json), 0) != (int)strlen(json)) return -1;
    return 0;
}

static int recvJson(int sock, char *buf, int bufSize) {
    uint32_t netLen, len;
    if (recvAll(sock, &netLen, sizeof(netLen)) != 0) return -1;
    len = sceNetNtohl(netLen);
    if (len == 0 || len >= (uint32_t)bufSize) return -1;
    if (recvAll(sock, buf, (int)len) != 0) return -1;
    buf[len] = '\0';
    return 0;
}

int iperfHandshake() {
    SceNetInAddr ip;
    SceNetSockaddrIn addr;
    char json[256];
    int b1, b2, b, i, timeout_us;

    if (iperfCtrl >= 0) { sceNetSocketClose(iperfCtrl); iperfCtrl = -1; }
    if (iperfData >= 0) { sceNetSocketClose(iperfData); iperfData = -1; }
    iperfLogCount = 0;

    if (sceNetInetPton(SCE_NET_AF_INET, IPERF_SERVER_IP, &ip) <= 0) { iperfStage = 1; return -1; }

    iperfCtrl = sceNetSocket("iperf-ctl", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (iperfCtrl < 0) { iperfStage = 2; return -1; }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = SCE_NET_AF_INET;
    addr.sin_port = sceNetHtons(IPERF_PORT);
    addr.sin_addr = ip;

    if (sceNetConnect(iperfCtrl, (SceNetSockaddr *)&addr, sizeof(addr)) < 0) { iperfStage = 3; goto fail; }

    timeout_us = 5 * 1000 * 1000;   // 5 seconds
    sceNetSetsockopt(iperfCtrl, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &timeout_us, sizeof(timeout_us));

    makeCookie(iperfCookie);
    if (sceNetSend(iperfCtrl, iperfCookie, COOKIE_SIZE_BYTES, 0) != COOKIE_SIZE_BYTES) { iperfStage = 4; goto fail; }

    b1 = recvStateByte(iperfCtrl);
    if (b1 == -1000) { iperfStage = 5; goto fail; }
    iperfLogPush(b1);

    snprintf(json, sizeof(json),
        "{\"tcp\":true,\"omit\":0,\"time\":10,\"num\":0,\"blockcount\":0,"
        "\"parallel\":1,\"len\":131072,\"pacing_timer\":1000,"
        "\"client_version\":\"3.21\"}");

    if (sendJson(iperfCtrl, json) != 0) { iperfStage = 6; goto fail; }

    b2 = recvStateByte(iperfCtrl);
    if (b2 == -1000) { iperfStage = 8; goto fail; }
    iperfLogPush(b2);

    iperfData = sceNetSocket("iperf-data", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (iperfData < 0) { iperfStage = 9; goto fail; }
    if (sceNetConnect(iperfData, (SceNetSockaddr *)&addr, sizeof(addr)) < 0) { iperfStage = 9; goto fail; }
    if (sceNetSend(iperfData, iperfCookie, COOKIE_SIZE_BYTES, 0) != COOKIE_SIZE_BYTES) { iperfStage = 10; goto fail; }

    for (i = 0; i < 4; i++) {
        b = recvStateByte(iperfCtrl);
        if (b == -1000) { iperfStage = 11; goto fail; }
        iperfLogPush(b);
        if (b == 2) break;
    }

    return 0;

fail:
    if (iperfData >= 0) { sceNetSocketClose(iperfData); iperfData = -1; }
    if (iperfCtrl >= 0) { sceNetSocketClose(iperfCtrl); iperfCtrl = -1; }
    return -1;
}

void iperfRunTest() {
    memset(sendBuf, 'x', sizeof(sendBuf));

    uint64_t start = sceKernelGetProcessTimeWide();
    iperfStart = start;
    uint64_t total = 0;
    uint64_t now = start;
    int ok = 1;

    while (now - start < (uint64_t)IPERF_TIME_SEC * 1000000ULL && !stopRequested) {
        int n = sceNetSend(iperfData, sendBuf, sizeof(sendBuf), 0);
        if (n < 0) { ok = 0; break; }
        total += n;
        iperfBytes += n;
        now = sceKernelGetProcessTimeWide();
    }

    double seconds = (now - start) / 1000000.0;
    if (seconds > 0) iperfSpeed = total * 8 / seconds / 1000000.0;

    if (ok) {
        unsigned char endByte = 4;
        sceNetSend(iperfCtrl, &endByte, 1, 0);

        int b = recvStateByte(iperfCtrl);
        if (b != -1000) iperfLogPush(b);

        if (b == 13) {
            char res[256];
            snprintf(res, sizeof(res),
                "{\"cpu_util_total\":0,\"cpu_util_user\":0,\"cpu_util_system\":0,"
                "\"sender_has_retransmits\":0,\"streams\":[{\"id\":1,\"bytes\":%llu,"
                "\"retransmits\":0,\"jitter\":0,\"errors\":0,\"packets\":0}]}",
                (unsigned long long)total);

            if (sendJson(iperfCtrl, res) != 0) {
                iperfStage = 12;
            }
            else if (recvJson(iperfCtrl, resultBuf, sizeof(resultBuf)) != 0) {
                iperfStage = 13;
            }
            else {
                unsigned long long recvBytes = 0;
                double recvSecs = 0;
                const char *p = strstr(resultBuf, "\"bytes\":");
                if (p) recvBytes = strtoull(p + 8, NULL, 10);
                const char *t = strstr(resultBuf, "\"end_time\":");
                if (t) recvSecs = strtod(t + 11, NULL);
                if (recvSecs <= 0) recvSecs = seconds;
                if (recvBytes > 0 && recvSecs > 0)
                    iperfRecvSpeed = recvBytes * 8 / recvSecs / 1000000.0;

                int b2 = recvStateByte(iperfCtrl);
                if (b2 != -1000) iperfLogPush(b2);
                unsigned char doneByte = 16;
                sceNetSend(iperfCtrl, &doneByte, 1, 0);
            }
        }
    }

    if (iperfData >= 0) { sceNetSocketClose(iperfData); iperfData = -1; }
    if (iperfCtrl >= 0) { sceNetSocketClose(iperfCtrl); iperfCtrl = -1; }
}

int iperfThread(SceSize args, void *argp) {
    iperfBytes = 0;
    iperfSpeed = 0;
    iperfRecvSpeed = 0;
    iperfPhase = 1;
    if (iperfHandshake() == 0) {
        iperfPhase = 2;
        iperfRunTest();
        iperfPhase = 3;
    }
    iperfState = 2;
    return sceKernelExitDeleteThread(0);
}

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
    sceNetCtlInetGetState(&state);
    vita2d_pgf_draw_text(font, 50, 100, RGBA8(255, 255, 255, 255), 1, "iperf3 Speedtest");
    vita2d_pgf_draw_textf(font, 50, 140, RGBA8(180, 180, 180, 255), 1, "Server: %s:%d", IPERF_SERVER_IP, IPERF_PORT);

    if (iperfState == 1) {
        if (iperfPhase == 1) {
            vita2d_pgf_draw_text(font, 50, 180, RGBA8(255, 255, 255, 255), 1, "Handshaking...");
        }
        else if (iperfPhase == 2) {
            vita2d_pgf_draw_text(font, 50, 180, RGBA8(255, 255, 255, 255), 1, "Sending...");
            uint64_t bytes = iperfBytes;
            if (bytes > 0) {
                double secs = (sceKernelGetProcessTimeWide() - iperfStart) / 1000000.0;
                if (secs > 0)
                    vita2d_pgf_draw_textf(font, 50, 220, RGBA8(0, 255, 0, 255), 1,
                                    "Upload: %.1f Mbps", bytes * 8 / secs / 1000000.0);
            }
        }
        else {
            vita2d_pgf_draw_text(font, 50, 180, RGBA8(255, 255, 255, 255), 1, "Finishing...");
        }
    }
    else if (iperfState == 2) {
        if (iperfStage != 0) {
            vita2d_pgf_draw_textf(font, 50, 180, RGBA8(255, 80, 80, 255), 1,
                            "Local failure at stage %d", iperfStage);
        }

        char line[128];
        int off = snprintf(line, sizeof(line), "States:");
        for (int i = 0; i < iperfLogCount && off < (int)sizeof(line); i++)
            off += snprintf(line + off, sizeof(line) - off, " %d", (int)iperfLog[i]);
        vita2d_pgf_draw_text(font, 50, 220, RGBA8(255, 255, 255, 255), 1, line);

        int last = (iperfLogCount > 0) ? iperfLog[iperfLogCount - 1] : -1;
        if (last == 14)
            vita2d_pgf_draw_text(font, 50, 260, RGBA8(0, 255, 0, 255), 1, "DISPLAY_RESULTS - done");
        else if (last == 13)
            vita2d_pgf_draw_text(font, 50, 260, RGBA8(255, 200, 0, 255), 1, "EXCHANGE_RESULTS");
        else if (last == 2)
            vita2d_pgf_draw_text(font, 50, 260, RGBA8(255, 200, 0, 255), 1, "TEST_RUNNING");

        if (iperfSpeed > 0)
            vita2d_pgf_draw_textf(font, 50, 300, RGBA8(0, 255, 0, 255), 1,
                            "Upload: %.1f Mbps", iperfSpeed);
        if (iperfRecvSpeed > 0)
            vita2d_pgf_draw_textf(font, 50, 340, RGBA8(0, 255, 0, 255), 1,
                            "Receiver: %.1f Mbps", iperfRecvSpeed);
    }
    else {
        vita2d_pgf_draw_text(font, 50, 180, RGBA8(255, 255, 255, 255), 1, "Press X to start handshake.");
    }
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
    SceCtrlData oldPad;
    memset(&oldPad, 0, sizeof(oldPad));
    while (true) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned int pressed = pad.buttons & ~oldPad.buttons;
        oldPad = pad;

        if (pressed & SCE_CTRL_START) break;
        if (chosen == -1) {
            if (pressed & SCE_CTRL_UP) selected = 0;
            if (pressed & SCE_CTRL_DOWN) selected = 1;
            if (pressed & SCE_CTRL_CROSS) {
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
            if (pressed & SCE_CTRL_CIRCLE) chosen = -1;
            if (chosen == 1 && (pressed & SCE_CTRL_CROSS) && iperfState != 1) {
                sceNetCtlInetGetState(&state);
                iperfStage = 0;
                iperfPhase = 1;
                iperfState = 1;
                SceUID thid = sceKernelCreateThread("iperf", iperfThread, 0x10000100, 0x10000, 0, 0, NULL);
                if (thid >= 0) sceKernelStartThread(thid, 0, NULL);
                else iperfState = 0;
            }
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
    while (testState == 1 || iperfState == 1) {
        sceKernelDelayThread(10 * 1000); // 10 ms
    }
    if (iperfData >= 0) { sceNetSocketClose(iperfData); iperfData = -1; }
    if (iperfCtrl >= 0) { sceNetSocketClose(iperfCtrl); iperfCtrl = -1; }
    sceNetCtlTerm();
    sceNetTerm();
    sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
    vita2d_wait_rendering_done();
    vita2d_free_pgf(font);
    vita2d_fini();
    sceKernelExitProcess(0);
    return 0;
}