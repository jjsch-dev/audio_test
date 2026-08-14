/**
 * audio_test — harness de consola para components/audio (proyecto ES24F).
 *
 * Comandos UART (115200 8N1, por el USB-Serial del devkit):
 *   play <id>          reproduce un clip del pack embebido (ej: play 13)
 *   seq <id> [id..]    prompt compuesto sin gaps  (ej: seq 72 121)
 *   say <n>            habla un numero con los digitos (ej: say 123)
 *   vol <0-100>        volumen digital (100 = 3,3 Vpp = paridad OEM)
 *   stop               corta la reproduccion
 *   under              muestra underruns del driver
 *   clips              lista los ids embebidos
 *
 * Cableado (adapter board):
 *   GPIO4 --[1k]--> pad INP del CS8302, y 8n2 de INP a GND
 *   GPIO5 --[1k]--> pad INN del CS8302, y 8n2 de INN a GND
 *   GPIO6 --------> pin SD del CS8302
 *   GND del S3 ----> GND de la placa de la cerradura
 * Cambiar los GPIO abajo si el adapter usa otros.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_console.h"
#include "esp_log.h"

#include "audio_driver.h"
#include "audio_player.h"

/* ---------- configuracion de pines (editar segun adapter) ---------- */
#define GPIO_PWM_P   4
#define GPIO_PWM_N   5
#define GPIO_SD      6

/* ---------- clips embebidos (pack 07, espanol, via EMBED_FILES) ------ */
#define EMBED_CLIP(name)                                              \
    extern const uint8_t name##_start[] asm("_binary_" #name "_start"); \
    extern const uint8_t name##_end[]   asm("_binary_" #name "_end");

EMBED_CLIP(p07_id002_raw)   /* jingle init          */
EMBED_CLIP(p07_id005_raw)   /* acceso denegado      */
EMBED_CLIP(p07_id013_raw)   /* beep tecla           */
EMBED_CLIP(p07_id016_raw)   /* timbre               */
EMBED_CLIP(p07_id035_raw)   /* sirena               */
EMBED_CLIP(p07_id039_raw)   /* error grave          */
EMBED_CLIP(p07_id040_raw)   /* pinball              */
EMBED_CLIP(p07_id045_raw)   /* bateria baja         */
EMBED_CLIP(p07_id046_raw)   /* agregado con exito   */
EMBED_CLIP(p07_id048_raw)   /* sistema bloqueado    */
EMBED_CLIP(p07_id052_raw)   /* desbloqueado         */
EMBED_CLIP(p07_id058_raw)   /* "numeros"            */
EMBED_CLIP(p07_id069_raw)   /* borrado exitoso      */
EMBED_CLIP(p07_id120_raw)   /* silenciar            */
EMBED_CLIP(p07_id232_raw)   /* "por favor presione" */
EMBED_CLIP(p07_id233_raw)   /* "huella dactilar"    */
EMBED_CLIP(p07_id234_raw)   /* "contrasena"         */
EMBED_CLIP(p07_id248_raw)   /* "tarjeta"            */
EMBED_CLIP(p07_id071_raw)   /* cero                 */
EMBED_CLIP(p07_id072_raw)   /* uno                  */
EMBED_CLIP(p07_id073_raw)   /* dos                  */
EMBED_CLIP(p07_id074_raw)   /* tres                 */
EMBED_CLIP(p07_id075_raw)   /* cuatro               */
EMBED_CLIP(p07_id076_raw)   /* cinco                */
EMBED_CLIP(p07_id077_raw)   /* seis                 */
EMBED_CLIP(p07_id078_raw)   /* siete                */
EMBED_CLIP(p07_id079_raw)   /* ocho                 */
EMBED_CLIP(p07_id090_raw)   /* nueve                */

typedef struct {
    uint16_t       id;
    const uint8_t *start;
    const uint8_t *end;
} clip_entry_t;

#define CLIP_ENTRY(id, name) { id, name##_start, name##_end }

