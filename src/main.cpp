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
int state;
char netmem[4 * 1024 * 1024];
int dnsResult = 0;
double speed = 0;
int failStage = 0;
int avgLatency = 0, minLatency = 0, maxLatency = 0;
int rcvBufSetRet = 0;
int rcvBufGetRet = 0;
int rcvBufEff = 0;
volatile int testState = 0;
volatile int stopRequested = 0;
volatile uint64_t bytesSoFar;
volatile uint64_t testStart = 0;
volatile uint64_t rampBaseBytes = 0;
volatile uint64_t rampBaseTime = 0;
volatile int rampDone = 0;

#define RAMP_MS 2000

#define CONNECT_TIMEOUT_MS 4000
#define LATENCY_CONNECT_TIMEOUT_MS 2000

#define IPERF_PORT 5201
#define COOKIE_SIZE_BYTES 37
#define IPERF_LOG_MAX 8
#define IPERF_TIME_SEC 10
#define IPERF_BLKSIZE 131072

#define CONFIG_DIR       "ux0:data/vitaspeed"
#define CONFIG_PATH      "ux0:data/vitaspeed/config.txt"
#define DEFAULT_IPERF_IP "192.168.1.11"

char iperfServerIp[32] = DEFAULT_IPERF_IP;
int  configStatus = 0;   // 0 = default, 1 = loaded, 2 = bad IP in file, 3 = can't write

volatile int iperfState = 0;
volatile int iperfStage = 0;
volatile int iperfPhase = 0;   // 1 = handshaking, 2 = sending, 3 = finishing
volatile int iperfReverse = 0;   // 0 = upload, 1 = download

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
    addr.sin_port = sceNetHtons(IPERF_PORT);
    addr.sin_addr = ip;

    if (connectWithTimeout(iperfCtrl, (SceNetSockaddr *)&addr, sizeof(addr), CONNECT_TIMEOUT_MS) < 0) { iperfStage = 3; goto fail; }

    timeout_us = 5 * 1000 * 1000;   // 5 seconds
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
        IPERF_TIME_SEC, iperfReverse ? "\"reverse\":true," : "");

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

    if (!iperfReverse) {
        while (now - start < (uint64_t)IPERF_TIME_SEC * 1000000ULL && !stopRequested) {
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

        while (now - start < (uint64_t)IPERF_TIME_SEC * 1000000ULL && !stopRequested) {
            int n = sceNetRecv(iperfData, sendBuf, sizeof(sendBuf), 0);
            if (n > 0) { total += n; iperfBytes += n; }
            else if (n == 0) { ok = 0; break; }
            now = sceKernelGetProcessTimeWide();
        }
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

#define MAX_STREAMS 4
static const char *kDlHost = "speedtest.belwue.net";
static const char *kDlPath = "/100M";
static char dlBuf[MAX_STREAMS][32 * 1024];
volatile uint32_t dlBytes[MAX_STREAMS];
volatile int dlDone[MAX_STREAMS];
volatile uint64_t dlDeadline = 0;
static SceNetInAddr dlIp;
volatile int dlStreams = 3;

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
             "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", kDlPath, kDlHost);
    if (sceNetSend(sock, req, strlen(req), 0) < 0) {
        sceNetSocketClose(sock);
        dlDone[idx] = 1;
        return sceKernelExitDeleteThread(0);
    }

    uint64_t now = sceKernelGetProcessTimeWide();
    while (now < dlDeadline && !stopRequested) {
        int n = sceNetRecv(sock, dlBuf[idx], sizeof(dlBuf[idx]), 0);
        if (n > 0) dlBytes[idx] += n;
        else if (n == 0) break;
        now = sceKernelGetProcessTimeWide();
    }

    sceNetSocketClose(sock);
    dlDone[idx] = 1;
    return sceKernelExitDeleteThread(0);
}

