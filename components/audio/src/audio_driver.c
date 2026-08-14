/**
 * audio_driver.c — MCPWM complementario + ISR 32 kHz + secuencia SD.
 * Ver audio_driver.h. Componente ESP-IDF v5.x (driver nuevo MCPWM).
 *
 * Notas de implementacion:
 * - La contrafase se genera con acciones directas espejadas sobre el mismo
 *   comparador (gen_n: LOW@TEZ / HIGH@CMP). NO se usa el modulo de
 *   dead-time: con delays en 0 el hardware no lo aplica y invert_output
 *   no es selectivo por generador (esp-idf issue #11327).
 * - Reposo (IDLE): timer MCPWM detenido y ambos GPIO forzados a GND via
 *   force level. El 50% solo existe durante STARTING/RUNNING/STOPPING.
 *   Motivos: EMI (portadora de 156 kHz conmutando en permanencia junto al
 *   front-end MIFARE de 13,56 MHz), consumo dinamico y compatibilidad con
 *   sleep a futuro. El transitorio 0%<->50% es comun-modo (el diferencial
 *   queda en ~0 V) y ocurre con el CS8302 en shutdown (lead/tail), asi
 *   que no produce pop audible.
 * - Solo la ISR actualiza el compare (post-init) -> sin contencion.
 * - Rampa/volumen en punto fijo dentro de la ISR (Q8).
 */

#include "audio_driver.h"

#include <string.h>
#include "driver/mcpwm_prelude.h"
#include "driver/gptimer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "audio_driver";

typedef enum {
    DRV_IDLE = 0,     /* SD LOW, timer apagado                        */
    DRV_STARTING,     /* SD HIGH, esperando lead (salida = silencio)  */
    DRV_RUNNING,      /* reproduciendo                                */
    DRV_STOPPING,     /* stop pedido: drenar -> rampa abajo -> tail   */
} drv_state_t;

static struct {
    /* config */
    int      gpio_sd;
    uint32_t period_ticks;
    uint32_t lead_ticks;        /* lead_ms * 32                          */
    uint32_t tail_ticks;        /* tail_ms * 32                          */
    /* hardware */
    mcpwm_timer_handle_t     timer;
    mcpwm_oper_handle_t      oper;
    mcpwm_cmpr_handle_t      cmpr;
    mcpwm_gen_handle_t       gen_p;
    mcpwm_gen_handle_t       gen_n;
    gptimer_handle_t         gpt;
    /* datos */
    StreamBufferHandle_t     ring;
    size_t                   ring_bytes;
    /* estado volatile (compartido con ISR) */
    volatile drv_state_t     state;
    volatile uint32_t        lead_left;     /* countdown del lead (SD on) */
    volatile uint32_t        tail_left;     /* countdown del tail         */
    volatile int32_t         ramp;          /* 0..256 (Q8)               */
    volatile uint8_t         volume;        /* 0..100                    */
    volatile uint32_t        underruns;
    volatile int32_t         test_duty_pct; /* -1 = off (diagnostico)    */
    volatile bool            pwm_running;   /* timer MCPWM corriendo     */
} s_drv;

/* ---------------- PWM on/off (llamar SOLO desde la ISR) ---------------- */

/* Idempotentes: el flag pwm_running evita escrituras repetidas. */
static void IRAM_ATTR pwm_hw_start(void)
{
    if (s_drv.pwm_running) return;
    /* arranca en 50% (silencio): el compare se presetea ANTES de soltar
     * el force, asi el primer ciclo ya sale con duty medio */
    mcpwm_comparator_set_compare_value(s_drv.cmpr, s_drv.period_ticks / 2);
    mcpwm_timer_start_stop(s_drv.timer, MCPWM_TIMER_START_NO_STOP);
    mcpwm_generator_set_force_level(s_drv.gen_p, -1, true);  /* -1 = soltar */
    mcpwm_generator_set_force_level(s_drv.gen_n, -1, true);
    s_drv.pwm_running = true;
}

