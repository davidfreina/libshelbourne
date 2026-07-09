/*
 * shelbourne.c — Audio hardware abstraction library
 *
 * See shelbourne.h for API documentation.
 */

#define _FILE_OFFSET_BITS 64

#include "shelbourne.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* ── Device paths ─────────────────────────────────────────────────── */

#define AUDIO_DEV       "/dev/snd/snd_shelby"
#define SOURCE_SYSFS    "/sys/devices/virtual/sound/snd_shelby/source"
#define FB_DEV          "/dev/fb0"
#define KEYPAD_DEV      "/dev/shelby-keypad"
#define LED_WHITE       "/dev/shelby-leds/uledw"
#define LED_YELLOW      "/dev/shelby-leds/uledy"

#define CODEC_CFG_SYSFS   "/sys/class/sound/snd_aic3256/configured"
#define CODEC_FILE_SYSFS  "/sys/class/sound/snd_aic3256/configuredFile"
#define CODEC_PROC        "/proc/snd_aic3256"

#define GPIO_MUTE         "/dev/gpiodev/mute_out"
#define GPIO_AMP_POWER    "/dev/gpiodev/ampstby"
#define GPIO_DM870_POWER  "/dev/gpiodev/psm_disable"

/* ── Device detection ─────────────────────────────────────────────── */

/*
 * Codec firmware config filenames, one per supported device.
 * These live in /lib/firmware/ and configure the TLV320AIC3256 for the
 * speaker enclosure's specific EQ tuning. Exactly one file should exist
 * on any given device, which is how we identify the hardware at runtime.
 *
 * Build-time override: pass -DSHELBOURNE_DEVICE_SPOTTY or
 * -DSHELBOURNE_DEVICE_RHINO to skip detection and hard-code the target.
 */
typedef enum {
    SHELBOURNE_DEV_SPOTTY = 0,  /* SoundTouch 20 */
    SHELBOURNE_DEV_RHINO,       /* SoundTouch 10 */
    SHELBOURNE_DEV_COUNT
} shelbourne_dev_t;

static const char *const codec_cfg_by_dev[SHELBOURNE_DEV_COUNT] = {
    [SHELBOURNE_DEV_SPOTTY] = "spotty_scm_normal.cfg",
    [SHELBOURNE_DEV_RHINO]  = "rhino_sm2_normal.cfg",
};

static const char *const dev_name[SHELBOURNE_DEV_COUNT] = {
    [SHELBOURNE_DEV_SPOTTY] = "SoundTouch 20 (spotty)",
    [SHELBOURNE_DEV_RHINO]  = "SoundTouch 10 (rhino)",
};

/* Resolved once by shelbourne_init(); all subsequent calls read this. */
static shelbourne_dev_t g_device = SHELBOURNE_DEV_SPOTTY;

static shelbourne_dev_t detect_device(void)
{
#if defined(SHELBOURNE_DEVICE_SPOTTY)
    fprintf(stderr, "shelbourne: device forced to %s at build time\n",
            dev_name[SHELBOURNE_DEV_SPOTTY]);
    return SHELBOURNE_DEV_SPOTTY;
#elif defined(SHELBOURNE_DEVICE_RHINO)
    fprintf(stderr, "shelbourne: device forced to %s at build time\n",
            dev_name[SHELBOURNE_DEV_RHINO]);
    return SHELBOURNE_DEV_RHINO;
#else
    /* Probe /lib/firmware/ for each device's codec tuning file. */
    int i;
    for (i = 0; i < SHELBOURNE_DEV_COUNT; i++) {
        char path[128];
        snprintf(path, sizeof(path), "/lib/firmware/%s", codec_cfg_by_dev[i]);
        if (access(path, R_OK) == 0) {
            fprintf(stderr, "shelbourne: detected %s\n", dev_name[i]);
            return (shelbourne_dev_t)i;
        }
    }
    fprintf(stderr, "shelbourne: device detection failed, defaulting to %s\n",
            dev_name[SHELBOURNE_DEV_SPOTTY]);
    return SHELBOURNE_DEV_SPOTTY;
#endif
}

/* ── Internal buffer size ─────────────────────────────────────────── */

#define OUTBUF_FRAMES (SHELBOURNE_FRAMES_PER_WRITE * 32)  /* ~341ms at 48kHz */

/* ── Opaque struct ────────────────────────────────────────────────── */

