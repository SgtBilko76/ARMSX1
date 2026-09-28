#include "mcd.h"
#include "../log.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#ifndef PATH_MAX
/* Strict ISO C (the -std=c11 test builds) hides the POSIX definition. */
#define PATH_MAX 4096
#endif
#ifdef _WIN32
#include <direct.h>
#endif

static void psx_mcd_ensure_parent(const char* path) {
    if (!path)
        return;

#ifdef PSVITA_TARGET
    const size_t path_len = strlen(path);
    if (path_len == 0)
        return;

    char* tmp = malloc(path_len + 1);
    if (!tmp)
        return;

    memcpy(tmp, path, path_len + 1);
#else
    char tmp[PATH_MAX];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
#endif

    char* slash = strrchr(tmp, '/');
#ifdef _WIN32
    char* bslash = strrchr(tmp, '\\');
    if (!slash || (bslash && bslash > slash))
        slash = bslash;
#endif
    if (!slash || slash == tmp) {
#ifdef PSVITA_TARGET
        free(tmp);
#endif
        return;
    }

    *slash = '\0';

    struct stat st;
    if (stat(tmp, &st) == 0) {
#ifdef PSVITA_TARGET
        free(tmp);
#endif
        return;
    }

#ifdef _WIN32
    _mkdir(tmp);
#else
    mkdir(tmp, 0755);
#endif

#ifdef PSVITA_TARGET
    free(tmp);
#endif
}

psx_mcd_t* psx_mcd_create(void) {
    return (psx_mcd_t*)malloc(sizeof(psx_mcd_t));
}

int psx_mcd_init(psx_mcd_t* mcd, const char* path) {
    memset(mcd, 0, sizeof(psx_mcd_t));

    mcd->state = MCD_STATE_TX_HIZ;
    mcd->flag = 0x08;
    mcd->path = path;
    mcd->buf = malloc(MCD_MEMORY_SIZE);
    mcd->tx_data_ready = 0;

    memset(mcd->buf, 0, MCD_MEMORY_SIZE);

    if (!path)
        return 0;

    psx_mcd_ensure_parent(path);

    FILE* file = fopen(path, "rb");

    if (!file) {
        // Create a blank card if missing
        file = fopen(path, "wb");
        if (!file)
            return 1;

        fwrite(mcd->buf, 1, MCD_MEMORY_SIZE, file);
        fclose(file);

        // Re-open for read so subsequent logic flows
        file = fopen(path, "rb");
        if (!file)
            return 1;
    }

    if (!fread(mcd->buf, 1, MCD_MEMORY_SIZE, file))
        return 2;

    fclose(file);

    return 0;
}