static void IRAM_ATTR pwm_hw_stop(void)
{
    if (!s_drv.pwm_running) return;
    mcpwm_timer_start_stop(s_drv.timer, MCPWM_TIMER_STOP_EMPTY);
    mcpwm_generator_set_force_level(s_drv.gen_p, 0, true);   /* GND */
    mcpwm_generator_set_force_level(s_drv.gen_n, 0, true);   /* GND */
    s_drv.pwm_running = false;
}

/* ---------------- ISR 32 kHz ---------------- */

static bool IRAM_ATTR drv_on_alarm(gptimer_handle_t timer,
                                   const gptimer_alarm_event_data_t *edata,
                                   void *user_ctx)
{
    int16_t sample = 0;

    switch (s_drv.state) {
    case DRV_IDLE:
        if (s_drv.test_duty_pct >= 0) {
            pwm_hw_start();               /* tone de banco: despierta PWM */
        } else {
            pwm_hw_stop();                /* reposo: GPIOs a GND          */
        }
        break;

    case DRV_STARTING:
        pwm_hw_start();                   /* 50% (silencio) desde el lead */
        if (s_drv.lead_left > 0) {
            s_drv.lead_left--;
        } else {
            s_drv.state = DRV_RUNNING;
        }
        break;                            /* lead: silencio (duty 50%) */

    case DRV_RUNNING:
    case DRV_STOPPING: {
        /* stop durante el lead: se respeta lo que falte del wake-up del
         * CS8302 antes de drenar (paridad OEM: el audio arranca 30 ms
         * despues de SD aunque el player ya haya escrito todo el clip) */
        if (s_drv.state == DRV_STOPPING && s_drv.lead_left > 0) {
            s_drv.lead_left--;
            break;                        /* sample=0 -> duty 50% */
        }
        size_t got = xStreamBufferReceiveFromISR(s_drv.ring, &sample,
                                                 sizeof(sample), NULL);
        if (got == sizeof(sample)) {
            /* sample real: la rampa sube SIEMPRE que haya datos, tambien
             * en STOPPING. Bug historico: un clip mas corto que el ring
             * (beep 126 ms < 128 ms) entraba entero durante el lead, el
             * player llamaba stop() y el clip drenaba con rampa=0 ->
             * silencio total con timing de SD perfecto. 'rep' lo
             * enmascaraba porque nunca llama stop() (se bloquea en el
             * ring lleno y el estado siempre alcanza RUNNING). */
            if (s_drv.ramp < 256) {
                s_drv.ramp += 2;          /* ~4 ms @ 32 kHz */
                if (s_drv.ramp > 256) s_drv.ramp = 256;
            }
        } else {
            sample = 0;
            if (s_drv.state == DRV_RUNNING) {
                s_drv.underruns++;
            } else {
                /* STOPPING y ring vacio: rampa abajo -> tail -> SD LOW */
                if (s_drv.ramp > 0) {
                    s_drv.ramp -= 2;              /* ~4 ms @ 32 kHz */
                    if (s_drv.ramp < 0) s_drv.ramp = 0;
                } else if (s_drv.tail_left > 0) {
                    s_drv.tail_left--;            /* tail */
                } else {
                    gpio_set_level(s_drv.gpio_sd, 0);
                    s_drv.state = DRV_IDLE;
                }
            }
        }
        break;
    }
    }

    /* escalado: volumen (0..100) y rampa (Q8) */
    int32_t v = sample;
    v = v * s_drv.volume / 100;
    v = (v * s_drv.ramp) >> 8;

    int32_t half  = (int32_t)s_drv.period_ticks / 2;
    int32_t duty  = half + (v * (half - 2)) / 32768;

    /* Modo diagnostico: duty fijo forzado AL FINAL (la maquina de estados
     * sigue corriendo: el ring drena y el canal cierra normalmente) */
    if (s_drv.test_duty_pct >= 0)
        duty = (int32_t)((s_drv.period_ticks * (uint32_t)s_drv.test_duty_pct) / 100);

    mcpwm_comparator_set_compare_value(s_drv.cmpr, (uint32_t)duty);
    return false;
}