struct shelbourne {
    /* Audio */
    int audio_fd;
    int32_t outbuf[OUTBUF_FRAMES * 2];  /* stereo interleaved */
    int outbuf_fill;                     /* frames buffered */
    struct timespec flush_next;
    int flush_started;

    /* AUX */
    int aux_active;
    int aux_read_fd;

    /* Display */
    int fb_fd;
    uint8_t fb[SHELBOURNE_FB_SIZE];

    /* Keypad */
    int keypad_fd;

    /* State */
    int in_standby;
};

/* ── Sysfs/file helpers ───────────────────────────────────────────── */

static int write_file(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    int len = strlen(value);
    int n = write(fd, value, len);
    close(fd);
    return (n == len) ? 0 : -1;
}

static int read_file_int(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    char buf[32];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    return atoi(buf);
}

/* ── Codec initialization ─────────────────────────────────────────── */

static int codec_read_reg(const char *cmd, unsigned int *val)
{
    write_file(CODEC_PROC, cmd);
    int fd = open(CODEC_PROC, O_RDONLY);
    if (fd < 0) return -1;
    char buf[256];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    char *rr = strstr(buf, "rr:");
    if (rr && sscanf(rr, "rr: %x", val) == 1)
        return 0;
    return -1;
}

static int codec_init_sysfs(void)
{
    unsigned int reg_val = 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0)
            fprintf(stderr, "shelbourne: codec sysfs retry %d...\n", attempt);

        write_file(CODEC_CFG_SYSFS, "0");
        for (int i = 0; i < 200; i++) {
            if (read_file_int(CODEC_CFG_SYSFS) == 0) break;
            usleep(10000);
        }

        write_file(CODEC_FILE_SYSFS, codec_cfg_by_dev[g_device]);

        int loaded = 0;
        for (int i = 0; i < 200; i++) {
            if (read_file_int(CODEC_CFG_SYSFS) == 1) { loaded = 1; break; }
            usleep(10000);
        }

        if (!loaded) {
            fprintf(stderr, "shelbourne: codec firmware load timeout\n");
            continue;
        }

        usleep(100000);
        write_file(CODEC_PROC, "fb");

        if (codec_read_reg("rr 2c 01", &reg_val) == 0 &&
            (reg_val == 0x06 || reg_val == 0x04)) {
            fprintf(stderr, "shelbourne: codec initialized via sysfs (rev 0x%02x)\n", reg_val);
            return 0;
        }
        fprintf(stderr, "shelbourne: codec verify failed (reg=0x%02x)\n", reg_val);
    }
    return -1;
}

static int codec_init_direct(void)
{
    char cfg_path[128];
    snprintf(cfg_path, sizeof(cfg_path), "/lib/firmware/%s", codec_cfg_by_dev[g_device]);
    FILE *f = fopen(cfg_path, "r");
    if (!f) {
        fprintf(stderr, "shelbourne: cannot open %s\n", cfg_path);
        return -1;
    }

    int page = 0, reg = 0, writes = 0;
    char line[256];

    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;

        unsigned int addr, r, val;

        if (sscanf(p, "w %x %x %x", &addr, &r, &val) == 3 && addr == 0x30) {
            if (r == 0x00) {
                page = val;
                reg = 0;
            } else if (r == 0xfe) {
                usleep(val * 1000);
            } else {
                char cmd[64];
                snprintf(cmd, sizeof(cmd), "rw %02x %02x %02x", page, r, val);
                write_file(CODEC_PROC, cmd);
                reg = r;
                writes++;
            }
        } else if (sscanf(p, "> %x", &val) == 1) {
            reg++;
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "rw %02x %02x %02x", page, reg, val);
            write_file(CODEC_PROC, cmd);
            writes++;
        }
    }

    fclose(f);
    fprintf(stderr, "shelbourne: direct register init: %d writes\n", writes);

    write_file(CODEC_PROC, "fb");

    unsigned int reg_val = 0;
    if (codec_read_reg("rr 2c 01", &reg_val) == 0 &&
        (reg_val == 0x06 || reg_val == 0x04)) {
        fprintf(stderr, "shelbourne: codec initialized via direct writes (rev 0x%02x)\n", reg_val);
        return 0;
    }
    fprintf(stderr, "shelbourne: codec direct init verify failed (reg=0x%02x)\n", reg_val);
    return -1;
}

