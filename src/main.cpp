#include <vita2d.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/io/stat.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const int kRcvBufBytes = 512 * 1024;

int selected = 0;
int chosen = -1;
char netmem[4 * 1024 * 1024];
int dnsResult = 0;
double speed = 0;
int failStage = 0;
int avgLatency = 0, minLatency = 0, maxLatency = 0;
int rcvBufSetRet = 0;
int rcvBufGetRet = 0;
int rcvBufEff = 0;
volatile int testState = 0;       // 0 idle, 1 running, 2 done, 3 cancelled
volatile int testPhase = 0;       // 1 = latency, 2 = download
volatile int stopRequested = 0;
volatile int cancelRequested = 0;
int showDebug = 0;
volatile int lastRunStreams = 3;
static inline int shouldStop() { return stopRequested || cancelRequested; }
volatile uint64_t bytesSoFar;
volatile uint64_t testStart = 0;
volatile uint64_t rampBaseBytes = 0;
volatile uint64_t rampBaseTime = 0;
volatile int rampDone = 0;

struct WifiStatus { int connected; char ip[16]; char ssid[33]; int rssiPct; int channel; };
static WifiStatus wifi;

static void refreshWifi() {
    SceNetCtlInfo q;
    memset(&wifi, 0, sizeof(wifi));
    int st = SCE_NETCTL_STATE_DISCONNECTED;
    sceNetCtlInetGetState(&st);
    wifi.connected = (st == SCE_NETCTL_STATE_CONNECTED);
    if (!wifi.connected) return;
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &q) == 0)
        snprintf(wifi.ip, sizeof(wifi.ip), "%s", q.ip_address);
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_SSID, &q) == 0)
        snprintf(wifi.ssid, sizeof(wifi.ssid), "%s", q.ssid);
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_RSSI_PERCENTAGE, &q) == 0) wifi.rssiPct = q.rssi_percentage;
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_CHANNEL, &q) == 0)         wifi.channel = q.channel;
}

#define RAMP_MS 2000

#define CONNECT_TIMEOUT_MS 4000
#define LATENCY_CONNECT_TIMEOUT_MS 2000

#define COOKIE_SIZE_BYTES 37
#define IPERF_LOG_MAX 8
#define IPERF_TIME_SEC 10
#define IPERF_BLKSIZE 131072
#define DOWNLOAD_TIME_US 10000000ULL // 10s

#define CONFIG_DIR       "ux0:data/vitaspeed"
#define CONFIG_PATH      "ux0:data/vitaspeed/config.txt"
#define DEFAULT_IPERF_IP    "192.168.1.11"
#define DEFAULT_IPERF_PORT  5201
#define DEFAULT_DL_HOST     "speedtest.belwue.net"
#define DEFAULT_DL_PATH     "/100M"

char iperfServerIp[32]  = DEFAULT_IPERF_IP;
int  iperfPort          = DEFAULT_IPERF_PORT;
char dlHostBuf[64]      = DEFAULT_DL_HOST;
char dlPathBuf[128]     = DEFAULT_DL_PATH;

int  configStatus = 0;
int  configIssues = 0;

volatile int iperfState = 0;      // 0 idle, 1 running, 2 done, 3 cancelled
volatile int iperfStage = 0;
volatile int iperfPhase = 0;      // 1 = handshaking, 2 = sending, 3 = finishing
volatile int iperfReverse = 0;
volatile int iperfRunReverse = 0;

volatile uint64_t iperfBytes = 0;
volatile uint64_t iperfStart = 0;
volatile double iperfSpeed = 0;
volatile double iperfPeerSpeed = 0;

volatile int iperfLog[IPERF_LOG_MAX];
volatile int iperfLogCount = 0;

static int iperfCtrl = -1;
static int iperfData = -1;

static char iperfCookie[COOKIE_SIZE_BYTES];

static char sendBuf[IPERF_BLKSIZE];

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

static int connectWithTimeout(int sock, const SceNetSockaddr *addr, unsigned int addrlen, int timeout_ms) {
    int t = timeout_ms * 1000;
    sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDTIMEO, &t, sizeof(t));
    int r = sceNetConnect(sock, addr, addrlen);
    int restore = 60 * 1000 * 1000;
    sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDTIMEO, &restore, sizeof(restore));
    return (r < 0) ? -1 : 0;
}

