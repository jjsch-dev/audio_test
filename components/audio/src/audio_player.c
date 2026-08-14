/**
 * audio_player.c — Task reproductor. Ver audio_player.h.
 *
 * Pipeline por clip: blob ADPCM -> audio_decoder (8 kHz s16) ->
 * upsample x4 lineal -> audio_driver_write (32 kHz s16, bloqueante).
 * Los clips de un prompt compuesto se encadenan sin gap.
 */

#include "audio_player.h"
#include "audio_decoder.h"
#include "audio_driver.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

static const char *TAG = "audio_player";

#define DEC_CHUNK   128    /* samples 8 kHz por pasada de decode */
#define UP_FACTOR   4      /* 8 kHz -> 32 kHz                    */

typedef struct {
    uint16_t ids[AUDIO_PLAYER_MAX_SEQ];
    uint8_t  n;
    uint32_t times;      /* 0 = repetir hasta audio_stop() */
} play_request_t;

static struct {
    audio_get_blob_fn get_blob;
    QueueHandle_t     queue;
    TaskHandle_t      task;
    volatile bool     cancel;      /* audio_stop() lo afirma */
} s_pl;

/* Upsample x4 con interpolacion lineal entera.
 * Entra pcm[0..n) @8k, sale out[0..4n) @32k. prev = ultimo sample previo. */
static void upsample_x4(const int16_t *pcm, size_t n, int16_t prev,
                        int16_t *out)
{
    for (size_t i = 0; i < n; i++) {
        int32_t cur = pcm[i];
        int32_t step = cur - prev;
        for (int k = 0; k < UP_FACTOR; k++)
            *out++ = (int16_t)(prev + (step * k) / UP_FACTOR);
        prev = (int16_t)cur;
    }
}

static bool play_one_clip(uint16_t id)
{
    const uint8_t *blob;
    size_t size;
    if (!s_pl.get_blob || !s_pl.get_blob(id, &blob, &size)) {
        ESP_LOGW(TAG, "id %u no existe en el pack", id);
        return false;
    }
    ESP_LOGI(TAG, "play id=%u (%u B, %lu ms)", id, (unsigned)size,
             (unsigned long)audio_decoder_duration_ms(size));

    audio_decoder_t dec;
    audio_decoder_init(&dec, blob, size);

    int16_t pcm8[DEC_CHUNK];
    int16_t pcm32[DEC_CHUNK * UP_FACTOR];
    int16_t prev = 0;

    size_t n;
    while (!s_pl.cancel &&
           (n = audio_decoder_read(&dec, pcm8, DEC_CHUNK)) > 0) {
        upsample_x4(pcm8, n, prev, pcm32);
        prev = pcm8[n - 1];
        size_t w = audio_driver_write(pcm32, n * UP_FACTOR, portMAX_DELAY);
        if (w < n * UP_FACTOR)
            return false;   /* no deberia pasar con portMAX_DELAY */
    }
    return !s_pl.cancel;
}

static void player_task(void *arg)
{
    play_request_t req;
    for (;;) {
        if (xQueueReceive(s_pl.queue, &req, portMAX_DELAY) != pdTRUE)
            continue;
        s_pl.cancel = false;

        if (audio_driver_start() != ESP_OK) {
            ESP_LOGW(TAG, "driver ocupado, pedido descartado");
            continue;
        }
        /* times = 0 -> loop infinito hasta audio_stop() (uso de banco) */
        for (uint32_t t = 0; (req.times == 0 || t < req.times) && !s_pl.cancel;
             t++)
            for (uint8_t i = 0; i < req.n && !s_pl.cancel; i++)
                play_one_clip(req.ids[i]);

        audio_driver_stop();
        while (!audio_driver_idle())
            vTaskDelay(pdMS_TO_TICKS(20));  /* espera tail + SD LOW
                                               (NO bajar de 10 ms: con tick
                                               de 100 Hz, 5 ms redondea a 0
                                               y el busy-loop dispara el WDT) */
    }
}

esp_err_t audio_player_init(const audio_player_config_t *cfg)
{
    if (!cfg || !cfg->get_blob)
        return ESP_ERR_INVALID_ARG;

    s_pl.get_blob = cfg->get_blob;
    s_pl.queue = xQueueCreate(cfg->queue_len ? cfg->queue_len : 4,
                              sizeof(play_request_t));
    if (!s_pl.queue)
        return ESP_ERR_NO_MEM;

    BaseType_t ok = xTaskCreate(player_task, "audio_player",
                                cfg->task_stack ? cfg->task_stack : 4096,
                                NULL, cfg->task_prio ? cfg->task_prio : 10,
                                &s_pl.task);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t audio_play_seq(const uint16_t *ids, size_t n)
{
    return audio_play_times(ids, n, 1);
}

esp_err_t audio_play_times(const uint16_t *ids, size_t n, uint32_t times)
{
    if (!ids || n == 0 || n > AUDIO_PLAYER_MAX_SEQ)
        return ESP_ERR_INVALID_ARG;
    play_request_t req = { .n = (uint8_t)n, .times = times };
    memcpy(req.ids, ids, n * sizeof(uint16_t));
    return xQueueSend(s_pl.queue, &req, 0) == pdTRUE ? ESP_OK
                                                     : ESP_ERR_INVALID_STATE;
}

esp_err_t audio_play_id(uint16_t id)
{
    return audio_play_seq(&id, 1);
}

esp_err_t audio_say_number(uint32_t number)
{
    uint16_t ids[10];
    size_t n = 0;

    if (number == 0) {
        ids[n++] = AUDIO_ID_DIGIT_BASE;      /* "cero" */
    } else {
        uint32_t div = 1000000000;           /* hasta 10 digitos */
        bool started = false;
        while (div) {
            uint32_t dig = number / div;
            number %= div;
            div /= 10;
            if (dig || started) {
                started = true;
                ids[n++] = (dig == 9) ? AUDIO_ID_DIGIT_NINE
                                      : AUDIO_ID_DIGIT_BASE + dig;
            }
        }
    }
    return audio_play_seq(ids, n);
}

void audio_stop(void)
{
    s_pl.cancel = true;
    xQueueReset(s_pl.queue);
}

bool audio_is_busy(void)
{
    return !audio_driver_idle() ||
           uxQueueMessagesWaiting(s_pl.queue) > 0;
}

void audio_set_volume(uint8_t vol)
{
    audio_driver_set_volume(vol);
}