/*
 * Enable the miniDSP audio path.
 *
 * The *_normal.cfg firmware loads the DSP's primary input-mixer volume
 * coefficient (page 0x2c, register 0x10) as zero on some enclosures — notably
 * rhino (SoundTouch 10) — which multiplies all audio by 0 inside the DSP and
 * produces total silence even though the codec, DAC and amplifier are fully
 * configured. The stock firmware sets this coefficient at runtime rather than
 * in the config (see /etc/init.d/dce_test_worker.sh: `aic cw 2c 10 400000`).
 * spotty's config already ships it non-zero, which is why spotty plays and
 * rhino did not.
 *
 * Write the stock unity coefficient so audio passes through the DSP; playback
 * volume is applied separately in software by the caller. The value 0x400000
 * is a 24-bit coefficient (the codec proc "cw" interface takes 24-bit values).
 */
static void codec_enable_dsp_output(void)
{
    write_file(CODEC_PROC, "cw 2c 10 400000");
    write_file(CODEC_PROC, "cw 2c 14 000000");
    fprintf(stderr, "shelbourne: DSP output coefficient set\n");
}

static int codec_init(void)
{
    write_file(GPIO_MUTE, "0");

    int rc;
    if (codec_init_sysfs() == 0) {
        rc = 0;
    } else {
        fprintf(stderr, "shelbourne: sysfs firmware load failed, trying direct writes...\n");
        rc = codec_init_direct();
    }

    if (rc == 0)
        codec_enable_dsp_output();
    return rc;
}

/* ── Amplifier control ────────────────────────────────────────────── */

static int amp_power_on(int wait)
{
    write_file(GPIO_AMP_POWER, "1");
    write_file(GPIO_DM870_POWER, "0");  /* active low */
    if (wait) {
        fprintf(stderr, "shelbourne: amp power on, stabilizing...\n");
        sleep(3);
    } else {
        fprintf(stderr, "shelbourne: amp power on\n");
    }
    return 0;
}

static void amp_power_off(void)
{
    write_file(GPIO_AMP_POWER, "0");
    write_file(GPIO_DM870_POWER, "1");  /* active low = off */
}

/* ── Kernel memory patching ───────────────────────────────────────── */

static void clear_tx_running(void)
{
    FILE *f = fopen("/sys/module/snd_shelby2/sections/.bss", "r");
    if (!f) { fprintf(stderr, "shelbourne: cannot read snd_shelby2 BSS\n"); return; }
    unsigned long bss_base;
    if (fscanf(f, "0x%lx", &bss_base) != 1) { fclose(f); return; }
    fclose(f);

    /*
     * txRunning is the guard flag snd_shelby_start_tx() checks before
     * starting the McASP TX EDMA: if it is non-zero, start_tx early-returns
     * and the DMA is never (re)started, so the next write() to the audio
     * device blocks forever. It lives at BSS+0xe4 in the snd_shelby2 module
     * (verified by disassembly on both sm2 devices; the previous 0xec pointed
     * at an unrelated variable). A clean close() clears it, but an unclean
     * exit (crash/SIGKILL) leaves it set, wedging the next run.
     */
    unsigned long addr = bss_base + 0xe4;
    int fd = open("/dev/kmem", O_RDWR);
    if (fd < 0) { fprintf(stderr, "shelbourne: cannot open /dev/kmem\n"); return; }

    if (lseek(fd, addr, SEEK_SET) >= 0) {
        uint32_t val;
        if (read(fd, &val, 4) == 4)
            fprintf(stderr, "shelbourne: txRunning was %u\n", val);
    }

    if (lseek(fd, addr, SEEK_SET) >= 0) {
        uint32_t zero = 0;
        write(fd, &zero, 4);
    }

    close(fd);
}

static void fix_keypad_timer(void)
{
    FILE *f = fopen("/proc/modules", "r");
    if (!f) return;
    uint32_t text_base = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "shelby_keypad")) {
            char *p = strstr(line, "0x");
            if (p) text_base = strtoul(p, NULL, 16);
            break;
        }
    }
    fclose(f);
    if (!text_base) return;

    uint32_t bss = text_base + 0x1ad8;

    int fd = open("/dev/kmem", O_RDWR);
    if (fd < 0) return;

    uint32_t gdev = 0;
    if (lseek(fd, (off_t)(bss + 0x68), SEEK_SET) >= 0)
        read(fd, &gdev, 4);

    if (gdev) {
        uint32_t timer_next = 0;
        if (lseek(fd, (off_t)(gdev + 0x80), SEEK_SET) >= 0)
            read(fd, &timer_next, 4);

        if (timer_next != 0) {
            uint32_t zero = 0;
            lseek(fd, (off_t)(gdev + 0x80), SEEK_SET);
            write(fd, &zero, 4);
            fprintf(stderr, "shelbourne: fixed keypad timer (was 0x%08x)\n", timer_next);
        }
    }
    close(fd);
}

