// ALSA backend for Linux — system library, no external deps
#ifdef __linux__
#include "audio.h"
#include <alsa/asoundlib.h>
#include <cstdio>
#include <poll.h>

class AlsaDevice : public AudioDevice {
public:
    ~AlsaDevice() override { close(); }

    bool open(const char* device, int sample_rate, bool capture, bool playback) override {
        sample_rate_ = sample_rate;
        const char* dev = (device && device[0]) ? device : "default";

        if (capture) {
            int err = snd_pcm_open(&cap_, dev, SND_PCM_STREAM_CAPTURE, 0);
            if (err < 0) {
                fprintf(stderr, "[Audio] Cannot open capture '%s': %s\n", dev, snd_strerror(err));
                return false;
            }
            if (!configure(cap_, sample_rate)) { snd_pcm_close(cap_); cap_ = nullptr; return false; }
            snd_pcm_start(cap_);
        }

        if (playback) {
            int err = snd_pcm_open(&play_, dev, SND_PCM_STREAM_PLAYBACK, 0);
            if (err < 0) {
                fprintf(stderr, "[Audio] Cannot open playback '%s': %s\n", dev, snd_strerror(err));
                return false;
            }
            if (!configure(play_, sample_rate)) { snd_pcm_close(play_); play_ = nullptr; return false; }
        }

        return true;
    }

    int read(int16_t* buf, int frames) override {
        if (!cap_) return 0;
        snd_pcm_sframes_t n = snd_pcm_readi(cap_, buf, frames);
        if (n == -EPIPE) {
            snd_pcm_prepare(cap_);
            n = snd_pcm_readi(cap_, buf, frames);
        }
        return (n > 0) ? (int)n : 0;
    }

    int write(const int16_t* buf, int frames) override {
        if (!play_) return 0;
        snd_pcm_sframes_t n = snd_pcm_writei(play_, buf, frames);
        if (n == -EPIPE) {
            snd_pcm_prepare(play_);
            n = snd_pcm_writei(play_, buf, frames);
        }
        return (n > 0) ? (int)n : 0;
    }

    void flush() override {
        if (play_) snd_pcm_drain(play_);
    }

    void close() override {
        if (cap_) { snd_pcm_close(cap_); cap_ = nullptr; }
        if (play_) { snd_pcm_close(play_); play_ = nullptr; }
    }

    int capture_fd() const override {
        if (!cap_) return -1;
        struct pollfd pfd;
        if (snd_pcm_poll_descriptors(cap_, &pfd, 1) == 1)
            return pfd.fd;
        return -1;
    }

private:
    snd_pcm_t* cap_ = nullptr;
    snd_pcm_t* play_ = nullptr;

    bool configure(snd_pcm_t* pcm, int rate) {
        snd_pcm_hw_params_t* hw;
        snd_pcm_hw_params_alloca(&hw);
        snd_pcm_hw_params_any(pcm, hw);
        snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
        snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
        snd_pcm_hw_params_set_channels(pcm, hw, 1);
        unsigned int r = (unsigned int)rate;
        snd_pcm_hw_params_set_rate_near(pcm, hw, &r, nullptr);
        snd_pcm_uframes_t period = 1024;
        snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, nullptr);
        int err = snd_pcm_hw_params(pcm, hw);
        if (err < 0) {
            fprintf(stderr, "[Audio] ALSA hw_params failed: %s\n", snd_strerror(err));
            return false;
        }
        return true;
    }
};

AudioDevice* AudioDevice::create() {
    return new AlsaDevice();
}

int AudioDevice::list_devices() {
    printf("%-4s  %-20s  %-40s  %-5s  %-5s\n", "#", "ALSA Device", "Description", "IN", "OUT");
    printf("%-4s  %-20s  %-40s  %-5s  %-5s\n", "---", "--------------------", "----------------------------------------", "-----", "-----");

    int listed = 0;

    // List PCM devices via ALSA hints
    void** hints = nullptr;
    if (snd_device_name_hint(-1, "pcm", &hints) < 0) {
        printf("Cannot enumerate ALSA devices.\n");
        return 0;
    }

    for (void** h = hints; *h; h++) {
        char* name = snd_device_name_get_hint(*h, "NAME");
        char* desc = snd_device_name_get_hint(*h, "DESC");
        char* ioid = snd_device_name_get_hint(*h, "IOID");

        if (!name) continue;

        // Skip null and complex virtual devices
        if (strncmp(name, "null", 4) == 0) { free(name); free(desc); free(ioid); continue; }

        bool has_input = true, has_output = true;
        if (ioid) {
            if (strcmp(ioid, "Input") == 0) has_output = false;
            if (strcmp(ioid, "Output") == 0) has_input = false;
        }

        // Clean up description (replace newlines with spaces)
        char clean_desc[80] = "";
        if (desc) {
            int j = 0;
            for (int i = 0; desc[i] && j < 79; i++) {
                if (desc[i] == '\n') { if (j > 0 && clean_desc[j-1] != ' ') clean_desc[j++] = ' '; }
                else clean_desc[j++] = desc[i];
            }
            clean_desc[j] = 0;
        }
        // Truncate long descriptions
        if (strlen(clean_desc) > 40) { clean_desc[37] = '.'; clean_desc[38] = '.'; clean_desc[39] = '.'; clean_desc[40] = 0; }

        printf("%-4d  %-20s  %-40s  %-5s  %-5s\n",
               listed, name, clean_desc,
               has_input  ? "yes" : "-",
               has_output ? "yes" : "-");
        listed++;

        free(name);
        free(desc);
        free(ioid);
    }
    snd_device_name_free_hint(hints);

    // Also list hardware cards
    printf("\nHardware cards:\n");
    int card = -1;
    while (snd_card_next(&card) == 0 && card >= 0) {
        char* name = nullptr;
        snd_card_get_name(card, &name);
        printf("  hw:%d  %s\n", card, name ? name : "(unknown)");
        free(name);
    }

    printf("\nFound %d ALSA device(s).\n", listed);
    printf("\nUsage:  modemtnc -d \"<ALSA Device>\" ...\n");
    printf("  Common choices:\n");
    printf("    -d default          System default (usually works)\n");
    printf("    -d plughw:1,0       USB sound card (card 1, device 0) — auto format conversion\n");
    printf("    -d hw:1,0           USB sound card (card 1, device 0) — raw hardware access\n");
    return listed;
}

#endif // __linux__