static const clip_entry_t s_clips[] = {
    CLIP_ENTRY(  2, p07_id002_raw), CLIP_ENTRY(  5, p07_id005_raw),
    CLIP_ENTRY( 13, p07_id013_raw), CLIP_ENTRY( 16, p07_id016_raw),
    CLIP_ENTRY( 35, p07_id035_raw), CLIP_ENTRY( 39, p07_id039_raw),
    CLIP_ENTRY( 40, p07_id040_raw), CLIP_ENTRY( 45, p07_id045_raw),
    CLIP_ENTRY( 46, p07_id046_raw), CLIP_ENTRY( 48, p07_id048_raw),
    CLIP_ENTRY( 52, p07_id052_raw), CLIP_ENTRY( 58, p07_id058_raw),
    CLIP_ENTRY( 69, p07_id069_raw), CLIP_ENTRY(120, p07_id120_raw),
    CLIP_ENTRY(232, p07_id232_raw), CLIP_ENTRY(233, p07_id233_raw),
    CLIP_ENTRY(234, p07_id234_raw), CLIP_ENTRY(248, p07_id248_raw),
    CLIP_ENTRY( 71, p07_id071_raw), CLIP_ENTRY( 72, p07_id072_raw),
    CLIP_ENTRY( 73, p07_id073_raw), CLIP_ENTRY( 74, p07_id074_raw),
    CLIP_ENTRY( 75, p07_id075_raw), CLIP_ENTRY( 76, p07_id076_raw),
    CLIP_ENTRY( 77, p07_id077_raw), CLIP_ENTRY( 78, p07_id078_raw),
    CLIP_ENTRY( 79, p07_id079_raw), CLIP_ENTRY( 90, p07_id090_raw),
};

/* Proveedor de clips para el player (v0: arrays embebidos) */
static bool embedded_get_blob(uint16_t id, const uint8_t **ptr, size_t *size)
{
    for (size_t i = 0; i < sizeof(s_clips) / sizeof(s_clips[0]); i++) {
        if (s_clips[i].id == id) {
            *ptr  = s_clips[i].start;
            *size = s_clips[i].end - s_clips[i].start;
            return true;
        }
    }
    return false;
}

/* ---------- comandos de consola ---------- */

static int cmd_play(int argc, char **argv)
{
    if (argc != 2) { printf("uso: play <id>\n"); return 1; }
    esp_err_t e = audio_play_id((uint16_t)atoi(argv[1]));
    printf(e == ESP_OK ? "ok\n" : "error: %s\n", esp_err_to_name(e));
    return 0;
}

static int cmd_seq(int argc, char **argv)
{
    if (argc < 2) { printf("uso: seq <id> [id ...]\n"); return 1; }
    uint16_t ids[AUDIO_PLAYER_MAX_SEQ];
    size_t n = (size_t)(argc - 1);
    if (n > AUDIO_PLAYER_MAX_SEQ) n = AUDIO_PLAYER_MAX_SEQ;
    for (size_t i = 0; i < n; i++) ids[i] = (uint16_t)atoi(argv[i + 1]);
    esp_err_t e = audio_play_seq(ids, n);
    printf(e == ESP_OK ? "ok\n" : "error: %s\n", esp_err_to_name(e));
    return 0;
}

static int cmd_say(int argc, char **argv)
{
    if (argc != 2) { printf("uso: say <numero>\n"); return 1; }
    esp_err_t e = audio_say_number((uint32_t)strtoul(argv[1], NULL, 10));
    printf(e == ESP_OK ? "ok\n" : "error: %s\n", esp_err_to_name(e));
    return 0;
}

/* rep <id> <n>: repite el clip n veces (n=0 -> infinito hasta 'stop').
 * Pensado para el banco: el scope captura el patron sin apurarse. */
static int cmd_rep(int argc, char **argv)
{
    if (argc != 3) { printf("uso: rep <id> <veces>\n"); return 1; }
    uint16_t id = (uint16_t)atoi(argv[1]);
    uint32_t times = (uint32_t)strtoul(argv[2], NULL, 10);
    esp_err_t e = audio_play_times(&id, 1, times);
    printf(e == ESP_OK ? "ok\n" : "error: %s\n", esp_err_to_name(e));
    return 0;
}