/* ── Stock process management ────────────────────────────────────── */

static const char *stock_procs[] = {
    "APServer", "BoseApp", "STSCertified", "UpnpSource",
    "WebServer", "CLIServer", "NetManager", "scmmond",
    "SoftwareUpdate", "IoT", "TPDA", "LegacyProduct",
    "PtsServer", "microbswitch", "httpd", NULL
};

/* Check if a non-zombie process with the given name exists in /proc */
static int process_running(const char *name)
{
    DIR *proc = opendir("/proc");
    if (!proc) return 0;
    struct dirent *ent;
    while ((ent = readdir(proc)) != NULL) {
        if (ent->d_name[0] < '1' || ent->d_name[0] > '9')
            continue;
        char path[64];
        snprintf(path, sizeof(path), "/proc/%s/stat", ent->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        char buf[512];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) continue;
        buf[n] = '\0';
        /* Format: "pid (comm) state ..." — find last ')' for robust parsing */
        char *rp = strrchr(buf, ')');
        if (!rp || rp[1] != ' ') continue;
        if (rp[2] == 'Z') continue;  /* skip zombies */
        /* Extract comm between first '(' and last ')' */
        char *lp = strchr(buf, '(');
        if (!lp) continue;
        lp++;
        int len = rp - lp;
        if (len <= 0 || len != (int)strlen(name)) continue;
        if (memcmp(lp, name, len) == 0) {
            closedir(proc);
            return 1;
        }
    }
    closedir(proc);
    return 0;
}

int shelbourne_stock_processes_active(void)
{
    for (int i = 0; stock_procs[i]; i++) {
        if (process_running(stock_procs[i]))
            return 1;
    }
    return 0;
}

void shelbourne_stop_stock_processes(void)
{
    fprintf(stderr, "shelbourne: freezing process supervisor...\n");
    system("kill -STOP $(pidof shepherdd) 2>/dev/null");

    fprintf(stderr, "shelbourne: killing stock audio processes...\n");
    for (int i = 0; stock_procs[i]; i++) {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "killall -9 %s 2>/dev/null", stock_procs[i]);
        system(cmd);
    }

    sleep(1);
}

void shelbourne_resume_stock_processes(void)
{
    fprintf(stderr, "shelbourne: restarting stock processes...\n");
    /* SIGKILL the frozen shepherdd — SIGCONT would let it wake up and
     * trigger crash-recovery reboot (critical daemons like APServer and
     * BoseApp are configured with recovery="reboot").  SIGKILL is
     * delivered to stopped processes immediately. */
    system("kill -9 $(pidof shepherdd) 2>/dev/null");
    sleep(2);
    system("/etc/init.d/SoundTouch start 2>/dev/null");
}

/* ── Audio DMA start ──────────────────────────────────────────────── */

static int start_tx_mode(shelbourne_t *hw)
{
    clear_tx_running();

    hw->audio_fd = open(AUDIO_DEV, O_RDWR);
    if (hw->audio_fd < 0) {
        fprintf(stderr, "shelbourne: cannot open %s: %s\n", AUDIO_DEV, strerror(errno));
        return -1;
    }

    int sfd = open(SOURCE_SYSFS, O_WRONLY);
    if (sfd >= 0) {
        write(sfd, "tx", 2);
        close(sfd);
    } else {
        fprintf(stderr, "shelbourne: cannot open %s: %s\n", SOURCE_SYSFS, strerror(errno));
    }

    usleep(100000);
    return 0;
}

/* ── Lifecycle ────────────────────────────────────────────────────── */