uint8_t psx_mcd_read(psx_mcd_t* mcd) {
    switch (mcd->state) {
        case MCD_STATE_TX_HIZ: mcd->tx_data = 0xff; break;
        case MCD_STATE_TX_FLG:
            if ((mcd->rx_data == 0x81) || (mcd->rx_data == 0x01)) {
                mcd->tx_data_ready = 1;
                mcd->tx_data = 0xff;
                mcd->state = MCD_STATE_TX_HIZ;
                break;
            }

            mcd->tx_data = mcd->flag;
            mcd->flag = 0x00;
            break;
        case MCD_STATE_TX_ID1: mcd->tx_data = 0x5a; break;
        case MCD_STATE_TX_ID2: {
            mcd->tx_data_ready = 1;
            mcd->tx_data = 0x5d;

            switch (mcd->mode) {
                case 'R': mcd->state = MCD_R_STATE_RX_MSB; break;
                case 'W': mcd->state = MCD_W_STATE_RX_MSB; break;
                case 'S': mcd->state = MCD_S_STATE_TX_ACK1; break;
                default:
                    mcd->tx_data_ready = 0;
                    mcd->tx_data = 0xff;
                    mcd->state = MCD_STATE_TX_HIZ;
                    return mcd->tx_data;
            }

            // printf("mcd read %02x\n", mcd->tx_data);

            // log_set_quiet(0);
            // log_fatal("mcd read %02x", mcd->tx_data);
            // log_set_quiet(1);

            return mcd->tx_data;
        } break;

        // Read states
        case MCD_R_STATE_RX_MSB: mcd->tx_data = 0x00; break;
        case MCD_R_STATE_RX_LSB: mcd->tx_data = mcd->msb; break;
        case MCD_R_STATE_TX_ACK1: mcd->tx_data = 0x5c; break;
        case MCD_R_STATE_TX_ACK2: mcd->tx_data = 0x5d; break;
        case MCD_R_STATE_TX_MSB: mcd->tx_data = mcd->msb; mcd->checksum  = mcd->msb; break;
        case MCD_R_STATE_TX_LSB: mcd->tx_data = mcd->lsb; mcd->checksum ^= mcd->lsb;
                                 mcd->pending_bytes = 128; break;
        case MCD_R_STATE_TX_DATA: {
            --mcd->pending_bytes;

            uint8_t data = mcd->buf[mcd->addr++];

            mcd->checksum ^= data;

            if (!mcd->pending_bytes) {
                mcd->tx_data = data;

                break;
            }

            // printf("mcd read %02x\n", data);

            // log_set_quiet(0);
            // log_fatal("mcd read %02x", data);
            // log_set_quiet(1);

            return data;
        } break;
        case MCD_R_STATE_TX_CHK: mcd->tx_data = mcd->checksum; break;
        case MCD_R_STATE_TX_MEB: {
            mcd->tx_data_ready = 0;
            mcd->state = MCD_STATE_TX_HIZ;

            // log_set_quiet(0);
            // log_fatal("mcd read %02x", 'G');
            // log_set_quiet(1);

            // printf("mcd read %02x\n", 'G');

            return 'G';
        } break;

        /* Write states */
        case MCD_W_STATE_RX_MSB: mcd->tx_data = 0x00; break;
        case MCD_W_STATE_RX_LSB: mcd->tx_data = mcd->msb;
                                 mcd->pending_bytes = 128; break;
        case MCD_W_STATE_RX_DATA: {
             --mcd->pending_bytes;

            mcd->buf[mcd->addr++] = mcd->rx_data;

            if (!mcd->pending_bytes) {
                mcd->tx_data = mcd->rx_data;

                break;
            }

            // printf("mcd read %02x\n", mcd->rx_data);

            // log_set_quiet(0);
            // log_fatal("mcd read %02x", mcd->rx_data);
            // log_set_quiet(1);

            return mcd->rx_data;
        } break;
        case MCD_W_STATE_RX_CHK: mcd->tx_data = mcd->rx_data; break;
        case MCD_W_STATE_RX_CHK2: mcd->tx_data = mcd->rx_data; break;
        case MCD_W_STATE_TX_ACK1: mcd->tx_data = 0x5c; break;
        case MCD_W_STATE_TX_ACK2: mcd->tx_data = 0x5d; break;
        case MCD_W_STATE_TX_MEB: {
            mcd->tx_data_ready = 0;
            mcd->state = MCD_STATE_TX_HIZ;

            // log_set_quiet(0);
            // log_fatal("mcd read %02x", 'G');
            // log_set_quiet(1);

            // printf("mcd read %02x\n", 'G');

            return 'G';
        } break;
    }

    mcd->tx_data_ready = 1;
    mcd->state++;

    // log_set_quiet(0);
    // log_fatal("mcd read %02x", mcd->tx_data);
    // log_set_quiet(1);

    // printf("mcd read %02x\n", mcd->tx_data);

    return mcd->tx_data;
}

void psx_mcd_write(psx_mcd_t* mcd, uint8_t data) {
    mcd->rx_data = data;

    switch (mcd->state) {
        case MCD_STATE_TX_FLG: mcd->mode = data; break;
        case MCD_R_STATE_RX_MSB: mcd->msb = data; break;
        case MCD_R_STATE_RX_LSB: {
            mcd->lsb = data;
            mcd->addr = ((mcd->msb << 8) | mcd->lsb) << 7;
        } break;
        case MCD_W_STATE_RX_MSB: mcd->msb = data; break;
        case MCD_W_STATE_RX_LSB: {
            mcd->lsb = data;
            mcd->addr = ((mcd->msb << 8) | mcd->lsb) << 7;
        } break;
        case MCD_W_STATE_RX_DATA: break;
        case MCD_W_STATE_RX_CHK: /* Don't care */ break;
        case MCD_W_STATE_RX_CHK2: /* Don't care */ break;
    }
}

int psx_mcd_query(psx_mcd_t* mcd) {
    return mcd->tx_data_ready;
}

void psx_mcd_reset(psx_mcd_t* mcd) {
    mcd->state = MCD_STATE_TX_HIZ;
}

void psx_mcd_destroy(psx_mcd_t* mcd) {
    FILE* file = mcd->path ? fopen(mcd->path, "wb") : NULL;

    if (file) {
        fwrite(mcd->buf, 1, MCD_MEMORY_SIZE, file);
        fclose(file);
    }

    free(mcd->buf);
    free(mcd);
}