/* ---------------- API ---------------- */

esp_err_t audio_driver_init(const audio_driver_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "cfg NULL");

    memset(&s_drv, 0, sizeof(s_drv));   /* nota: driver de uso unico */
    s_drv.state  = DRV_IDLE;
    s_drv.volume = 100;
    s_drv.test_duty_pct = -1;
    s_drv.gpio_sd = cfg->gpio_sd;

    uint32_t res    = cfg->pwm_resolution_hz ? cfg->pwm_resolution_hz
                                             : AUDIO_DRIVER_PWM_RESOLUTION_HZ;
    uint32_t period = cfg->pwm_period_ticks ? cfg->pwm_period_ticks
                                            : AUDIO_DRIVER_PWM_PERIOD_TICKS;
    size_t ring_n   = cfg->ring_samples ? cfg->ring_samples
                                        : AUDIO_DRIVER_RING_SAMPLES;
    uint32_t lead_ms = cfg->sd_lead_ms ? cfg->sd_lead_ms : AUDIO_DRIVER_SD_LEAD_MS;
    uint32_t tail_ms = cfg->sd_tail_ms ? cfg->sd_tail_ms : AUDIO_DRIVER_SD_TAIL_MS;

    s_drv.lead_ticks = lead_ms * (AUDIO_DRIVER_SAMPLE_RATE / 1000);
    s_drv.tail_ticks = tail_ms * (AUDIO_DRIVER_SAMPLE_RATE / 1000);

    /* fallback si el clock de 160 MHz no esta disponible (p.ej. solo XTAL) */
    mcpwm_timer_config_t tcfg = {
        .group_id      = 0,
        .clk_src       = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = res,
        .count_mode    = MCPWM_TIMER_COUNT_MODE_UP,
        .period_ticks  = period,
    };
    esp_err_t err = mcpwm_new_timer(&tcfg, &s_drv.timer);
    if (err != ESP_OK && res == AUDIO_DRIVER_PWM_RESOLUTION_HZ) {
        ESP_LOGW(TAG, "160 MHz no disponible, probando 40 MHz / 256 ticks");
        res = 40000000; period = 256;
        tcfg.resolution_hz = res; tcfg.period_ticks = period;
        err = mcpwm_new_timer(&tcfg, &s_drv.timer);
    }
    ESP_RETURN_ON_ERROR(err, TAG, "mcpwm_new_timer");
    s_drv.period_ticks = period;
    ESP_LOGI(TAG, "MCPWM carrier: %lu Hz, %lu ticks (%d bits efectivos)",
             (unsigned long)(res / period), (unsigned long)period,
             (int)(31 - __builtin_clz(period)));

    mcpwm_operator_config_t ocfg = { .group_id = 0 };
    ESP_RETURN_ON_ERROR(mcpwm_new_operator(&ocfg, &s_drv.oper), TAG, "oper");
    ESP_RETURN_ON_ERROR(mcpwm_operator_connect_timer(s_drv.oper, s_drv.timer),
                        TAG, "connect");

    mcpwm_comparator_config_t ccfg = { .flags.update_cmp_on_tez = true };
    ESP_RETURN_ON_ERROR(mcpwm_new_comparator(s_drv.oper, &ccfg, &s_drv.cmpr),
                        TAG, "cmpr");

    mcpwm_generator_config_t gcfg_p = { .gen_gpio_num = cfg->gpio_pwm_p };
    mcpwm_generator_config_t gcfg_n = { .gen_gpio_num = cfg->gpio_pwm_n };
    ESP_RETURN_ON_ERROR(mcpwm_new_generator(s_drv.oper, &gcfg_p, &s_drv.gen_p),
                        TAG, "gen_p");
    ESP_RETURN_ON_ERROR(mcpwm_new_generator(s_drv.oper, &gcfg_n, &s_drv.gen_n),
                        TAG, "gen_n");

    /* fase P: HIGH al vaciar, LOW al compare
     * (IDF 6.x: API de accion unica; las variadicas *_END fueron removidas) */
    mcpwm_gen_timer_event_action_t act_tez = {
        .direction = MCPWM_TIMER_DIRECTION_UP,
        .event     = MCPWM_TIMER_EVENT_EMPTY,
        .action    = MCPWM_GEN_ACTION_HIGH,
    };
    ESP_RETURN_ON_ERROR(
        mcpwm_generator_set_action_on_timer_event(s_drv.gen_p, act_tez),
        TAG, "act_p_tez");

    mcpwm_gen_compare_event_action_t act_cmp = {
        .direction  = MCPWM_TIMER_DIRECTION_UP,
        .comparator = s_drv.cmpr,
        .action     = MCPWM_GEN_ACTION_LOW,
    };
    ESP_RETURN_ON_ERROR(
        mcpwm_generator_set_action_on_compare_event(s_drv.gen_p, act_cmp),
        TAG, "act_p_cmp");

    /* fase N: complementaria con ACCIONES DIRECTAS sobre el mismo comparador
     * (LOW@TEZ / HIGH@CMP = espejo de la fase P). NO se usa el modulo de
     * dead-time: con delays en 0 el hardware no lo aplica y el flag
     * invert_output no es selectivo por generador (esp-idf issue #11327).
     * Como cada GPIO alimenta su propio RC (no hay medio puente), no hay
     * riesgo de shoot-through y el dead-time no es necesario. */
    mcpwm_gen_timer_event_action_t act_n_tez = {
        .direction = MCPWM_TIMER_DIRECTION_UP,
        .event     = MCPWM_TIMER_EVENT_EMPTY,
        .action    = MCPWM_GEN_ACTION_LOW,
    };
    ESP_RETURN_ON_ERROR(
        mcpwm_generator_set_action_on_timer_event(s_drv.gen_n, act_n_tez),
        TAG, "act_n_tez");

    mcpwm_gen_compare_event_action_t act_n_cmp = {
        .direction  = MCPWM_TIMER_DIRECTION_UP,
        .comparator = s_drv.cmpr,
        .action     = MCPWM_GEN_ACTION_HIGH,
    };
    ESP_RETURN_ON_ERROR(
        mcpwm_generator_set_action_on_compare_event(s_drv.gen_n, act_n_cmp),
        TAG, "act_n_cmp");

    /* duty inicial 50% (silencio) antes de habilitar */
    ESP_RETURN_ON_ERROR(mcpwm_comparator_set_compare_value(s_drv.cmpr,
                                                           period / 2),
                        TAG, "cmp init");
    ESP_RETURN_ON_ERROR(mcpwm_timer_enable(s_drv.timer), TAG, "timer enable");
    /* Reposo de fabrica: timer DETENIDO y ambas fases forzadas a GND.
     * La ISR arranca el PWM recien al entrar en STARTING (o con 'tone'). */
    ESP_RETURN_ON_ERROR(mcpwm_generator_set_force_level(s_drv.gen_p, 0, true),
                        TAG, "force p");
    ESP_RETURN_ON_ERROR(mcpwm_generator_set_force_level(s_drv.gen_n, 0, true),
                        TAG, "force n");
    s_drv.pwm_running = false;

    /* ring buffer (ISR-safe) */
    s_drv.ring_bytes = ring_n * sizeof(int16_t);
    s_drv.ring = xStreamBufferCreate(s_drv.ring_bytes, sizeof(int16_t));
    ESP_RETURN_ON_FALSE(s_drv.ring != NULL, ESP_ERR_NO_MEM, TAG, "ring");

    /* GPIO SD (CS8302: HIGH = habilitado, pull-down interno) */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << cfg->gpio_sd,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio sd");
    gpio_set_level(cfg->gpio_sd, 0);

    /* timer de 32 kHz exactos: 8 MHz / 250 */
    gptimer_config_t g = {
        .clk_src       = GPTIMER_CLK_SRC_DEFAULT,
        .direction     = GPTIMER_COUNT_UP,
        .resolution_hz = 8000000,
    };
    ESP_RETURN_ON_ERROR(gptimer_new_timer(&g, &s_drv.gpt), TAG, "gpt new");
    gptimer_alarm_config_t al = {
        .alarm_count = 8000000 / AUDIO_DRIVER_SAMPLE_RATE,   /* 250 */
        .reload_count = 0,
        .flags.auto_reload_on_alarm = true,
    };
    ESP_RETURN_ON_ERROR(gptimer_set_alarm_action(s_drv.gpt, &al), TAG, "gpt al");
    gptimer_event_callbacks_t cb = { .on_alarm = drv_on_alarm };
    ESP_RETURN_ON_ERROR(gptimer_register_event_callbacks(s_drv.gpt, &cb, NULL),
                        TAG, "gpt cb");
    ESP_RETURN_ON_ERROR(gptimer_enable(s_drv.gpt), TAG, "gpt enable");
    ESP_RETURN_ON_ERROR(gptimer_start(s_drv.gpt), TAG, "gpt start");

    ESP_LOGI(TAG, "init OK (P=%d N=%d SD=%d, lead=%lums tail=%lums)",
             cfg->gpio_pwm_p, cfg->gpio_pwm_n, cfg->gpio_sd,
             (unsigned long)lead_ms, (unsigned long)tail_ms);
    return ESP_OK;
}