shelbourne_t *shelbourne_init(void)
{
    shelbourne_t *hw = calloc(1, sizeof(*hw));
    if (!hw) return NULL;

    hw->audio_fd = -1;
    hw->aux_read_fd = -1;
    hw->fb_fd = -1;
    hw->keypad_fd = -1;

    g_device = detect_device();

    fix_keypad_timer();

    /* Mute and power off amplifier before codec init — if a previous
     * process left the amp on, reconfiguring the codec can cause
     * transients that trigger an amp fault and hardware reset. */
    write_file(GPIO_MUTE, "0");
    amp_power_off();

    /* Initialize codec */
    fprintf(stderr, "shelbourne: initializing codec...\n");
    if (codec_init() < 0) {
        fprintf(stderr, "shelbourne: codec init failed\n");
        free(hw);
        return NULL;
    }

    /* Power on amplifier (wait for stabilization on first init) */
    amp_power_on(1);

    /* Start TX mode (clears txRunning, opens device, configures McASP) */
    if (start_tx_mode(hw) < 0) {
        free(hw);
        return NULL;
    }

    /* Unmute */
    write_file(GPIO_MUTE, "1");

    /* Step 9: Open display (non-fatal) */
    hw->fb_fd = open(FB_DEV, O_RDWR);
    if (hw->fb_fd < 0)
        fprintf(stderr, "shelbourne: display not available: %s\n", strerror(errno));

    /* Step 10: Open keypad (non-fatal) */
    hw->keypad_fd = open(KEYPAD_DEV, O_RDONLY | O_NONBLOCK);
    if (hw->keypad_fd < 0)
        fprintf(stderr, "shelbourne: keypad not available: %s\n", strerror(errno));

    fprintf(stderr, "shelbourne: initialized\n");
    return hw;
}

void shelbourne_shutdown(shelbourne_t *hw)
{
    if (!hw) return;

    /* Mute */
    write_file(GPIO_MUTE, "0");

    /* Clear display */
    if (hw->fb_fd >= 0) {
        memset(hw->fb, 0, SHELBOURNE_FB_SIZE);
        lseek(hw->fb_fd, 0, SEEK_SET);
        write(hw->fb_fd, hw->fb, SHELBOURNE_FB_SIZE);
    }

    /* LED off */
    shelbourne_led_set(hw, SHELBOURNE_LED_OFF);

    /* Close fds */
    if (hw->aux_read_fd >= 0) close(hw->aux_read_fd);
    if (hw->audio_fd >= 0) close(hw->audio_fd);
    if (hw->fb_fd >= 0) close(hw->fb_fd);
    if (hw->keypad_fd >= 0) close(hw->keypad_fd);

    free(hw);
}

/* ── Audio output ─────────────────────────────────────────────────── */

int shelbourne_audio_write(shelbourne_t *hw, const int32_t *samples, int frames)
{
    if (hw->outbuf_fill + frames > OUTBUF_FRAMES)
        return -1;

    memcpy(hw->outbuf + hw->outbuf_fill * 2, samples, frames * 2 * sizeof(int32_t));
    hw->outbuf_fill += frames;
    return 0;
}

int shelbourne_audio_wait(shelbourne_t *hw)
{
    while (hw->outbuf_fill >= SHELBOURNE_FRAMES_PER_WRITE) {
        if (hw->flush_started) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long wait_us = (hw->flush_next.tv_sec - now.tv_sec) * 1000000L +
                           (hw->flush_next.tv_nsec - now.tv_nsec) / 1000;
            if (wait_us > 500)
                usleep(wait_us - 200);
        }

        ssize_t n = write(hw->audio_fd, hw->outbuf, SHELBOURNE_WRITE_SIZE);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }

        uint32_t ioctl_buf[16] = {0};
        ioctl(hw->audio_fd, 1, ioctl_buf);

        if (!hw->flush_started) {
            clock_gettime(CLOCK_MONOTONIC, &hw->flush_next);
            hw->flush_started = 1;
        }
        hw->flush_next.tv_nsec += 10666667;
        if (hw->flush_next.tv_nsec >= 1000000000) {
            hw->flush_next.tv_nsec -= 1000000000;
            hw->flush_next.tv_sec++;
        }

        hw->outbuf_fill -= SHELBOURNE_FRAMES_PER_WRITE;
        if (hw->outbuf_fill > 0)
            memmove(hw->outbuf, hw->outbuf + SHELBOURNE_FRAMES_PER_WRITE * 2,
                    hw->outbuf_fill * 2 * sizeof(int32_t));
    }
    return 0;
}