int iperfHandshake() {
    SceNetInAddr ip;
    SceNetSockaddrIn addr;
    char json[256];
    int b1, b2, b, i, timeout_us;

    if (iperfCtrl >= 0) { sceNetSocketClose(iperfCtrl); iperfCtrl = -1; }
    if (iperfData >= 0) { sceNetSocketClose(iperfData); iperfData = -1; }
    iperfLogCount = 0;

    if (sceNetInetPton(SCE_NET_AF_INET, iperfServerIp, &ip) <= 0) { iperfStage = 1; return -1; }

    iperfCtrl = sceNetSocket("iperf-ctl", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (iperfCtrl < 0) { iperfStage = 2; return -1; }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = SCE_NET_AF_INET;
    addr.sin_port = sceNetHtons(iperfPort);
    addr.sin_addr = ip;

    if (connectWithTimeout(iperfCtrl, (SceNetSockaddr *)&addr, sizeof(addr), CONNECT_TIMEOUT_MS) < 0) { iperfStage = 3; goto fail; }

    timeout_us = 5 * 1000 * 1000;   // 5s
    sceNetSetsockopt(iperfCtrl, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &timeout_us, sizeof(timeout_us));

    makeCookie(iperfCookie);
    if (sceNetSend(iperfCtrl, iperfCookie, COOKIE_SIZE_BYTES, 0) != COOKIE_SIZE_BYTES) { iperfStage = 4; goto fail; }

    b1 = recvStateByte(iperfCtrl);
    if (b1 == -1000) { iperfStage = 5; goto fail; }
    iperfLogPush(b1);

    // NOTE: the server treats the PRESENCE of "reverse" as true (its bool check
    // accepts false too), so the key must be omitted for upload - never sent as
    // false. The real client only adds it when reverse is on.
    snprintf(json, sizeof(json),
        "{\"tcp\":true,\"omit\":0,\"time\":%d,\"num\":0,\"blockcount\":0,"
        "\"parallel\":1,\"len\":131072,\"pacing_timer\":1000,"
        "%s\"client_version\":\"3.21\"}",
        IPERF_TIME_SEC, iperfRunReverse ? "\"reverse\":true," : "");

    if (sendJson(iperfCtrl, json) != 0) { iperfStage = 6; goto fail; }

    b2 = recvStateByte(iperfCtrl);
    if (b2 == -1000) { iperfStage = 7; goto fail; }
    iperfLogPush(b2);

    iperfData = sceNetSocket("iperf-data", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (iperfData < 0) { iperfStage = 8; goto fail; }
    sceNetSetsockopt(iperfData, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVBUF, &kRcvBufBytes, sizeof(kRcvBufBytes));
    if (connectWithTimeout(iperfData, (SceNetSockaddr *)&addr, sizeof(addr), CONNECT_TIMEOUT_MS) < 0) { iperfStage = 8; goto fail; }
    if (sceNetSend(iperfData, iperfCookie, COOKIE_SIZE_BYTES, 0) != COOKIE_SIZE_BYTES) { iperfStage = 9; goto fail; }

    for (i = 0; i < 4; i++) {
        b = recvStateByte(iperfCtrl);
        if (b == -1000) { iperfStage = 10; goto fail; }
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

    if (!iperfRunReverse) {
        while (now - start < (uint64_t)IPERF_TIME_SEC * 1000000ULL && !shouldStop()) {
            int n = sceNetSend(iperfData, sendBuf, sizeof(sendBuf), 0);
            if (n < 0) { ok = 0; break; }
            total += n;
            iperfBytes += n;
            now = sceKernelGetProcessTimeWide();
        }
    }
    else {
        // Download (reverse): the server sends, we receive. A short receive
        // timeout on the data socket keeps the last recv (after the server
        // stops) from blocking forever - a negative return just re-checks the
        // clock. now must refresh every pass, timeouts included.
        int timeout_us = 500 * 1000;   // 500 ms
        sceNetSetsockopt(iperfData, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &timeout_us, sizeof(timeout_us));

        while (now - start < (uint64_t)IPERF_TIME_SEC * 1000000ULL && !shouldStop()) {
            int n = sceNetRecv(iperfData, sendBuf, sizeof(sendBuf), 0);
            if (n > 0) { total += n; iperfBytes += n; }
            else if (n == 0) { ok = 0; break; }
            now = sceKernelGetProcessTimeWide();
        }
    }

    double seconds = (now - start) / 1000000.0;
    if (seconds > 0) iperfSpeed = total * 8 / seconds / 1000000.0;

    // Skip the results exchange on cancel so we don't negotiate a partial result.
    if (ok && !cancelRequested) {
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
                iperfStage = 11;   // could not send our results
            }
            else if (recvJson(iperfCtrl, resultBuf, sizeof(resultBuf)) != 0) {
                iperfStage = 12;   // could not read the server's results
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
                    iperfPeerSpeed = recvBytes * 8 / recvSecs / 1000000.0;

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
    iperfPeerSpeed = 0;
    iperfPhase = 1;
    if (iperfHandshake() == 0) {
        iperfPhase = 2;
        iperfRunTest();
        iperfPhase = 3;
    }
    iperfState = cancelRequested ? 3 : 2;
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

#define MAX_STREAMS 4
static char dlBuf[MAX_STREAMS][32 * 1024];
volatile uint32_t dlBytes[MAX_STREAMS];
volatile int dlDone[MAX_STREAMS];
volatile uint64_t dlDeadline = 0;
static SceNetInAddr dlIp;
volatile int dlStreams = 3;
volatile int httpStatus = 0;

int dlWorker(SceSize args, void *argp) {
    int idx = *(int *)argp;
    if (idx < 0 || idx >= MAX_STREAMS) return sceKernelExitDeleteThread(0);

    int sock = sceNetSocket("dl", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (sock < 0) { dlDone[idx] = 1; return sceKernelExitDeleteThread(0); }

    int sret = sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVBUF, &kRcvBufBytes, sizeof(kRcvBufBytes));
    int eff = 0;
    unsigned int elen = sizeof(eff);
    int gret = sceNetGetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVBUF, &eff, &elen);
    if (idx == 0) { rcvBufSetRet = sret; rcvBufGetRet = gret; rcvBufEff = eff; }

    int timeout_us = 500 * 1000;
    sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &timeout_us, sizeof(timeout_us));

    SceNetSockaddrIn addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = SCE_NET_AF_INET;
    addr.sin_port = sceNetHtons(80);
    addr.sin_addr = dlIp;

    if (connectWithTimeout(sock, (SceNetSockaddr *)&addr, sizeof(addr), CONNECT_TIMEOUT_MS) < 0) {
        sceNetSocketClose(sock);
        dlDone[idx] = 1;
        return sceKernelExitDeleteThread(0);
    }

    char req[256];
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", dlPathBuf, dlHostBuf);
    if (sceNetSend(sock, req, strlen(req), 0) < 0) {
        sceNetSocketClose(sock);
        dlDone[idx] = 1;
        return sceKernelExitDeleteThread(0);
    }

    int sawHeader = 0;
    uint64_t now = sceKernelGetProcessTimeWide();
    while (now < dlDeadline && !shouldStop()) {
        int n = sceNetRecv(sock, dlBuf[idx], sizeof(dlBuf[idx]), 0);
        if (n > 0) {
            if (idx == 0 && !sawHeader) {
                sawHeader = 1;
                const char *sp = (const char *)memchr(dlBuf[idx], ' ', n);
                if (sp) httpStatus = atoi(sp + 1);
            }
            dlBytes[idx] += n;
        }
        else if (n == 0) break;
        now = sceKernelGetProcessTimeWide();
    }

    sceNetSocketClose(sock);
    dlDone[idx] = 1;
    return sceKernelExitDeleteThread(0);
}

double downloadTest() {
    if (shouldStop()) return -1;

    if (resolveHost(dlHostBuf, &dlIp) != 0) { failStage = 1; return -1; }

    int n = lastRunStreams;
    if (n < 1) n = 1;
    if (n > MAX_STREAMS) n = MAX_STREAMS;

    for (int i = 0; i < MAX_STREAMS; i++) { dlBytes[i] = 0; dlDone[i] = 0; }
    httpStatus = 0;

    uint64_t start = sceKernelGetProcessTimeWide();
    testStart = start;
    testPhase = 2;
    rampBaseBytes = 0;
    rampBaseTime = start;
    rampDone = 0;
    bytesSoFar = 0;
    dlDeadline = start + DOWNLOAD_TIME_US;

    SceUID startedThreads[MAX_STREAMS];
    int startedIdx[MAX_STREAMS];
    int started = 0;
    for (int i = 0; i < n; i++) {
        SceUID th = sceKernelCreateThread("dl", dlWorker, 0x10000100, 0x10000, 0, 0, NULL);
        if (th < 0) continue;
        int idx = i;
        if (sceKernelStartThread(th, sizeof(idx), &idx) == 0) {
            startedThreads[started] = th;
            startedIdx[started] = i;
            started++;
        } else {
            sceKernelDeleteThread(th);
        }
    }
    if (started == 0) { failStage = 2; return -1; }

    while (sceKernelGetProcessTimeWide() < dlDeadline && !shouldStop()) {
        uint64_t sum = 0;
        for (int i = 0; i < MAX_STREAMS; i++) sum += dlBytes[i];
        bytesSoFar = sum;

        uint64_t now = sceKernelGetProcessTimeWide();
        if (!rampDone && now - start >= (uint64_t)RAMP_MS * 1000) {
            rampBaseBytes = sum;
            rampBaseTime = now;
            rampDone = 1;
        }

        int alldone = 1;
        for (int i = 0; i < started; i++) if (!dlDone[startedIdx[i]]) { alldone = 0; break; }
        if (alldone) break;

        sceKernelDelayThread(100 * 1000);   // 100 ms
    }
    uint64_t end = sceKernelGetProcessTimeWide();

    for (int i = 0; i < started; i++)
        sceKernelWaitThreadEnd(startedThreads[i], NULL, NULL);

    uint64_t total = 0;
    for (int i = 0; i < MAX_STREAMS; i++) total += dlBytes[i];
    bytesSoFar = total;

    uint64_t bytes;
    double seconds;
    if (rampDone && end - rampBaseTime >= 1000000ULL) {
        bytes = total - rampBaseBytes;
        seconds = (end - rampBaseTime) / 1000000.0;
    } else {
        bytes = total;
        seconds = (end - start) / 1000000.0;
    }

    if (bytes == 0 || seconds <= 0) { failStage = 5; return -1; }
    return bytes * 8 / seconds / 1000000.0;
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
    int ret = connectWithTimeout(sock, (SceNetSockaddr *)&addr, sizeof(addr), LATENCY_CONNECT_TIMEOUT_MS);
    uint64_t end = sceKernelGetProcessTimeWide();

    sceNetSocketClose(sock);

    if (ret < 0) return -1;
    return (end - start) / 1000;
}

void getAvgLatency(int testcount, int &min, int &avg, int &max) {
    int latencysum = 0;
    int ok = 0;
    min = 1000000000;
    max = 0;
    for (int i = 0; i < testcount; i++) {
        if (shouldStop()) break;
        int latency = measureLatency();
        if (latency < 0) continue;
        latencysum += latency;
        ok++;
        if (latency > max) max = latency;
        if (latency < min) min = latency;
    }
    if (ok == 0) { min = -1; avg = -1; max = -1; return; }
    avg = latencysum / ok;
}

int testThread(SceSize args, void *argp) {
    bytesSoFar = 0;
    rampDone = 0;
    testPhase = 1;
    getAvgLatency(5, minLatency, avgLatency, maxLatency);
    speed = downloadTest();
    testState = cancelRequested ? 3 : 2;
    return sceKernelExitDeleteThread(0);
}

static void startInternetTest() {
    if (testState == 1) return;
    cancelRequested = 0;
    failStage = 0;
    dnsResult = 0;
    lastRunStreams = dlStreams;
    testState = 1;
    SceUID thid = sceKernelCreateThread("test", testThread, 0x10000100, 0x10000, 0, 0, NULL);
    if (thid >= 0) sceKernelStartThread(thid, 0, NULL);
    else testState = 0;
}

static void drawProgress(int x, int y, int w, int h, float frac) {
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    vita2d_draw_rectangle((float)x, (float)y, (float)w, (float)h, RGBA8(60, 60, 60, 255));
    vita2d_draw_rectangle((float)x, (float)y, (float)w * frac, (float)h, RGBA8(0, 200, 0, 255));
}

static void drawBigSpeed(vita2d_pgf *font, int x, int y, double mbps) {
    char num[16];
    snprintf(num, sizeof(num), "%.1f", mbps);
    vita2d_pgf_draw_text(font, x, y, RGBA8(0, 255, 0, 255), 3.0f, num);
    int w = vita2d_pgf_text_width(font, 3.0f, num);
    vita2d_pgf_draw_text(font, x + w + 10, y, RGBA8(160, 160, 160, 255), 1.3f, "Mbps");
}

static const char *iperfErrorText(int stage) {
    switch (stage) {
        case 1: return "Server address is invalid";
        case 2: return "Couldn't create a socket";
        case 3: return "Can't reach the server (timed out or refused)";
        case 5: case 7: case 10: return "Server stopped answering";
        case 4: case 6: case 8: case 9: return "Connection to the server failed";
        case 11: case 12: return "Couldn't exchange results";
        default: return "Test failed";
    }
}

static const char *internetErrorText(int stage) {
    switch (stage) {
        case 1: return "Can't look up the download server";
        case 2: return "Couldn't start the download threads";
        case 5: return "No data received from the server";
        default: return "Test failed";
    }
}

void drawMenu(vita2d_pgf *font) {
    const unsigned int WHITE = RGBA8(255, 255, 255, 255);
    const unsigned int GREEN = RGBA8(0, 255, 0, 255);
    const unsigned int DIM   = RGBA8(120, 120, 120, 255);
    const unsigned int AMBER = RGBA8(255, 200, 0, 255);

    vita2d_pgf_draw_text(font, 50, 100, selected == 0 ? GREEN : WHITE, 1, "1 - Internet");

    const char *label = "2 - iperf3";
    vita2d_pgf_draw_text(font, 50, 140, selected == 1 ? GREEN : WHITE, 1, label);

    int w = vita2d_pgf_text_width(font, 1.0f, label);
    int x = 50 + w + 12;
    char target[64];
    snprintf(target, sizeof(target), "@ %s:%d", iperfServerIp, iperfPort);
    vita2d_pgf_draw_text(font, x, 140, DIM, 1, target);

    if (configIssues > 0) {
        int tw = vita2d_pgf_text_width(font, 1.0f, target);
        vita2d_pgf_draw_text(font, x + tw + 8, 140, AMBER, 1, "!");
    }

    vita2d_pgf_draw_text(font, 50, 450, DIM, 1.0f,
                    "Change the iperf3 server IP and download settings in:");
    vita2d_pgf_draw_text(font, 50, 474, RGBA8(150, 150, 150, 255), 1.0f,
                    "ux0:data/vitaspeed/config.txt   (ip, port, download_host, download_path)");
}

void drawInternet(vita2d_pgf *font) {
    const unsigned int WHITE = RGBA8(255, 255, 255, 255);
    const unsigned int DIM   = RGBA8(150, 150, 150, 255);
    const unsigned int RED   = RGBA8(255, 80, 80, 255);
    const unsigned int AMBER = RGBA8(255, 200, 0, 255);

    vita2d_pgf_draw_text(font, 50, 50, WHITE, 1.2f, "Internet Speedtest");
    vita2d_pgf_draw_textf(font, 50, 90, DIM, 1, "Next run: %d stream%s (L/R to change)",
                    dlStreams, dlStreams == 1 ? "" : "s");

    if (testState == 1) {
        if (cancelRequested) {
            vita2d_pgf_draw_text(font, 50, 230, RED, 1, "Cancelling...");
        }
        else if (testPhase == 1) {
            vita2d_pgf_draw_text(font, 50, 230, WHITE, 1, "Measuring latency...");
        }
        else {
            float frac = (sceKernelGetProcessTimeWide() - testStart) / (float)DOWNLOAD_TIME_US;
            drawProgress(50, 100, 860, 14, frac);
            vita2d_pgf_draw_text(font, 50, 200, WHITE, 1, "Downloading...");
            uint64_t bytes = bytesSoFar;
            uint64_t baseBytes = 0, baseTime = testStart;
            if (rampDone) { baseBytes = rampBaseBytes; baseTime = rampBaseTime; }
            if (bytes > baseBytes) {
                double secs = (sceKernelGetProcessTimeWide() - baseTime) / 1000000.0;
                if (secs > 0) drawBigSpeed(font, 50, 270, (bytes - baseBytes) * 8 / secs / 1000000.0);
            }
        }
    }
    else if (testState == 2) {
        if (speed < 0) {
            char msg[128];
            if (showDebug) snprintf(msg, sizeof(msg), "%s (stage %d)", internetErrorText(failStage), failStage);
            else           snprintf(msg, sizeof(msg), "%s", internetErrorText(failStage));
            vita2d_pgf_draw_text(font, 50, 250, RED, 1, msg);
        }
        else {
            drawBigSpeed(font, 50, 270, speed);
        }

        if (minLatency == -1)
            vita2d_pgf_draw_text(font, 50, 320, DIM, 1, "Latency: failed");
        else
            vita2d_pgf_draw_textf(font, 50, 320, DIM, 1,
                            "Latency: avg %d ms, min %d ms, max %d ms", avgLatency, minLatency, maxLatency);
        vita2d_pgf_draw_textf(font, 50, 350, DIM, 1, "%d streams  -  %s%s",
                        lastRunStreams, dlHostBuf, dlPathBuf);
        if (httpStatus != 0 && httpStatus != 200)
            vita2d_pgf_draw_textf(font, 50, 380, AMBER, 1,
                            "Server answered %d (expected 200)", httpStatus);
    }
    else if (testState == 3) {
        vita2d_pgf_draw_text(font, 50, 230, RED, 1, "Cancelled");
    }
    else {
        vita2d_pgf_draw_text(font, 50, 230, WHITE, 1, "Press X to run");
        vita2d_pgf_draw_textf(font, 50, 270, DIM, 1, "%d streams  -  %s%s",
                        dlStreams, dlHostBuf, dlPathBuf);
    }

    if (showDebug) {
        vita2d_pgf_draw_textf(font, 50, 442, DIM, 0.9f,
                        "failStage %d  dns 0x%08X", failStage, (unsigned int)dnsResult);
        vita2d_pgf_draw_textf(font, 50, 462, DIM, 0.9f,
                        "RCVBUF want %d KB, got %d KB (set=%d get=%d)",
                        kRcvBufBytes / 1024, rcvBufEff / 1024, rcvBufSetRet, rcvBufGetRet);
    }
}

void drawIperf(vita2d_pgf *font) {
    const unsigned int WHITE = RGBA8(255, 255, 255, 255);
    const unsigned int DIM   = RGBA8(150, 150, 150, 255);
    const unsigned int RED   = RGBA8(255, 80, 80, 255);

    vita2d_pgf_draw_text(font, 50, 50, WHITE, 1.2f, "iperf3 Speedtest");

    char src[64];
    if (configStatus == 1 && configIssues == 0)
        snprintf(src, sizeof(src), "config");
    else if (configStatus == 1)
        snprintf(src, sizeof(src), "config, %d bad value(s) ignored", configIssues);
    else if (configStatus == 3)
        snprintf(src, sizeof(src), "can't write config");
    else
        snprintf(src, sizeof(src), "default");
    vita2d_pgf_draw_textf(font, 50, 90, DIM, 1, "Server: %s:%d (%s)", iperfServerIp, iperfPort, src);
    vita2d_pgf_draw_textf(font, 50, 120, DIM, 1, "Next run: %s (Square to change)",
                    iperfReverse ? "Download" : "Upload");

    if (iperfState == 1) {
        if (cancelRequested) {
            vita2d_pgf_draw_text(font, 50, 230, RED, 1, "Cancelling...");
        }
        else if (iperfPhase == 2) {
            float frac = (sceKernelGetProcessTimeWide() - iperfStart) / (float)(IPERF_TIME_SEC * 1000000LL);
            drawProgress(50, 132, 860, 14, frac);   // below the two header lines
            vita2d_pgf_draw_text(font, 50, 200, WHITE, 1, iperfRunReverse ? "Receiving..." : "Sending...");
            uint64_t bytes = iperfBytes;
            if (bytes > 0) {
                double secs = (sceKernelGetProcessTimeWide() - iperfStart) / 1000000.0;
                if (secs > 0) drawBigSpeed(font, 50, 270, bytes * 8 / secs / 1000000.0);
            }
        }
        else {
            vita2d_pgf_draw_text(font, 50, 230, WHITE, 1, "Handshaking...");
        }
    }
    else if (iperfState == 2) {
        if (iperfStage != 0) {
            char msg[128];
            if (showDebug) snprintf(msg, sizeof(msg), "%s (stage %d)", iperfErrorText(iperfStage), iperfStage);
            else           snprintf(msg, sizeof(msg), "%s", iperfErrorText(iperfStage));
            vita2d_pgf_draw_text(font, 50, 250, RED, 1, msg);
        }
        else if (iperfSpeed > 0) {
            drawBigSpeed(font, 50, 270, iperfSpeed);
            if (iperfPeerSpeed > 0)
                vita2d_pgf_draw_textf(font, 50, 330, DIM, 1,
                                iperfRunReverse ? "Server sent: %.1f Mbps" : "Receiver: %.1f Mbps",
                                iperfPeerSpeed);
        }
    }
    else if (iperfState == 3) {
        vita2d_pgf_draw_text(font, 50, 230, RED, 1, "Cancelled");
    }
    else {
        vita2d_pgf_draw_text(font, 50, 230, WHITE, 1, "Press X to start");
    }

    if (showDebug) {
        char line[128];
        int off = snprintf(line, sizeof(line), "States:");
        for (int i = 0; i < iperfLogCount && off < (int)sizeof(line); i++)
            off += snprintf(line + off, sizeof(line) - off, " %d", (int)iperfLog[i]);
        vita2d_pgf_draw_text(font, 50, 442, DIM, 0.9f, line);

        int last = (iperfLogCount > 0) ? iperfLog[iperfLogCount - 1] : -1;
        const char *name = last == 14 ? "done" : last == 13 ? "exchange results"
                         : last == 2 ? "test running" : last == 1 ? "test start" : "";
        if (*name)
            vita2d_pgf_draw_textf(font, 50, 462, DIM, 0.9f, "last state %d (%s)", last, name);
    }
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r' || s[n-1] == '\n'))
        s[--n] = '\0';
    return s;
}

static int validHost(const char *s, size_t maxLen) {
    size_t n = strlen(s);
    if (n == 0 || n >= maxLen) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-')) return 0;
    }
    return 1;
}

static int validPath(const char *s, size_t maxLen) {
    size_t n = strlen(s);
    if (n == 0 || n >= maxLen || s[0] != '/') return 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)s[i] <= 0x20 || s[i] == 0x7F) return 0;
    return 1;
}

