/**
 * audio_player.h — Reproductor de prompts: decodifica ADPCM, upsamplea
 * x4 a 32 kHz y alimenta el audio_driver. Soporta prompts COMPUESTOS
 * (lista de ids encadenados sin gaps — patron de menus del OEM) y
 * audio_say_number() para hablar numeros con los clips de digitos.
 *
 * El acceso a los clips se inyecta por callback (get_blob): el harness v0
 * usa arrays embebidos; la v1 lo conecta a la particion `audio`. El player
 * no sabe de donde salen los bytes.
 *
 * Uso:
 *   audio_player_init(&(audio_player_config_t){ .get_blob = mi_provider });
 *   audio_play_id(13);                        // beep
 *   audio_play_seq((uint16_t[]){72,121}, 2);  // "uno" + "anadir usuario"
 *   audio_say_number(123);                    // "uno dos tres"
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_PLAYER_MAX_SEQ   16    /* ids max por prompt compuesto */

/* Digitos en el pack (OEM): 0..8 -> ids 71..79, 9 -> id 90 */
#define AUDIO_ID_DIGIT_BASE    71
#define AUDIO_ID_DIGIT_NINE    90

/**
 * Proveedor de clips: dado un id devuelve puntero/tamano del blob ADPCM.
 * El puntero debe seguir valido durante toda la reproduccion del clip
 * (arrays embebidos o buffer propio de la capa de assets).
 * Devuelve false si el id no existe.
 */
typedef bool (*audio_get_blob_fn)(uint16_t id, const uint8_t **ptr,
                                  size_t *size);

typedef struct {
    audio_get_blob_fn get_blob;   /* obligatorio */
    uint32_t task_stack;          /* 0 = 4096    */
    uint32_t task_prio;           /* 0 = 10      */
    uint32_t queue_len;           /* 0 = 4       */
} audio_player_config_t;

esp_err_t audio_player_init(const audio_player_config_t *cfg);

/** Encola un clip. ESP_ERR_INVALID_STATE si la cola esta llena. */
esp_err_t audio_play_id(uint16_t id);

/** Encola un prompt compuesto (ids encadenados sin gap). */
esp_err_t audio_play_seq(const uint16_t *ids, size_t n);

/**
 * Encola un prompt compuesto y lo repite `times` veces (driver abre y
 * cierra una sola vez). times = 0 -> repite hasta audio_stop()
 * (modo banco de pruebas para scope).
 */
esp_err_t audio_play_times(const uint16_t *ids, size_t n, uint32_t times);

/** Compone un numero digito a digito (0..9999). */
esp_err_t audio_say_number(uint32_t number);

/** Corta la reproduccion en curso y vacia la cola. */
void audio_stop(void);

/** true si esta reproduciendo o hay pedidos en cola. */
bool audio_is_busy(void);

/** Volumen digital 0..100 (delega en el driver). */
void audio_set_volume(uint8_t vol);

#ifdef __cplusplus
}
#endif