int shelbourne_audio_drain(shelbourne_t *hw)
{
    if (hw->outbuf_fill > 0) {
        while (hw->outbuf_fill < SHELBOURNE_FRAMES_PER_WRITE) {
            hw->outbuf[hw->outbuf_fill * 2] = 0;
            hw->outbuf[hw->outbuf_fill * 2 + 1] = 0;
            hw->outbuf_fill++;
        }
        return shelbourne_audio_wait(hw);
    }
    return 0;
}

void shelbourne_audio_reset(shelbourne_t *hw)
{
    hw->outbuf_fill = 0;
    hw->flush_started = 0;
}

/* ── AUX input ────────────────────────────────────────────────────── */

int shelbourne_aux_enable(shelbourne_t *hw)
{
    write_file(GPIO_MUTE, "0");

    /* Start AUX (RX) mode — TX stays running from init */
    int sfd = open(SOURCE_SYSFS, O_WRONLY);
    if (sfd < 0) {
        write_file(GPIO_MUTE, "1");
        return -1;
    }
    write(sfd, "aux", 3);
    close(sfd);
    usleep(200000);

    /* Open second fd for reading RX data */
    hw->aux_read_fd = open(AUDIO_DEV, O_RDWR);
    if (hw->aux_read_fd < 0) {
        fprintf(stderr, "shelbourne: cannot open %s for AUX read: %s\n",
                AUDIO_DEV, strerror(errno));
        write_file(GPIO_MUTE, "1");
        return -1;
    }

    write_file(GPIO_MUTE, "1");
    hw->aux_active = 1;
    shelbourne_audio_reset(hw);
    fprintf(stderr, "shelbourne: AUX enabled\n");
    return 0;
}

int shelbourne_aux_pump(shelbourne_t *hw, int gain)
{
    if (!hw->aux_active || hw->aux_read_fd < 0) return -1;

    uint8_t raw[SHELBOURNE_WRITE_SIZE];
    ssize_t n = read(hw->aux_read_fd, raw, sizeof(raw));
    if (n <= 0) {
        if (errno == EINTR) return 0;
        return -1;
    }

    /* Apply volume gain */
    if (gain != 65536 && gain >= 0) {
        int32_t *samples = (int32_t *)raw;
        int num_samples = n / (int)sizeof(int32_t);
        for (int i = 0; i < num_samples; i++)
            samples[i] = (int32_t)(((int64_t)samples[i] * gain) >> 16);
    }

    ssize_t w = write(hw->audio_fd, raw, n);
    if (w < 0) return -1;

    uint32_t ioctl_buf[16] = {0};
    ioctl(hw->audio_fd, 1, ioctl_buf);

    return 0;
}

int shelbourne_aux_disable(shelbourne_t *hw)
{
    write_file(GPIO_MUTE, "0");

    /* Close AUX read fd */
    if (hw->aux_read_fd >= 0) {
        close(hw->aux_read_fd);
        hw->aux_read_fd = -1;
    }

    /* Stop RX */
    int sfd = open(SOURCE_SYSFS, O_WRONLY);
    if (sfd >= 0) {
        write(sfd, "wifi", 4);
        close(sfd);
    }
    usleep(100000);

    /* Restart TX mode */
    clear_tx_running();
    sfd = open(SOURCE_SYSFS, O_WRONLY);
    if (sfd >= 0) {
        write(sfd, "tx", 2);
        close(sfd);
    }
    usleep(100000);

    write_file(GPIO_MUTE, "1");
    hw->aux_active = 0;
    shelbourne_audio_reset(hw);
    fprintf(stderr, "shelbourne: AUX disabled, TX restored\n");
    return 0;
}

/* ── Volume ───────────────────────────────────────────────────────── */

int shelbourne_volume_to_gain(int volume)
{
    if (volume <= 0) return 0;
    if (volume >= 100) return 65536;
    return (int)(65536.0 * pow(10.0, (volume - 100) / 30.0));
}

void shelbourne_mute(shelbourne_t *hw, int mute)
{
    (void)hw;
    write_file(GPIO_MUTE, mute ? "0" : "1");
}

/* ── Standby / Resume ─────────────────────────────────────────────── */

void shelbourne_standby(shelbourne_t *hw)
{
    write_file(GPIO_MUTE, "0");
    write_file(GPIO_AMP_POWER, "0");  /* amp off, but leave DM870 (WiFi) on */

    /* Blank display */
    if (hw->fb_fd >= 0) {
        uint8_t black[SHELBOURNE_FB_SIZE];
        memset(black, 0, sizeof(black));
        lseek(hw->fb_fd, 0, SEEK_SET);
        write(hw->fb_fd, black, sizeof(black));
    }

    shelbourne_led_set(hw, SHELBOURNE_LED_OFF);
    hw->in_standby = 1;
}