static int cmd_vol(int argc, char **argv)
{
    if (argc != 2) { printf("uso: vol <0-100>\n"); return 1; }
    audio_set_volume((uint8_t)atoi(argv[1]));
    printf("vol=%u\n", audio_driver_get_volume());
    return 0;
}

static int cmd_stop(int argc, char **argv)
{
    (void)argc; (void)argv;
    audio_stop();
    printf("ok\n");
    return 0;
}

static int cmd_under(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("underruns: %lu\n", (unsigned long)audio_driver_underruns());
    return 0;
}

static int cmd_clips(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("ids embebidos:");
    for (size_t i = 0; i < sizeof(s_clips) / sizeof(s_clips[0]); i++)
        printf(" %u", s_clips[i].id);
    printf("\n");
    return 0;
}

/* tone <0-100|off>: duty fijo forzado por la ISR (bypasea player/ring).
 * Sirve para aislar: si 'tone 75' muestra 75% en GPIO4 y su espejo en
 * GPIO5, el camino compare->hardware esta sano y el problema esta en la
 * cadena de datos (decoder/player/ring). */
static int cmd_tone(int argc, char **argv)
{
    if (argc != 2) { printf("uso: tone <0-100|off>\n"); return 1; }
    if (!strcmp(argv[1], "off")) {
        audio_driver_test_duty(-1);
        printf("tone off\n");
    } else {
        int pct = atoi(argv[1]);
        audio_driver_test_duty(pct);
        printf("tone %d%% (duty forzado; 'tone off' para salir)\n", pct);
    }
    return 0;
}

static int cmd_diag(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("estado: %s | pwm: %s | ring: %u bytes | rampa: %d/256 | vol: %u | "
           "underruns: %lu\n",
           audio_driver_state_str(),
           audio_driver_pwm_running() ? "on" : "off(GND)",
           (unsigned)audio_driver_bytes_in_ring(),
           audio_driver_ramp(),
           audio_driver_get_volume(),
           (unsigned long)audio_driver_underruns());
    return 0;
}

void app_main(void)
{
    /* driver */
    audio_driver_config_t dcfg = {
        .gpio_pwm_p = GPIO_PWM_P,
        .gpio_pwm_n = GPIO_PWM_N,
        .gpio_sd    = GPIO_SD,
    };
    ESP_ERROR_CHECK(audio_driver_init(&dcfg));

    /* player */
    audio_player_config_t pcfg = { .get_blob = embedded_get_blob };
    ESP_ERROR_CHECK(audio_player_init(&pcfg));

    /* consola REPL por UART */
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "audio> ";
    esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl));

    const esp_console_cmd_t cmds[] = {
        { .command = "play",  .help = "play <id>",          .func = cmd_play  },
        { .command = "seq",   .help = "seq <id> [id...]",   .func = cmd_seq   },
        { .command = "say",   .help = "say <numero>",       .func = cmd_say   },
        { .command = "vol",   .help = "vol <0-100>",        .func = cmd_vol   },
        { .command = "stop",  .help = "corta reproduccion", .func = cmd_stop  },
        { .command = "under", .help = "underruns driver",   .func = cmd_under },
        { .command = "clips", .help = "lista ids embebidos",.func = cmd_clips },
        { .command = "tone",  .help = "tone <0-100|off> duty fijo (diag)", .func = cmd_tone },
        { .command = "diag",  .help = "estado driver/ring/rampa",          .func = cmd_diag },
        { .command = "rep",   .help = "rep <id> <n> repite (0=infinito)",  .func = cmd_rep  },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    esp_console_register_help_command();
    ESP_ERROR_CHECK(esp_console_start_repl(repl));

    printf("\n=== audio_test ES24F ===\n");
    printf("Cableado: GPIO%d->RC->INP | GPIO%d->RC->INN | GPIO%d->SD | GND comun\n",
           GPIO_PWM_P, GPIO_PWM_N, GPIO_SD);
    printf("RC por rama: 1k serie + 8n2 a GND (fc~20 kHz)\n");
    printf("Prueba rapida: 'play 13' (beep 1,01 kHz), 'play 2' (jingle), "
           "'say 123', 'seq 232 233'\n");
}
