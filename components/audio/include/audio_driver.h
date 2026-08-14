/**
 * audio_driver.h — Driver de salida de audio para ESP32-S3 → CS8302.
 *
 * Backend v0: MCPWM complementario (2 GPIO en contrafase) + ISR de timer
 * a 32 kHz. El RC externo (1k + 8n2 por rama, fc≈20 kHz) reconstruye el
 * analogico diferencial que ve el CS8302 en INP/INN.
 *
 * Senal: duty 50% = silencio (CM 1,65 V, bloqueado por los Cin del OEM).
 * 100% de volumen = 3,3 Vpp por rama = paridad con el OEM medido.
 * En reposo (IDLE) el timer MCPWM se detiene y ambos GPIO quedan forzados
 * a GND: menos EMI junto al front-end MIFARE de 13,56 MHz y menos consumo.
 *
 * Secuencia SD con timing OEM: lead 30 ms antes del audio, tail 16 ms
 * despues; rampa anti-pop de ~4 ms al inicio y al cierre.
 *
 * El flujo es: task alimenta ring via audio_driver_write() (bloqueante);
 * la ISR consume 1 sample por tick. Nunca llamar desde otra ISR.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_DRIVER_SAMPLE_RATE       32000  /* stream interno (8k x4)  */
#define AUDIO_DRIVER_CARRIER_HZ        156250 /* portadora PWM           */
#define AUDIO_DRIVER_PWM_RESOLUTION_HZ 160000000 /* MCPWM PLL160M         */
#define AUDIO_DRIVER_PWM_PERIOD_TICKS  1024   /* 160 MHz / 156,25 kHz    */
#define AUDIO_DRIVER_SD_LEAD_MS        30     /* medido: 30 ms (scope)   */
#define AUDIO_DRIVER_SD_TAIL_MS        16     /* medido: 16 ms (scope)   */
#define AUDIO_DRIVER_RING_SAMPLES      4096   /* 128 ms @ 32 kHz         */

typedef struct {
    int      gpio_pwm_p;        /* salida PWM fase      -> RC -> INP  */
    int      gpio_pwm_n;        /* salida PWM contrafase -> RC -> INN */
    int      gpio_sd;           /* shutdown del CS8302 (HIGH = on)    */
    uint32_t carrier_hz;        /* 0 = AUDIO_DRIVER_CARRIER_HZ        */
    uint32_t pwm_resolution_hz; /* 0 = default (fallback auto a 40M)  */
    uint32_t pwm_period_ticks;  /* 0 = default                        */
    uint32_t sd_lead_ms;        /* 0 = 30 ms                          */
    uint32_t sd_tail_ms;        /* 0 = 16 ms                          */
    size_t   ring_samples;      /* 0 = 4096                           */
} audio_driver_config_t;

/** Inicializa MCPWM complementario + timer 32 kHz + GPIO SD (SD queda LOW). */
esp_err_t audio_driver_init(const audio_driver_config_t *cfg);

/**
 * Abre el canal: SD HIGH, espera lead, rampa anti-pop. No bloqueante:
 * la ISR maneja la secuencia; el llamador puede empezar a escribir ya.
 */
esp_err_t audio_driver_start(void);

/**
 * Cierra el canal: drena lo que quede en el ring, rampa abajo, tail, SD LOW.
 * No bloqueante; consultar audio_driver_idle() para saber cuando termino.
 */
esp_err_t audio_driver_stop(void);

/** true cuando el canal esta totalmente cerrado (SD LOW). */
bool audio_driver_idle(void);

/**
 * Escribe samples PCM s16 @ 32 kHz al ring. Bloquea hasta que entre todo
 * o expire timeout_ms. Devuelve samples escritos.
 */
size_t audio_driver_write(const int16_t *samples, size_t count, uint32_t timeout_ms);

/** Samples libres en el ring. */
size_t audio_driver_write_space(void);

/** Volumen digital 0..100 (100 = 3,3 Vpp/rama = paridad OEM). */
void audio_driver_set_volume(uint8_t vol);
uint8_t audio_driver_get_volume(void);

/** Contador de underruns (ISR no encontro samples) para diagnostico. */
uint32_t audio_driver_underruns(void);

/* ---------- diagnostico (harness / lab) ---------- */

/** Fuerza un duty fijo 0..100% (bypasea player/ring). -1 = off. */
void audio_driver_test_duty(int pct);

/** Estado del canal como string ("IDLE", "RUNNING", ...). */
const char *audio_driver_state_str(void);

/** Bytes de PCM actualmente en el ring. */
size_t audio_driver_bytes_in_ring(void);

/** Valor actual de la rampa anti-pop (0..256). */
int audio_driver_ramp(void);

/** true si la portadora PWM esta corriendo (false en reposo = GPIOs a GND). */
bool audio_driver_pwm_running(void);

#ifdef __cplusplus
}
#endif
