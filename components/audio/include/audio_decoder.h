/**
 * audio_decoder.h — Decoder IMA-ADPCM (WAVE_FORMAT_IMA_ADPCM, 0x11) mono 8 kHz
 * para los clips del pack de audio (proyecto ES24F, stage 4).
 *
 * Formato del blob (crudo, sin header WAV — el builder lo elimina):
 *   Secuencia de bloques de AUDIO_DECODER_BLOCK_SIZE (256) bytes:
 *     [predictor s16 LE][step_index u8][reservado u8][252 bytes de nibbles]
 *   El primer sample del bloque es el predictor del header; luego 504
 *   samples de nibbles (nibble BAJO primero dentro de cada byte).
 *   Total por bloque completo: 505 samples. Ultimo bloque puede venir corto.
 *
 * Uso tipico (audio_player):
 *   audio_decoder_init(&dec, blob_ram, blob_size);
 *   while ((n = audio_decoder_read(&dec, buf, BUF_LEN)) > 0) { ... }
 *
 * Validado bit-exacto contra ffmpeg (adpcm_ima_wav) sobre los 1004 clips
 * del dump OEM ZB25VQ64C.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_DECODER_BLOCK_SIZE      256   /* bytes por bloque (block_align) */
#define AUDIO_DECODER_BLOCK_SAMPLES   505   /* samples por bloque completo   */
#define AUDIO_DECODER_SAMPLE_RATE     8000  /* Hz, fijo en todo el pack       */

typedef struct {
    const uint8_t *blob;        /* blob ADPCM en RAM (cargado por audio_assets) */
    size_t         blob_size;   /* tamano total del blob en bytes               */
    size_t         byte_pos;    /* proximo byte a consumir                      */
    int16_t        predictor;   /* predictor IMA (sample anterior)              */
    int8_t         step_index;  /* indice de step (0..88)                       */
    uint8_t        nib_buf;     /* byte en curso (2 nibbles)                    */
    bool           nib_high;    /* true si toca el nibble alto de nib_buf       */
    size_t         block_data_left; /* bytes de nibbles restantes en el bloque  */
    bool           need_header; /* true = hay que cargar el header de bloque    */
} audio_decoder_t;

/** Inicializa el decoder sobre un blob ADPCM crudo ya residente en RAM. */
void audio_decoder_init(audio_decoder_t *dec, const uint8_t *blob, size_t size);

/**
 * Decodifica hasta max_samples samples PCM s16 @ 8 kHz en out.
 * Devuelve la cantidad producida; 0 = fin del clip.
 * Idempotente y reanudable: pensado para alimentar un ring buffer.
 */
size_t audio_decoder_read(audio_decoder_t *dec, int16_t *out, size_t max_samples);

/** Reinicia el decoder al inicio del blob (mismo blob). */
void audio_decoder_rewind(audio_decoder_t *dec);

/** Cantidad total de samples del blob (calculo O(1), sin decodificar). */
size_t audio_decoder_total_samples(size_t blob_size);

/** Duracion del blob en milisegundos (para logs/UI). */
uint32_t audio_decoder_duration_ms(size_t blob_size);

#ifdef __cplusplus
}
#endif