static void loadConfig() {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        sceIoMkdir(CONFIG_DIR, 0777);
        f = fopen(CONFIG_PATH, "w");
        if (f) {
            fprintf(f,
                "# Vita Speedtest config\n"
                "ip=%s\n"
                "port=%d\n"
                "download_host=%s\n"
                "download_path=%s\n",
                DEFAULT_IPERF_IP, DEFAULT_IPERF_PORT, DEFAULT_DL_HOST, DEFAULT_DL_PATH);
            fclose(f);
            configStatus = 0;
        } else {
            configStatus = 3;
        }
        return;
    }

    configStatus = 1;
    char line[256];
    int first = 1;
    while (fgets(line, sizeof(line), f)) {
        if (!strchr(line, '\n') && !feof(f)) {
            int ch;
            while ((ch = fgetc(f)) != '\n' && ch != EOF) { }
            configIssues++;
            first = 0;
            continue;
        }

        char *s = line;
        if (first && strlen(s) >= 3 &&
            (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
            s += 3;
        first = 0;

        s = trim(s);
        if (*s == '\0' || *s == '#') continue;

        char *eq = strchr(s, '=');
        if (!eq) { configIssues++; continue; }

        *eq = '\0';
        char *key = trim(s);
        char *val = trim(eq + 1);

        if (strcmp(key, "ip") == 0) {
            SceNetInAddr t;
            if (strlen(val) < sizeof(iperfServerIp) &&
                sceNetInetPton(SCE_NET_AF_INET, val, &t) > 0)
                snprintf(iperfServerIp, sizeof(iperfServerIp), "%s", val);
            else configIssues++;
        }
        else if (strcmp(key, "port") == 0) {
            char *end;
            long p = strtol(val, &end, 10);
            if (*val != '\0' && *end == '\0' && p >= 1 && p <= 65535) iperfPort = (int)p;
            else configIssues++;
        }
        else if (strcmp(key, "download_host") == 0) {
            if (validHost(val, sizeof(dlHostBuf))) snprintf(dlHostBuf, sizeof(dlHostBuf), "%s", val);
            else configIssues++;
        }
        else if (strcmp(key, "download_path") == 0) {
            if (validPath(val, sizeof(dlPathBuf))) snprintf(dlPathBuf, sizeof(dlPathBuf), "%s", val);
            else configIssues++;
        }
    }
    fclose(f);
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

    loadConfig();

    refreshWifi();
    uint64_t lastWifiPoll = sceKernelGetProcessTimeWide();

    SceCtrlData pad;
    SceCtrlData oldPad;
    memset(&oldPad, 0, sizeof(oldPad));
    while (true) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned int pressed = pad.buttons & ~oldPad.buttons;
        oldPad = pad;

        uint64_t now = sceKernelGetProcessTimeWide();
        if (now - lastWifiPoll >= 1000000ULL) { lastWifiPoll = now; refreshWifi(); }

        if (pressed & SCE_CTRL_SELECT) showDebug = !showDebug;
        if (pressed & SCE_CTRL_START) break;

        if (pressed & SCE_CTRL_CIRCLE) {
            int running = (chosen == 0 && testState == 1) || (chosen == 1 && iperfState == 1);
            if (running) cancelRequested = 1;
            else if (chosen != -1) chosen = -1;
        }

        if (chosen == -1) {
            if (pressed & SCE_CTRL_UP) selected = 0;
            if (pressed & SCE_CTRL_DOWN) selected = 1;
            if (pressed & SCE_CTRL_CROSS) {
                chosen = selected;
                if (chosen == 0) startInternetTest();
            }
        }
        else if (chosen == 0) {
            if (testState != 1) {
                if ((pressed & SCE_CTRL_LTRIGGER) && dlStreams > 1) dlStreams--;
                if ((pressed & SCE_CTRL_RTRIGGER) && dlStreams < MAX_STREAMS) dlStreams++;
                if (pressed & SCE_CTRL_CROSS) startInternetTest();
            }
        }
        else {
            if ((pressed & SCE_CTRL_SQUARE) && iperfState != 1)
                iperfReverse = !iperfReverse;
            if ((pressed & SCE_CTRL_CROSS) && iperfState != 1) {
                iperfRunReverse = iperfReverse;
                cancelRequested = 0;
                iperfStage = 0;
                iperfPhase = 1;
                iperfState = 1;
                SceUID thid = sceKernelCreateThread("iperf", iperfThread, 0x10000100, 0x10000, 0, 0, NULL);
                if (thid >= 0) sceKernelStartThread(thid, 0, NULL);
                else iperfState = 0;
            }
        }

        const char *hint;
        if (chosen == -1)                        hint = "Up/Down choose    X open    Start exit";
        else if (chosen == 0 && testState == 1)  hint = "O cancel    Start exit";
        else if (chosen == 0)                    hint = "X run again    L/R streams    O back    Select debug    Start exit";
        else if (chosen == 1 && iperfState == 1) hint = "O cancel    Start exit";
        else                                     hint = "X start    Square mode    O back    Select debug    Start exit";

        vita2d_start_drawing();
        vita2d_clear_screen();
        if (chosen == -1) drawMenu(font);
        else if (chosen == 1) drawIperf(font);
        else drawInternet(font);
        vita2d_pgf_draw_text(font, 10, 508, RGBA8(110, 110, 110, 255), 0.85f, hint);
        if (wifi.connected)
            vita2d_pgf_draw_textf(font, 10, 536, RGBA8(180, 180, 180, 255), 0.9f,
                            "Wi-Fi: %s ch %d signal %d%% IP %s", wifi.ssid, wifi.channel, wifi.rssiPct, wifi.ip);
        else
            vita2d_pgf_draw_text(font, 10, 536, RGBA8(200, 120, 120, 255), 0.9f, "Not connected");
        vita2d_pgf_draw_text(font, 890, 536, RGBA8(180, 180, 180, 255), 1, "@jajtic");
        vita2d_end_drawing();
        vita2d_swap_buffers();
    }
    stopRequested = 1;
    uint64_t exitDeadline = sceKernelGetProcessTimeWide() + 8 * 1000000ULL;   // 8 s max
    while ((testState == 1 || iperfState == 1) && sceKernelGetProcessTimeWide() < exitDeadline) {
        sceKernelDelayThread(10 * 1000); // 10 ms
    }

    if (testState != 1 && iperfState != 1) {
        if (iperfData >= 0) { sceNetSocketClose(iperfData); iperfData = -1; }
        if (iperfCtrl >= 0) { sceNetSocketClose(iperfCtrl); iperfCtrl = -1; }
        sceNetCtlTerm();
        sceNetTerm();
        sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
        vita2d_wait_rendering_done();
        vita2d_free_pgf(font);
        vita2d_fini();
    }
    sceKernelExitProcess(0);
    return 0;
}