double downloadTest() {
    if (resolveHost(kDlHost, &dlIp) != 0) { failStage = 1; return -1; }

    int n = dlStreams;
    if (n < 1) n = 1;
    if (n > MAX_STREAMS) n = MAX_STREAMS;

    for (int i = 0; i < MAX_STREAMS; i++) { dlBytes[i] = 0; dlDone[i] = 0; }

    uint64_t start = sceKernelGetProcessTimeWide();
    testStart = start;
    rampBaseBytes = 0;
    rampBaseTime = start;
    rampDone = 0;
    bytesSoFar = 0;
    dlDeadline = start + 10000000ULL;   // 10 s

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

    while (sceKernelGetProcessTimeWide() < dlDeadline && !stopRequested) {
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
        if (stopRequested) break;
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
    getAvgLatency(5, minLatency, avgLatency, maxLatency);
    speed = downloadTest();
    testState = 2;
    return sceKernelExitDeleteThread(0);
}

static void startInternetTest() {
    sceNetCtlInetGetState(&state);
    if (testState != 1) {
        testState = 1;
        SceUID thid = sceKernelCreateThread("test", testThread, 0x10000100, 0x10000, 0, 0, NULL);
        if (thid >= 0) sceKernelStartThread(thid, 0, NULL);
        else testState = 0;
    }
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
        uint64_t baseBytes = 0, baseTime = testStart;
        if (rampDone) { baseBytes = rampBaseBytes; baseTime = rampBaseTime; }
        if (bytes > baseBytes) {
            double secs = (sceKernelGetProcessTimeWide() - baseTime) / 1000000.0;
            if (secs > 0) vita2d_pgf_draw_textf(font, 50, 180, RGBA8(255, 255, 255, 255), 1, "Measuring: %.1f Mbps", (bytes - baseBytes) * 8 / secs / 1000000.0);
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
        vita2d_pgf_draw_textf(font, 50, 260, RGBA8(200, 200, 200, 255), 1,
                        "RCVBUF want %d KB, got %d KB (set=%d get=%d)",
                        kRcvBufBytes / 1024, rcvBufEff / 1024, rcvBufSetRet, rcvBufGetRet);
        vita2d_pgf_draw_textf(font, 50, 300, RGBA8(180, 180, 180, 255), 1,
                        "Streams: %d (L/R to change, X to re-run)", dlStreams);
    }
    else {
        vita2d_pgf_draw_text(font, 50, 140, RGBA8(255, 255, 255, 255), 1, "Test is idle.");
        vita2d_pgf_draw_textf(font, 50, 180, RGBA8(180, 180, 180, 255), 1,
                        "Streams: %d (L/R to change)", dlStreams);
    }
}

void drawIperf(vita2d_pgf *font) {
    sceNetCtlInetGetState(&state);
    vita2d_pgf_draw_text(font, 50, 100, RGBA8(255, 255, 255, 255), 1, "iperf3 Speedtest");
    const char *src = configStatus == 1 ? "config"
                    : configStatus == 2 ? "config invalid, using default"
                    : configStatus == 3 ? "can't write config"
                    :                     "default";
    vita2d_pgf_draw_textf(font, 50, 140, RGBA8(180, 180, 180, 255), 1,
                    "Server: %s:%d (%s)", iperfServerIp, IPERF_PORT, src);
    vita2d_pgf_draw_textf(font, 50, 180, RGBA8(180, 180, 180, 255), 1, "Mode: %s (Square to change)",
                    iperfReverse ? "Download" : "Upload");

    if (iperfState == 1) {
        if (iperfPhase == 1) {
            vita2d_pgf_draw_text(font, 50, 220, RGBA8(255, 255, 255, 255), 1, "Handshaking...");
        }
        else if (iperfPhase == 2) {
            vita2d_pgf_draw_text(font, 50, 220, RGBA8(255, 255, 255, 255), 1,
                            iperfReverse ? "Receiving..." : "Sending...");
            uint64_t bytes = iperfBytes;
            if (bytes > 0) {
                double secs = (sceKernelGetProcessTimeWide() - iperfStart) / 1000000.0;
                if (secs > 0)
                    vita2d_pgf_draw_textf(font, 50, 260, RGBA8(0, 255, 0, 255), 1,
                                    iperfReverse ? "Download: %.1f Mbps" : "Upload: %.1f Mbps",
                                    bytes * 8 / secs / 1000000.0);
            }
        }
        else {
            vita2d_pgf_draw_text(font, 50, 220, RGBA8(255, 255, 255, 255), 1, "Finishing...");
        }
    }
    else if (iperfState == 2) {
        if (iperfStage != 0) {
            vita2d_pgf_draw_textf(font, 50, 220, RGBA8(255, 80, 80, 255), 1,
                            "Local failure at stage %d", iperfStage);
        }

        char line[128];
        int off = snprintf(line, sizeof(line), "States:");
        for (int i = 0; i < iperfLogCount && off < (int)sizeof(line); i++)
            off += snprintf(line + off, sizeof(line) - off, " %d", (int)iperfLog[i]);
        vita2d_pgf_draw_text(font, 50, 260, RGBA8(255, 255, 255, 255), 1, line);

        int last = (iperfLogCount > 0) ? iperfLog[iperfLogCount - 1] : -1;
        if (last == 14)
            vita2d_pgf_draw_text(font, 50, 300, RGBA8(0, 255, 0, 255), 1, "DISPLAY_RESULTS - done");
        else if (last == 13)
            vita2d_pgf_draw_text(font, 50, 300, RGBA8(255, 200, 0, 255), 1, "EXCHANGE_RESULTS");
        else if (last == 2)
            vita2d_pgf_draw_text(font, 50, 300, RGBA8(255, 200, 0, 255), 1, "TEST_RUNNING");

        if (iperfSpeed > 0)
            vita2d_pgf_draw_textf(font, 50, 340, RGBA8(0, 255, 0, 255), 1,
                            iperfReverse ? "Download: %.1f Mbps" : "Upload: %.1f Mbps", iperfSpeed);
        if (iperfPeerSpeed > 0)
            vita2d_pgf_draw_textf(font, 50, 380, RGBA8(0, 255, 0, 255), 1,
                            iperfReverse ? "Server sent: %.1f Mbps" : "Receiver: %.1f Mbps", iperfPeerSpeed);
    }
    else {
        vita2d_pgf_draw_text(font, 50, 220, RGBA8(255, 255, 255, 255), 1, "Press X to start handshake.");
    }
}

static void loadConfig() {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        sceIoMkdir(CONFIG_DIR, 0777);
        f = fopen(CONFIG_PATH, "w");
        if (f) { fprintf(f, "%s\n", DEFAULT_IPERF_IP); fclose(f); configStatus = 0; }
        else configStatus = 3;
        return;
    }

    char line[64];
    configStatus = 2;
    if (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r' || line[n-1] == ' '))
            line[--n] = '\0';

        SceNetInAddr test;
        if (n > 0 && n < sizeof(iperfServerIp) &&
            sceNetInetPton(SCE_NET_AF_INET, line, &test) > 0) {
            strcpy(iperfServerIp, line);
            configStatus = 1;
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
                if (chosen == 0) startInternetTest();
            }
        }
        else {
            if (pressed & SCE_CTRL_CIRCLE) chosen = -1;
            if (chosen == 0 && testState != 1) {
                if ((pressed & SCE_CTRL_LTRIGGER) && dlStreams > 1) dlStreams--;
                if ((pressed & SCE_CTRL_RTRIGGER) && dlStreams < MAX_STREAMS) dlStreams++;
                if (pressed & SCE_CTRL_CROSS) startInternetTest();
            }
            if (chosen == 1 && (pressed & SCE_CTRL_SQUARE) && iperfState != 1)
                iperfReverse = !iperfReverse;
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