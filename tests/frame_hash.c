/*
    Determinism probe: boots a disc headless for a fixed number of frames and prints a hash of
    RAM, VRAM and CPU state at checkpoints. Run it before and after a core change that must not
    alter emulation; identical output means identical behaviour.

        make frame-hash BIOS=/path/bios.bin IMAGE=/path/game.cue FRAMES=1800

    FRAME_HASH_STEREO=1 runs with the stereo depth side channel on, which must not change a bit.
*/

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "psx/log.h"
#include "psx/psx.h"
#include "psx/dev/spu.h"
#include "psx/dev/cdrom/cdrom.h"

static uint64_t fnv1a(uint64_t hash, const void* data, size_t size) {
    const uint8_t* bytes = (const uint8_t*)data;
    for (size_t i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s bios image frames\n", argv[0]);
        return 2;
    }

    const int frames = atoi(argv[3]);
    log_set_quiet(1);
    psx_t* psx = psx_create();
    if (!psx || psx_init(psx, argv[1], NULL) != 0) {
        fprintf(stderr, "FRAME_HASH failed reason=init\n");
        return 1;
    }
    if (!psx_cdrom_open(psx_get_cdrom(psx), argv[2])) { /* returns 1 on success */
        fprintf(stderr, "FRAME_HASH failed reason=disc\n");
        return 1;
    }

    const char* stereo = getenv("FRAME_HASH_STEREO");
    if (stereo && stereo[0] == '1')
        psx_set_stereo_depth(psx, 1);

    /* FRAME_HASH_AUDIO=1: also mix audio exactly like the frontend (MixPsxAudio) and report how
       often it clips or wraps. Mixing advances the SPU, so hashes then differ from a plain run. */
    const char* audio_env = getenv("FRAME_HASH_AUDIO");
    const char* wav_path = getenv("FRAME_HASH_WAV"); /* also write the mix as 44.1 kHz stereo WAV */
    const int audio = (audio_env && audio_env[0] == '1') || (wav_path && wav_path[0]);
    FILE* wav = (wav_path && wav_path[0]) ? fopen(wav_path, "wb") : NULL;
    uint32_t wav_bytes = 0;
    if (wav) {
        static const uint8_t header[44] = {0};
        fwrite(header, 1, sizeof(header), wav);
    }
    unsigned long long spu_samples = 0, spu_clipped = 0, mix_wrapped = 0, cd_nonzero = 0, loud = 0;
    unsigned long long vol_negative = 0, vol_sweep = 0, voice_checks = 0;
    unsigned main_vol_seen[4] = {0, 0, 0, 0};
    int16_t buffer[735 * 2];

    const clock_t start = clock();
    for (int frame = 1; frame <= frames; frame++) {
        psx_run_frame(psx);

        if (audio) {
            psx_spu_t* spu = psx->spu;
            memset(buffer, 0, sizeof(buffer));
            psx_cdrom_get_audio_samples(psx->cdrom, buffer, sizeof(buffer));
            psx_spu_update_cdda_buffer(spu, psx->cdrom->cdda_buf);
            for (int i = 0; i < 735; i++) {
                for (int c = 0; c < 2; c++)
                    cd_nonzero += buffer[i * 2 + c] != 0;
                const uint32_t value = psx_spu_get_sample(spu);
                const int16_t sv[2] = {(int16_t)(value & 0xffff), (int16_t)(value >> 16)};
                for (int c = 0; c < 2; c++) {
                    spu_samples++;
                    spu_clipped += (sv[c] >= 32766 || sv[c] <= -32767);
                    const int sum = buffer[i * 2 + c] + sv[c];
                    if (wav) {
                        const int16_t out = (int16_t)(sum > 32767 ? 32767 : (sum < -32768 ? -32768 : sum));
                        fwrite(&out, sizeof(out), 1, wav);
                        wav_bytes += sizeof(out);
                    }
                    mix_wrapped += (sum > 32767 || sum < -32768);
                    loud += (sum > 24000 || sum < -24000);
                }
            }
            for (int v = 0; v < 24; v++) {
                if (!spu->data[v].playing)
                    continue;
                voice_checks++;
                vol_negative += ((spu->voice[v].volumel & 0xc000) == 0x4000) || ((spu->voice[v].volumer & 0xc000) == 0x4000);
                vol_sweep += ((spu->voice[v].volumel & 0x8000) != 0) || ((spu->voice[v].volumer & 0x8000) != 0);
            }
            main_vol_seen[(spu->mainlvol >> 14) & 3]++;
        }

        if ((frame % 300) == 0 || frame == frames) {
            uint64_t hash = 0xcbf29ce484222325ull;
            hash = fnv1a(hash, psx_get_ram(psx)->buf, psx_get_ram(psx)->size);
            hash = fnv1a(hash, psx_get_vram(psx), PSX_GPU_VRAM_SIZE);
            hash = fnv1a(hash, psx->cpu, offsetof(psx_cpu_t, bus));
            printf("FRAME_HASH frame=%d hash=%016llx\n", frame, (unsigned long long)hash);
        }
    }

    if (wav) {
        const uint32_t rate = 44100, byte_rate = rate * 4, riff = 36 + wav_bytes;
        const uint16_t pcm = 1, channels = 2, align = 4, bits = 16;
        const uint32_t fmt_size = 16;
        fseek(wav, 0, SEEK_SET);
        fwrite("RIFF", 1, 4, wav); fwrite(&riff, 4, 1, wav); fwrite("WAVEfmt ", 1, 8, wav);
        fwrite(&fmt_size, 4, 1, wav); fwrite(&pcm, 2, 1, wav); fwrite(&channels, 2, 1, wav);
        fwrite(&rate, 4, 1, wav); fwrite(&byte_rate, 4, 1, wav); fwrite(&align, 2, 1, wav);
        fwrite(&bits, 2, 1, wav); fwrite("data", 1, 4, wav); fwrite(&wav_bytes, 4, 1, wav);
        fclose(wav);
    }

    if (audio) {
        fprintf(stderr, "FRAME_HASH audio spu_samples=%llu spu_clipped=%llu mix_wrapped=%llu loud=%llu cd_nonzero=%llu "
            "voices=%llu vol_negative=%llu vol_sweep=%llu mainvol_top_bits=%u/%u/%u/%u\n",
            spu_samples, spu_clipped, mix_wrapped, loud, cd_nonzero, voice_checks, vol_negative, vol_sweep,
            main_vol_seen[0], main_vol_seen[1], main_vol_seen[2], main_vol_seen[3]);
    }

    const double seconds = (double)(clock() - start) / CLOCKS_PER_SEC;
    fprintf(stderr, "FRAME_HASH cpu_seconds=%.2f ms_per_frame=%.3f\n", seconds, seconds * 1000.0 / frames);
    return 0;
}