esp_err_t audio_driver_start(void)
{
    if (s_drv.state != DRV_IDLE)
        return ESP_ERR_INVALID_STATE;
    xStreamBufferReset(s_drv.ring);
    s_drv.ramp = 0;
    s_drv.lead_left = s_drv.lead_ticks;
    s_drv.tail_left = s_drv.tail_ticks;
    gpio_set_level(s_drv.gpio_sd, 1);
    s_drv.state = DRV_STARTING;
    return ESP_OK;
}

esp_err_t audio_driver_stop(void)
{
    if (s_drv.state == DRV_IDLE)
        return ESP_OK;
    s_drv.tail_left = s_drv.tail_ticks;
    s_drv.state = DRV_STOPPING;   /* la ISR: fin de lead -> drena ->
                                     rampa abajo -> tail -> SD LOW */
    return ESP_OK;
}

bool audio_driver_idle(void)
{
    return s_drv.state == DRV_IDLE;
}

size_t audio_driver_write(const int16_t *samples, size_t count,
                          uint32_t timeout_ms)
{
    size_t sent = xStreamBufferSend(s_drv.ring, samples,
                                    count * sizeof(int16_t),
                                    pdMS_TO_TICKS(timeout_ms));
    return sent / sizeof(int16_t);
}

size_t audio_driver_write_space(void)
{
    return xStreamBufferSpacesAvailable(s_drv.ring) / sizeof(int16_t);
}

void audio_driver_set_volume(uint8_t vol)
{
    if (vol > 100) vol = 100;
    s_drv.volume = vol;
}

uint8_t audio_driver_get_volume(void)
{
    return s_drv.volume;
}

uint32_t audio_driver_underruns(void)
{
    return s_drv.underruns;
}

/* ---------- diagnostico (harness) ---------- */

void audio_driver_test_duty(int pct)
{
    s_drv.test_duty_pct = (pct < 0 || pct > 100) ? -1 : pct;
}

const char *audio_driver_state_str(void)
{
    switch (s_drv.state) {
    case DRV_IDLE:     return "IDLE";
    case DRV_STARTING: return "STARTING(lead)";
    case DRV_RUNNING:  return "RUNNING";
    case DRV_STOPPING: return "STOPPING(drain/tail)";
    default:           return "?";
    }
}

size_t audio_driver_bytes_in_ring(void)
{
    return xStreamBufferBytesAvailable(s_drv.ring);
}

int audio_driver_ramp(void)
{
    return (int)s_drv.ramp;
}

bool audio_driver_pwm_running(void)
{
    return s_drv.pwm_running;
}
