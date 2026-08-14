/**
 * audio_decoder.c — IMA-ADPCM mono, aritmetica entera pura.
 * Sin malloc, sin float, IRAM-friendly. Ver audio_decoder.h para el formato.
 */

#include "audio_decoder.h"

/* Tablas IMA/DVI ADPCM estandar (89 steps, ajuste de indice por nibble) */
static const int16_t ima_step_table[89] = {
        7,     8,     9,    10,    11,    12,    13,    14,
       16,    17,    19,    21,    23,    25,    28,    31,
       34,    37,    41,    45,    50,    55,    60,    66,
       73,    80,    88,    97,   107,   118,   130,   143,
      157,   173,   190,   209,   230,   253,   279,   307,
      337,   371,   408,   449,   494,   544,   598,   658,
      724,   796,   876,   963,  1060,  1166,  1282,  1411,
     1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,
     3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,
     7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767
};

static const int8_t ima_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

static inline int16_t clamp_s16(int32_t v)
{
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

/* Decodifica un nibble (0..15) y actualiza predictor/step_index.
 * Variante multiplicativa ((2*delta+1)*step)>>3 — identica a ffmpeg
 * (adpcm_ima_wav), validada bit-exacta contra el corpus OEM. */
static inline int16_t ima_decode_nibble(audio_decoder_t *dec, uint8_t nibble)
{
    int32_t step = ima_step_table[dec->step_index];
    int32_t diff = ((((int32_t)(nibble & 7)) << 1) + 1) * step >> 3;

    int32_t pred = (nibble & 8) ? (int32_t)dec->predictor - diff
                                : (int32_t)dec->predictor + diff;
    dec->predictor = clamp_s16(pred);

    int32_t idx = (int32_t)dec->step_index + ima_index_table[nibble];
    if (idx < 0)  idx = 0;
    if (idx > 88) idx = 88;
    dec->step_index = (int8_t)idx;

    return dec->predictor;
}

/* Carga el header del proximo bloque. Devuelve false si no hay mas datos. */
static bool load_block_header(audio_decoder_t *dec)
{
    if (dec->byte_pos + 4 > dec->blob_size)
        return false;   /* no queda ni el header: fin del clip */

    dec->predictor  = (int16_t)((uint16_t)dec->blob[dec->byte_pos]
                               | ((uint16_t)dec->blob[dec->byte_pos + 1] << 8));
    dec->step_index = (int8_t)dec->blob[dec->byte_pos + 2];
    if (dec->step_index < 0)  dec->step_index = 0;
    if (dec->step_index > 88) dec->step_index = 88;
    /* blob[pos+3] = reservado, se ignora */

    size_t avail = dec->blob_size - (dec->byte_pos + 4);
    size_t nibble_area = AUDIO_DECODER_BLOCK_SIZE - 4;
    dec->block_data_left = (avail < nibble_area) ? avail : nibble_area;
    dec->byte_pos += 4;
    dec->nib_high = false;
    return true;
}

void audio_decoder_init(audio_decoder_t *dec, const uint8_t *blob, size_t size)
{
    dec->blob            = blob;
    dec->blob_size       = size;
    dec->byte_pos        = 0;
    dec->predictor       = 0;
    dec->step_index      = 0;
    dec->nib_buf         = 0;
    dec->nib_high        = false;
    dec->block_data_left = 0;
    dec->need_header     = true;   /* fuerza carga del primer header */
}

void audio_decoder_rewind(audio_decoder_t *dec)
{
    audio_decoder_init(dec, dec->blob, dec->blob_size);
}

size_t audio_decoder_read(audio_decoder_t *dec, int16_t *out, size_t max_samples)
{
    size_t produced = 0;

    while (produced < max_samples) {
        if (dec->need_header) {
            if (!load_block_header(dec))
                break;                       /* fin del clip */
            out[produced++] = dec->predictor;  /* sample 0 = predictor del header */
            dec->need_header = false;
            continue;
        }

        /* Bloque agotado SOLO si no queda un nibble alto pendiente del
           ultimo byte leido (si no, ese nibble se perderia: -1 sample/bloque) */
        if (dec->block_data_left == 0 && !dec->nib_high) {
            dec->need_header = true;         /* bloque agotado: proximo header */
            continue;
        }

        if (!dec->nib_high) {
            dec->nib_buf = dec->blob[dec->byte_pos++];
            dec->block_data_left--;
            out[produced++] = ima_decode_nibble(dec, dec->nib_buf & 0x0F);
            dec->nib_high = true;
        } else {
            out[produced++] = ima_decode_nibble(dec, dec->nib_buf >> 4);
            dec->nib_high = false;
        }
    }
    return produced;
}

size_t audio_decoder_total_samples(size_t blob_size)
{
    size_t full = blob_size / AUDIO_DECODER_BLOCK_SIZE;
    size_t rem  = blob_size % AUDIO_DECODER_BLOCK_SIZE;
    size_t n    = full * AUDIO_DECODER_BLOCK_SAMPLES;
    if (rem > 4)
        n += 1 + (rem - 4) * 2;
    else if (rem > 0)
        n += 0;   /* bloque degenerado: solo header parcial, sin samples */
    return n;
}

uint32_t audio_decoder_duration_ms(size_t blob_size)
{
    return (uint32_t)(audio_decoder_total_samples(blob_size) * 1000ULL
                      / AUDIO_DECODER_SAMPLE_RATE);
}