void shelbourne_resume(shelbourne_t *hw)
{
    amp_power_on(0);
    write_file(GPIO_MUTE, "1");
    shelbourne_audio_reset(hw);
    hw->in_standby = 0;
}

/* ── WiFi ────────────────────────────────────────────────────────── */

void shelbourne_wifi_set(shelbourne_t *hw, int on)
{
    (void)hw;
    write_file(GPIO_DM870_POWER, on ? "0" : "1");  /* active low */
}

/* ── Display ──────────────────────────────────────────────────────── */

void shelbourne_display_clear(shelbourne_t *hw)
{
    memset(hw->fb, 0, SHELBOURNE_FB_SIZE);
}

void shelbourne_display_pixel(shelbourne_t *hw, int x, int y, uint8_t val)
{
    if (x >= 0 && x < SHELBOURNE_FB_WIDTH && y >= 0 && y < SHELBOURNE_FB_HEIGHT)
        hw->fb[y * SHELBOURNE_FB_WIDTH + x] = val;
}

uint8_t *shelbourne_display_buffer(shelbourne_t *hw)
{
    if (hw->fb_fd < 0) return NULL;
    return hw->fb;
}

void shelbourne_display_flush(shelbourne_t *hw)
{
    if (hw->fb_fd < 0) return;
    lseek(hw->fb_fd, 0, SEEK_SET);
    write(hw->fb_fd, hw->fb, SHELBOURNE_FB_SIZE);
}

/* ── Keypad ───────────────────────────────────────────────────────── */

/*
 * Raw byte mapping from keypad driver:
 *   Press = key_index + 0x61, Release = key_index + 0x41
 */
static shelbourne_key_t decode_key(unsigned char raw)
{
    unsigned char index = (raw >= 0x61) ? raw - 0x61 : raw - 0x41;
    switch (index) {
    case 0:  return SHELBOURNE_KEY_VOLUME_UP;
    case 4:  return SHELBOURNE_KEY_VOLUME_DOWN;
    case 2:  return SHELBOURNE_KEY_POWER;
    case 12: return SHELBOURNE_KEY_AUX;
    case 6:  return SHELBOURNE_KEY_PRESET_1;
    case 5:  return SHELBOURNE_KEY_PRESET_2;
    case 1:  return SHELBOURNE_KEY_PRESET_3;
    case 10: return SHELBOURNE_KEY_PRESET_4;
    case 9:  return SHELBOURNE_KEY_PRESET_5;
    case 8:  return SHELBOURNE_KEY_PRESET_6;
    default: return SHELBOURNE_KEY_UNKNOWN;
    }
}

int shelbourne_keypad_poll(shelbourne_t *hw, shelbourne_key_event_t *events, int max_events)
{
    if (hw->keypad_fd < 0) return -1;

    unsigned char buf[64];
    int n = read(hw->keypad_fd, buf, sizeof(buf));
    if (n <= 0) return 0;

    int count = 0;
    for (int i = 0; i < n && count < max_events; i++) {
        unsigned char c = buf[i];
        if (c < 0x41) continue;  /* skip invalid bytes */
        events[count].key = decode_key(c);
        events[count].pressed = (c >= 0x61) ? 1 : 0;
        count++;
    }
    return count;
}

int shelbourne_keypad_fd(shelbourne_t *hw)
{
    return hw->keypad_fd;
}

/* ── LED ──────────────────────────────────────────────────────────── */

static void led_write(const char *dev, uint8_t val)
{
    int fd = open(dev, O_WRONLY);
    if (fd < 0) return;
    write(fd, &val, 1);
    close(fd);
}

void shelbourne_led_set(shelbourne_t *hw, shelbourne_led_state_t state)
{
    (void)hw;
    switch (state) {
    case SHELBOURNE_LED_OFF:
        led_write(LED_WHITE, 0x00);
        led_write(LED_YELLOW, 0x00);
        break;
    case SHELBOURNE_LED_WHITE:
        led_write(LED_YELLOW, 0x00);
        led_write(LED_WHITE, 0xFF);
        break;
    case SHELBOURNE_LED_YELLOW:
        led_write(LED_WHITE, 0x00);
        led_write(LED_YELLOW, 0xFF);
        break;
    }
}
