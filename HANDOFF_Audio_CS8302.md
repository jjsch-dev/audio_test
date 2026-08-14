# Handoff Técnico — Stage 4: Librería de Audio (CS8302) + Dump de Flash ZB25VQ64C

> Documento de transferencia para continuar el desarrollo en un nuevo chat.
> Proyecto: ES242F smart-lock, retrofit ESP32-S3 (ver `PROJECT_NOTES.md`).
> Stage 4 (audio) en arranque. Fecha: 2026-08-11/12.
> Fuente: datasheet CS8302M (ChipSourceTek) + plan de evidencia acordado (D4).

---

## 1. Objetivo de la etapa

Construir `components/audio/` (API pública en `include/`, implementación en
`src/`, harness de consola en `main/`) que reproduzca los sonidos de la
cerradura con **paridad OEM** (D2): beeps de tecla + prompts de voz
(boot, enroll, verify OK/fail, tarjeta, etc.).

Dos frentes de trabajo, en este orden:

1. **Dump de la flash ZB25VQ64C** — contiene firmware Tuya + (casi seguro)
   los prompts de voz. Es también la red de seguridad del proyecto
   (referencia de fallback del firmware, `PROJECT_NOTES.md` §6.2).
2. **Caracterización del CS8302 y del reproductor del OEM** — cómo el
   FR8018H saca audio (PWM filtrado vs. otra cosa), qué formato tienen los
   clips, y qué clip se usa en cada evento.

---

## 2. Hechos de hardware (datasheet CS8302M, ya revisado)

| Parámetro | Valor | Nota |
|---|---|---|
| Tipo | Clase D mono, 3 W, **filterless** (sin LC de salida), ultra-bajo EMI (AERC) | |
| Entrada | **Analógica**, diferencial (+IN/-IN) o single-ended | El chip NO define formato digital — el formato lo decide el FR8018H |
| Ganancia | `Av = 2 × (150 kΩ / Rin)` | Rin externa en placa + Cin de acople (high-pass de entrada) |
| Pin SD (1) | Shutdown con **pull-down interno**: HIGH = habilitado, LOW = < 0.1 µA | Análogo a NRSTPD: clave para consumo en batería |
| Alimentación | 2.5–6 V | 1.66 W @ 8 Ω/5 V (10% THD), 2.9 W @ 4 Ω |
| Encendido | ~40 ms startup, supresión pop & click interna | Relevante para timing de SD antes del audio |

**Implicancia:** el FR8018H no tiene DAC → lo esperable es PWM filtrado por
RC hacia +IN (o PDM/bitstream). A confirmar con scope (fase 5).

---

## 3. Dump in-circuit de la ZB25VQ64C (fases 1–2)

### 3.1 Estrategia (acordada, NO desoldar)

Reutiliza la técnica ya validada de poner el CPU en alta impedancia:

```
Reset del FR8018H afirmado (pines SPI en Hi-Z)
   + NRSTPD atado a GND (NZ3801 en hard power-down, SPI en Hi-Z)
   = bus SPI de la flash 100% libre para el CH341A
```

Este escenario ("reader apagado + flash activa") es el modo de uso normal
del diseño OEM (Stage 2, `HANDOFF_Mifare_NZ3801.md` §4). Con CPU en reset,
**NRSTPD queda flotante** — atarlo a GND con jumper o R de 10 k durante todo
el dump, si no el reader despierta por ruido y corrompe la lectura.

### 3.2 Checklist eléctrico

- [x] **CH341A modeado a 3V3** (bug de 5 V en señales SPI de la placa negra
  ya corregido por el usuario — verificado).
- [ ] **Secuencia:** afirmar reset → energizar placa → leer → desenergizar
  → soltar reset. El FR8018H bootea desde esta flash: energizar sin reset
  afirmado = contención de bus.
- [ ] **Alimentación:** la placa con su propia fuente/baterías. Del CH341A
  solo GND + CS + CLK + MOSI + MISO. **NO conectar el pin VCC del
  programador** (backfeed arranca el CPU; regulador del CH341A insuficiente).
- [ ] Verificar WP#/HOLD# de la flash en alto (normalmente atados a VCC en placa).

### 3.3 Comandos (Ubuntu, flashrom ya instalado)

El ZB25VQ64C **no está en la base de chips de flashrom** (upstream solo tiene
ZB25VQ16). Además los ZB25VQ reportan un manufacturer ID distinto al del
datasheet (0x0E vs 0x5E documentado) → el auto-detect dirá probablemente
"unknown SPI chip (RDID)". No es problema para leer: el comando Read 0x03 es
estándar en toda la familia 25Q; se fuerza un compatible del mismo tamaño.

```bash
# 1. Ver qué RDID reporta (esperado: algo como 0E 40 17 → "unknown")
#    Anotar el ID: es la evidencia de que hablamos con la flash
#    (bus flotante devuelve 0xFFFFFF o 0x000000).
flashrom -p ch341a_spi

# 2. Leer forzando compatible W25Q64 de 3V (8 MB, mismo comando Read 0x03).
#    OJO: en flashrom v1.6 la entrada "W25Q64.V" se renombró a sus
#    variantes explícitas (el -L la muestra partida en 3 líneas):
flashrom -p ch341a_spi -c "W25Q64BV/W25Q64CV/W25Q64FV" -r zb25vq64_dump_1.bin
flashrom -p ch341a_spi -c "W25Q64BV/W25Q64CV/W25Q64FV" -r zb25vq64_dump_2.bin
flashrom -p ch341a_spi -c "W25Q64BV/W25Q64CV/W25Q64FV" -r zb25vq64_dump_3.bin
sha256sum zb25vq64_dump_*.bin
```

Notas:
- Sufijos en el listado de flashrom: `.V` = 2.7–3.6 V (la nuestra),
  `.W` = 1.65–1.95 V (descartada, es de 1.8 V). Para lectura el comando
  0x03 es idéntico en toda la familia 25Q — lo único crítico es el tamaño
  (8192 kB). Al forzar con `-c` no hay comparación de ID.

Notas:
- flashrom con `ch341a_spi` corre fijo a **2 MHz** — adecuado in-circuit.
- Error libusb `errno=13` → regla udev para VID:PID `1a86:5512`.
- Si en el futuro hace falta **escribir** la flash: agregar la entrada del
  ZB25VQ64 a `flashchips/zbit.c` (copiar ZB25VQ16, cambiar size/ID) y
  recompilar. Para lectura no hace falta.

### 3.4 Criterio de aceptación del dump

**3 lecturas con SHA256 idénticos.** Un dump corrupto tiene el mismo tamaño
que uno bueno — la única evidencia de integridad es la repetibilidad.
Si los hashes difieren: problema eléctrico (clip, reset, NRSTPD, alimentación).
No se "promedia": se revisa el setup y se repite. Si las fallas son
sistemáticas → plan B: clip SOIC-8 distinto o desoldar.

---

## 4. Búsqueda del audio: método acordado

### 4.1 Por qué no alcanza con binwalk (expectativa realista)

Los prompts de voz en SoC BLE chinos (Tuya/FR8018H) típicamente **no tienen
header de archivo**: son PCM crudo o ADPCM referenciados por una tabla de
punteros en el firmware. binwalk encuentra RIFF/ID3 si existen; si no, el
carving a ciegas en 8 MB es "probar formatos al azar en regiones al azar".

### 4.2 Fase 3 — exploración estática del dump (barata, hacer igual)

- `binwalk zb25vq64_dump_1.bin` (firmas: RIFF/WAV, ID3/MP3)
- `binwalk -E` (mapa de entropía: separa código / comprimido / PCM / vacío 0xFF)
- `strings` (pistas de tablas de prompts, nombres de recursos Tuya)
- Si aparecen WAVs con header → extracción directa, saltar a fase 6.

### 4.3 Fase 4 — sniffer de flash con clustering de ráfagas (la clave)

**Concepto:** no necesitamos sincronización fina SPI↔audio; necesitamos
**asociación de eventos**. Cada prompt es una ráfaga contigua de lecturas
`Read(0x03, dir, len)` separada por segundos de silencio de bus. El desfase
por buffering en RAM son ms — no hay ambigüedad de a qué evento pertenece
cada ráfaga.

**Setup:**
- Sniffer SPI PIO (el de siempre), capturando el CS de la **flash**
  (discriminar del CS del NZ3801 con el gating ya usado en Stage 2).
- Referencia temporal de los eventos: pin **SD del CS8302** en un canal del
  scope/lógico — cuando el OEM habilita el ampli, viene un sonido. Capturar
  ventana de unos segundos alrededor de cada flanco de SD.
- Eventos a capturar (mismo set en fase 4 y 5): **boot, beep de cada tecla,
  enroll, verify OK, verify fail, tarjeta OK, tarjeta rechazada**.

**Procesamiento (Python sobre el log):**
1. Parsear transacciones de flash: comando, dirección, longitud, timestamp.
2. Clustering: gap > ~100 ms = nueva ráfaga.
3. Salida: ráfaga N → rango de direcciones, total de bytes, duración.
4. Mapeo ráfaga ↔ evento (boot = primera ráfaga tras energizar, etc.).

**El "desfase" es información, no problema:**
- Lecturas **todas antes** del flanco de SD → el CPU bufferea el clip en RAM
  (formato compacto o decodificación previa).
- Lecturas **ritmadas durante** la reproducción → streaming desde flash;
  la tasa de lectura da el sample rate por división directa.

**Ojo con el beep de tecla:** puede no estar en la flash (tono sintetizado
por PWM). El sniffer lo resuelve: si el beep no tiene ráfaga asociada, es
generado.

### 4.4 Fase 5 — scope sobre el CS8302 (misma sesión, mismos eventos)

- **SD**: cuándo habilita/deshabilita el ampli respecto del sonido (timing
  para nuestra lib; el chip tarda ~40 ms en arrancar — ¿el OEM anticipa SD?).
- **+IN/-IN**: naturaleza de la señal (¿PWM filtrado por RC? ¿frecuencia?
  ¿single-ended o diferencial?). Esto es la **spec de entrada para validar
  el reproductor del ESP32-S3**.
- Medir en placa **Rin y Cin** del lazo de entrada → ganancia real
  (`Av = 2 × 150k/Rin`) y corte del high-pass. Paridad de volumen.

### 4.5 Fase 6 — extracción y formato

Con las regiones acotadas por la fase 4:
- **Audacity** → Importar → Datos en crudo. Candidatas en orden:
  8-bit unsigned PCM @ 8 kHz, 16-bit signed PCM @ 8/16 kHz, IMA-ADPCM 4-bit.
- **sox** para conversión en lote y audición rápida de todas las variantes.
- Si nada suena: sospechar PCM 1-bit (PDM para PWM directo) — la fase 5 da
  la tasa real.
- Plan C (solo si hace falta): Ghidra sobre el dump (FR8018H = Cortex-M3)
  para ubicar la tabla de prompts y la lógica evento→clip.

---

## 4bis. RESULTADOS — Dump y exploración estática (2026-08-12) ✅

### Dump verificado

- **3 lecturas idénticas**, SHA256 = `2c45cb0d…450c1295`, 8.388.608 bytes.
- 66,4% de la memoria con datos (5,57 MB usados).
- Firma `TUYASMARTLOCK` en offset 0x0.
- **El ZB25VQ64C es SFDP-capable**: flashrom lo autodetecta
  ("SFDP-capable chip", 8192 kB) y lee/escribe **sin forzar `-c`**.
  El `-c "W25Q64BV/..."` falla por diseño (probea el JEDEC ID Winbond
  0xEF4017, que no coincide con el del Zbit). **Usar modo automático.**

### Estructura del área de audio (descubierta)

- **1004 clips WAV completos** (con header RIFF), todos:
  **IMA-ADPCM, 8 kHz, mono, 4 bits**, codificados de fábrica con FFmpeg
  (tag `Lavf58.29.100` en chunk INFO). Sin cifrado ni compresión extra.
- Organización: **10 packs de idioma autocontenidos**. Cada pack:
  `[header 6B][tabla N×9B][pad 0xFF][N clips contiguos]`.
  Bases: `0x2000, 0x71000, 0xF8000, 0x15E000, 0x20A000, 0x295000,
  0x32F000, 0x3B3000, 0x444000, 0x4D1000`.
  Clips por pack: 108, 105, 99, 99, 99, 99, 99, 98, 99, 99 (= 1004).
- Entrada de tabla (9 bytes, LE): `[addr u32][size u32][prompt_id u8]`.
  `addr` está en coordenadas del pack 0 (base 0x2000);
  **dirección real = pack_base + (addr − 0x2000)**.
- El `prompt_id` es consistente entre packs: mismo id = mismo mensaje,
  traducido. Algunos ids son **idénticos bit a bit en los 10 packs**
  (independientes del idioma — tonos/melodías):
  - **id=2** (1,6 s, contenido tonal) → candidato a **melodía de bienvenida**.
  - **id=13** (0,13 s, pico espectral fuerte) → candidato a **beep de tecla**.
  - id=39, id=60 (0,3 s) → tonos cortos.
- El header de 6 bytes de cada pack (ej. `6c3c d0060000`) aún no está
  decodificado (¿CRC? ¿contador?). Baja prioridad.
- **Herramienta:** `catalog_audio.py` regenera todo desde el dump
  (`python3 catalog_audio.py dump1.bin`). Catálogo completo en
  `catalogo_audio.csv`. Extracción organizada en `audio_extraido/packNN/`.
- **Idiomas identificados** (escucha del usuario, 2026-08-12):
  pack00 = chino, pack01 = inglés, pack03 = portugués, **pack07 = español**.
  Pendientes: pack02, 04, 05, 06, 08, 09.

### Prompt IDs mapeados (escucha del usuario + verificación de duraciones)

| prompt_id | Significado | Notas |
|---|---|---|
| 2 | **Jingle de bienvenida** (boot) | idéntico en los 10 packs (1,6 s, tonal) |
| 5 | Tarjeta no registrada | 0,7 s, 2 variantes entre packs |
| 13 | **Beep de tecla** | 0,13 s, tono puro |
| 16 | Tono tipo timbre (tecla `#` en ciertos contextos) | |
| 35 | Sirena de alarma | |
| 39 | Error (tono grave) | |
| 40 | Rebote tipo pin-ball | |
| 60 | Desconexión | |
| 61 | Error (variante de tono del 39) | |
| 71–79 + 90 | **Dígitos hablados 0–9** (10 clips cortos ~0,6 s, orden ascendente) | verificar orden exacto al completar la tabla de strings |
| 221–230 | **"Por favor presione 0"…"9"** (10 frases, 1,7–1,9 s) | |

- Los tonos (2, 5, 13, 16, 35, 39, 40, 60, 61) están duplicados
  bit-idénticos en todos los packs → son recursos globales; en la
  librería propia alcanza con guardarlos una sola vez.
- El usuario está armando la tabla prompt_id → texto (string) completa;
  al tenerla, mergear como columnas `texto_es` / `evento` en
  `catalogo_audio.csv` — esa tabla es la spec de UX sonora para la app final.

### Implicancias para la librería `audio`

- El reproductor del ESP32-S3 debe decodificar **IMA-ADPCM WAV a 8 kHz**
  (decoder IMA es ~50 líneas de C; también se puede re-encodear a PCM
  8/16-bit si el presupuesto de flash propio lo permite).
- Mantener el esquema **prompt_id + pack de idioma** da paridad OEM y
  multi-idioma gratis: se puede portar la tabla tal cual.
- Si el beep/tonos terminan confirmándose en flash (id=13, id=2), no hay
  nada sintetizado por PWM que replicar — todo es reproducción de clips.
  **A verificar en fase 4 (sniffer):** qué direcciones lee el CPU en cada
  evento y si coinciden con estos ids.

## 4ter. RESULTADOS — Caracterización del front-end de audio (2026-08-12) ✅

### El FR8018H tiene CODEC de audio (cambia el modelo)

- Datasheet FR8018H (lo tiene el usuario): **16-bit mono audio CODEC
  embebido**, DAC SNR 96 dB / THD −86 dB, sample rates 8/16/44.1/48 kHz.
  Salida analógica diferencial en pines 16/17 (AOP/AON); puede incluso
  manejar un parlante chico directo.
- **NO hay PWM ni filtro pasa-bajos en la placa** — por eso no se
  encontraba el filtro: el OEM no lo necesita. Confirmado por scope
  (ver abajo).

### Circuito levantado (audio_schem.jpg)

- INP e INN conectados al CPU, cada uno con **Cin** (desacople DC) +
  **Rin** (seteo de ganancia) → entrada **diferencial** del CS8302.
- **Hallazgo:** el OEM agrega **Rf externas salida→entrada**
  (VO−→IN− y VO+→IN+), lazo alrededor de todo el ampli:
  - La fórmula del datasheet `Av = 2×(150k/Rin)` vale sin Rf externo;
    con Rf ≪ 150 kΩ domina el lazo externo → **Av ≈ Rf/Rin**, más baja
    y precisa. Razón: el DAC del FR8018H entrega señal fuerte
    (diferencial, cerca de rail-to-rail) — con la ganancia nominal
    habría clipping. Bonus: mejor linealidad y PSRR.
  - High-pass de entrada: `fc = 1/(2π·Rin·Cin)`.
- **Drive diferencial (contrafase)**: +6 dB de swing gratis, rechazo de
  modo común (vecino: antena 13,56 MHz), retorno de corriente cancelado.
  Lo genera el CODEC por hardware — costo cero para el firmware OEM.

### Evidencia de scope (RIGOL DHO924S, puntas en salida del CPU)

- `audio_keypressed_beep.png` — CH1=INP, CH2=INN, CH3=SD:
  beep de tecla = **tono 1,01 kHz, ~120 ms** (consistente con id=13,
  0,13 s). Secuencia: **SD sube → ~20 ms de rampa DC** (carga de Cin al
  common-mode) → tono → SD baja al finalizar.
- `audio_door_bell.png` — tecla `#` (id=16): melodía de 2 notas ~2,3 s,
  SD alto durante toda la reproducción, baja ~0,4-0,5 s después.
- Señales analógicas montadas en continua, contrafase confirmada.

### Impacto en la librería `audio` (replanificado)

- ESP32-S3 **no tiene DAC** (el ESP32 original sí; el S3 solo I2S digital).
  No se puede acoplar sin agregar nada, pero los pasivos van **en el
  adapter board** (hardware propio), no en la cerradura:
  ```
  IMA-ADPCM decode (SW) → PCM 8 kHz → upsample
    → MCPWM complementario 2 GPIO (~64 kHz) [paridad diferencial]
      o LEDC single-ended [v0 de harness]
    → RC LPF (adapter, fc ≈ 20 kHz) → pads INP/INN → CS8302
  SD: GPIO con timing OEM (assert ~20 ms antes; release con cola ~0,4 s)
  ```
- Descartado: DAC I2S externo (BOM extra innecesario); PWM crudo sin
  filtro (EMI/intermodulación junto al lector 13,56 MHz — solo como
  experimento).
- La paridad se mide en lo que ve el CS8302 (nivel, banda, timing SD),
  no en el mecanismo interno del CPU.

### Mediciones pendientes (próxima sesión de lab)

1. Valores de **Rin, Cin, Rf ×2** (placa apagada / levantar terminal).
2. Amplitud Vpp y common-mode de AOP/AON (fina, no 2 V/div).
3. Timing exacto de SD: lead (~20 ms beep) y cola (~0,4-0,5 s voz) por
   tipo de evento.
4. Alimentación del CS8302: ¿batería directa o regulada?

## 4qua. Mediciones de lab + PLAN DE DISEÑO CONSOLIDADO (2026-08-14) ✅

### Valores medidos — DESOLDADOS (lab, definitivos)

| Parámetro | Valor real | In-circuit (descartado) | Fuente |
|---|---|---|---|
| Rin (×2) | **100 kΩ** | 20 kΩ (contaminado por AOUT en paralelo) | medición desoldada |
| Rf (×2, VO∓→IN± cruzado) | **820 kΩ** (nominal datasheet 800 kΩ) | 150 kΩ | medición desoldada |
| Cin (×2) | **100 nF** | 10 µF | medición desoldada |
| SD lead time | **30 ms** | — | `audio_cs_lead_time_30mS.png` |
| SD tail time | **16 ms** | — | `audio_cs_tail_time_16mS.png` |
| Amplitud INP | **3,34 Vpp** (Vpp(C1) medido) | — | `audio_INP_VPP_3.3V.png` |
| Swing diferencial (INN−INP) | ≈ 6 Vpp (contrafase) | — | `audio_math_ch2-ch1.png` |
| Alimentación CS8302 | 3,3 V (regulador) | — | medición |

Derivados con valores reales: ganancia diferencial **Av = Rf/Rin =
820k/100k = 8,2 V/V (18,3 dB)** (topología totalmente diferencial,
feedback cruzado VO+→IN− / VO−→IN+; patrón de la familia CS8302/XS8302/
CST8302S: Av = Rf/Rin); high-pass de entrada **fc = 1/(2π·100k·100nF) ≈
15,9 Hz** — el cap chico NO recorta graves (con 100 kΩ no hace falta más);
el límite de graves lo pone el parlante. Cs de 1 µF = bypass de VDD/PVDD.

### Datasheet FR8018H verificado (FR8018H_CPU.pdf)

- Pines **16/17 = AOUT_P/AOUT_N** (AO, "Speaker output positive/negative");
  pin 15 **VMID** (tensión de modo común del CODEC).
- DAC mono §3.4: Fs 8–48 kHz, SNR 92 dB (A-w), **ganancia digital
  −48…+32 dB**, ganancia analógica 0…−30 dB, full-scale **1500 mV
  @ VDDA=2,9 V**.
- El 3,3 Vpp medido (> full-scale nominal) implica que el OEM usa ganancia
  digital positiva, probablemente **por prompt** (beep saturado a propósito
  para máximo volumen; voz más baja). A confirmar midiendo INP durante un
  prompt de voz.

### Inconsistencia de ganancia — RESUELTA (medición desoldada)

6,6 Vpp diferencial de entrada × Av=8,2 ≈ 54 Vpp pedidos a un ampli
alimentado con 3,3 V (máx. físico ≈ 6,6 Vpp BTL) → **el OEM opera el
CS8302 en saturación deliberada: todo entra cerca del full-scale del codec
(−2 dBFS) con ~18 dB de ganancia de más**. El beep sale casi cuadrado
(más fuerte y penetrante; los armónicos no los reproduce el parlante) y la
voz sale comprimida/distorsionada pero inteligible y fuerte — consistente
con cómo suenan estas cerraduras. No era medición contaminada de nivel,
era clipping intencional. **Consecuencia para la paridad (D2):** nuestro
PWM full-scale (3,3 Vpp/rama = 6,6 Vpp dif) reproduce EXACTAMENTE el
mismo comportamiento; `vol` baja la entrada → menos clipping → más limpio
y más bajo (así se mapea el menú de volumen 92/93/94/120). El target de
calibración sigue siendo el swing en VO±/parlante, no la entrada.
**Experimento decisivo pendiente:** si la placa OEM aún corre con los
componentes resoldados, capturar VO± durante un prompt OEM y comparar la
forma (rails planos) contra la nuestra a `vol 100`.

### PLAN ACORDADO — arquitectura `components/audio/` (MCPWM complementario)

```
flash: pack de idioma (ADPCM tal cual, 4:1) + tonos globales deduplicados
   │
   ▼
audio_decoder   IMA-ADPCM → PCM s16 @ 8 kHz (on-the-fly, ~nulo costo CPU)
   ▼
audio_player    cola de prompt_id + upsample ×4 (interp. lineal) → 32 kHz
   ▼
audio_driver    MCPWM complementario 180°, carrier 156,25 kHz / 10 bit
                (160 MHz/1024); duty por ISR a 32 kHz; rampa anti-pop;
                SD GPIO: assert 30 ms antes, release 16 ms después
   ▼  hardware adapter board (2R + 2C, esfuerzo mínimo acordado)
RC LPF por rama  fc ≈ 20 kHz  (R=1 kΩ, C=8,2 nF; carga Rin=20 kΩ ≈ −5%)
   ▼
pads INP / INN  →  CS8302 (Av≈7,5, fc HP 0,8 Hz)  →  parlante
```

Decisiones y números clave:

- **Carrier 156,25 kHz, 10 bit** (1024 pasos @ 160 MHz). Residuo de
  switching tras RC de 20 kHz ≈ −18 dB; si en scope resulta audible/EMI,
  plan B de una línea: 312,5 kHz / 9 bit. Lejísimos de 13,56 MHz y de la
  banda de audio; el propio CS8302 ya conmuta en ~cientos de kHz.
- **Modo común propio = 1,65 V durante el play (duty 50% = silencio);
  reposo = GPIOs a GND** (ver §4nov). No importa que el OEM use VMID ≈
  1,0-1,1 V ni que nuestro DC cambie: **los Cin bloquean DC** — lo único
  que llega al ampli es la componente diferencial de AC. El escalón
  común-modo 0↔1,65 V pasa por el HP de 15,9 Hz como un pulso lento
  (τ=10 ms, absorbido por el lead de 30 ms) y es simétrico en ambas ramas
  → diferencial ~0 → sin pop.
- **Amplitud máxima disponible = 3,3 Vpp por rama** (duty 0↔100%), igual
  al máximo medido del OEM. Calibración: ganancia digital por software
  hasta igualar el swing de VO±/parlante del OEM (target a medir).
- **Tiempos SD = paridad exacta OEM**: lead 30 ms / tail 16 ms; rampa de
  duty desde 50% al arrancar y hacia 50% al cerrar (anti-pop, el chip
  también ayuda con su supresión interna).
- **Assets**: se embebe UN solo pack de idioma (~110 clips, ~550 KB ADPCM)
  + tonos globales deduplicados (ids 2/13/16/35/39/40/60/61 idénticos en
  todos los packs → una sola copia). Idioma seleccionable por config.
- **v0 de harness** (opcional, 15 min): LEDC single-ended → INP con INN a
  CM, solo para validar decoder + player antes de soldar el par RC doble.

### Validación (D4, contra capturas OEM ya guardadas)

1. Scope en pad INP: id=13 → 1,0113 kHz / ~120 ms / ≈3,3 Vpp — A/B con
   `audio_keypressed_beep.png`.
2. Scope en VO± (o parlante): mismo evento OEM vs nuestro → ajustar
   ganancia digital hasta igualar Vpp (éste es el criterio de paridad real).
3. Lead/tail de SD: 30/16 ms — A/B con capturas `audio_cs_*.png`.
4. Escucha: tabla completa de prompt_ids (la que está armando el usuario).
5. EMI: polling MIFARE continuo mientras suena sirena (id=35) — la lectura
   de tarjetas no debe degradarse.

### Mediciones pendientes (usuario, próxima sesión de lab)

1. Desoldar y medir Rin / Rf / Cin reales.
2. Vpp en INP durante un **prompt de voz** (¿el OEM baja la ganancia?).
3. **Swing en VO± o en el parlante** durante beep y voz → target de calibración.
4. (Opcional) Modo común: scope Math **(CH1+CH2)/2** — la resta CH2−CH1 ya
   hecha da el swing diferencial, no el CM. No bloquea: Cin bloquea DC.

---

## 4quin. Vpp de voz, análisis de "duplicados" y tabla de mensajes (2026-08-14) ✅

### Vpp de prompts de voz (scope, pack 7)

| Clip | Texto | Vpp(C1) | Captura |
|---|---|---|---|
| id=17 | "por favor verifique el administrador" (* luego #) | **3,157 V** | `audio_msg_17_vpp.png` |
| id=48 | "el sistema está bloqueado" (reintentos admin inválido) | **3,121 V** | `audio_msg_48_vpp.png` |

**Conclusión:** la voz sale al mismo nivel que el beep (3,34 Vpp) — el OEM
**no varía la ganancia por prompt**: todo se reproduce cerca de full-scale
(3,3 Vpp/rama). Las diferencias de volumen percibido están en el contenido
(RMS de la grabación), no en la ganancia. Simplifica la calibración:
driver a fondo de escala + ajuste fino comparando VO± contra OEM.

### Los "mensajes duplicados" NO son duplicados (verificado en el dump)

Comparación byte a byte dentro del pack 7 (SHA256 del clip completo):

| Texto | ids | Resultado |
|---|---|---|
| Exitoso | 67, 69, 151 | 3 tomas distintas (96% bytes difieren; 151 más largo) |
| Fracaso | 68, 163, 250 | 3 tomas distintas |
| Por favor introduzca de nuevo | 26, 131 | mismo tamaño (9054 B) pero 93,7% bytes difieren → tomas distintas |
| Restaurar configuración de fábrica | 34, 142 | tamaños distintos (9822/9566) |
| Introduzca la información de desbloqueo | 124, 141 | tamaños distintos (9822/10078) |
| Desbloqueado | 52, 54 | tamaños distintos (4446/4190) |

**Decisión:** no se elimina nada. El firmware OEM referencia por
**prompt_id**, no por texto — cada id se usa en un contexto distinto y la
toma puede tener prosodia diferente (afirmación vs. pregunta vs. menú).
Para nuestra librería: catálogo por id; al mapear evento→id podemos
audicionar ambas tomas y elegir la que mejor suene (mejora de UX gratis).
(Ojo: esto NO contradice la dedup de §4bis — los *tonos* sí son
bit-idénticos **entre packs**; los "duplicados" de voz son **dentro** del
mismo pack y todos difieren.)

### ¿Se usan todos los ids? (pregunta abierta)

Vía barata (acordada): sniffer SPI (§4.3 fase 4) da el mapa evento→id
empírico para los flujos que ejercitemos. Vía cara (opcional): Ghidra —
localizar la función `play_prompt(id)` por xref a la tabla del pack y
buscar los ids como inmediatos en el código ARM. Para el retrofit no es
bloqueante: nosotros definimos nuestro propio mapa evento→id.

### Verificación automática de traducciones (speech-to-text)

Entregado `verificar_traducciones.py` (faster-whisper, corre en Ubuntu del
usuario; el sandbox no pudo instalarlo por red):
- `--langs`: detecta el idioma de los **6 packs sin identificar**
  (02, 04, 05, 06, 08, 09) usando el clip más largo de cada uno.
- Normal: transcribe packs 0 (zh), 1 (en), 7 (es) y genera
  `compara_por_id.csv` lado a lado → detectar malas traducciones y
  aclarar mensajes ambiguos.

### Idiomas identificados (faster-whisper --langs, 2026-08-14) ✅

| pack | idioma | prob | | pack | idioma | prob |
|---|---|---|---|---|---|---|
| 00 | zh (chino) | 0.99 | | 05 | ru (ruso) | 1.00 |
| 01 | en (inglés) | 0.99 | | 06 | th (tailandés) | 0.98 |
| 02 | vi (vietnamita) | 0.98 | | 07 | es (español) | 1.00 |
| 03 | pt (portugués) | 0.94 | | 08 | id (indonesio) | 0.95 |
| 04 | ar (árabe) | 0.99 | | 09 | de (alemán) | 1.00 |

### Mapa canónico id→mensaje (entregable: `mapa_prompt_ids.csv`)

Cruce de: lista curada por el usuario (pack 7) + transcripciones Whisper
zh/en/es (`compara_por_id.csv`). Hallazgos:

1. **Whisper sobre tonos alucina** ("subtítulos Amara.org", "You", etc.) —
   confirma automáticamente cuáles ids son tonos (2, 5, 13, 16, 35, 39, 40,
   60, 61) y cuáles voz.
2. **Los ids NO son universales entre packs**: zh tiene prompts extra
   (18 "ingrese huella", 22 "test", 50 "usuario ya existe", 100/101/102
   mute/idioma/chino, 120 "English", 133, 140) y algunos corridos un número
   (es 51 "usuario ya existe" = zh 50). La librería embebe UN pack con su
   propia tabla — no asumir id X = mismo significado en otro idioma.
3. **Disambiguaciones resueltas con zh/en**:
   - id 145 (# para confirmar) **no existe en pack 7**; id 146 es
     "presione * para SALIR" (zh 退出请按*号键 / en "press the asterisk to
     exit") pero la grabación española dice "# para confirmar" → **bug de
     traducción del OEM**. Verificar a oído en qué flujo se usa.
   - id 164/168 = activar/desactivar **modo pasaje** (zh 打开/关闭常开),
     NO "activar/cerrar configuración" como dice el español.
   - id 250 = **fallo de configuración de red** (zh 配网失败), no fracaso
     genérico — confirma la intuición del usuario.
   - id 169/180 = autenticación doble ON/OFF (zh 组合/单一开锁) —
     "desbloqueo combinado/normal".
   - id 131 = "reingrese la NUEVA contraseña" (flujo cambio de clave),
     distinto de 26 (reintento genérico).
   - id 69 = "borrado exitoso" (zh 删除成功), 67 = "operación exitosa",
     151 = éxito genérico: las tomas "duplicadas" tienen matices de contexto.
   - id 53: zh/en dicen "Locked" pero el español grabó "Apagado" — otra
     probable mala traducción.
   - id 141: zh/en = "gestión del sistema", pero el español repite el texto
     de 124 → probable error del OEM en el pack español.
   - id 221: zh 请按铃 es homófono de 请按零 → "presione cero" en los tres
     idiomas (test de teclado 221–230).
4. **Prompts componibles**: 232 ("por favor presione…") + 233/234/248
   (huella/contraseña/tarjeta) — el firmware concatena clips. Nuestra
   librería debe soportar colas de ids (ya previsto en `audio_player`).

### Tabla de mensajes pack 7 (español) — relevada por el usuario

| id | dur s | mensaje | | id | dur s | mensaje |
|---|---|---|---|---|---|---|---|
| 0 | 3.20 | Por favor registre la contraseña del administrador | | 68 | 0.77 | Fracaso |
| 2 | 1.60 | init jingle (tono) | | 69 | 0.83 | Exitoso |
| 5 | 0.70 | sonido acceso denegado (tono) | | 70 | 2.05 | Ajuste del bloqueo de la puerta |
| 13 | 0.13 | key pressed (beep 1,01 kHz) | | 71–79 | ~0.6 | cero…ocho |
| 16 | 2.37 | door bell | | 90 | 0.64 | nueve |
| 17 | 2.11 | por favor verifique el administrador | | 91 | 1.34 | control del volumen |
| 22 | 2.43 | por favor introduzca la huella dactilar | | 92 | 1.02 | volumen alto |
| 23 | 1.92 | bloqueo de seguridad activado | | 93 | 1.09 | volumen medio |
| 24 | 2.18 | bloqueo de seguridad desactivado | | 94 | 1.09 | volumen bajo |
| 26 | 2.24 | por favor introduzca de nuevo | | 120 | 0.90 | silenciar |
| 34 | 2.43 | restaurar la configuración de fabrica | | 121 | 1.47 | añadir un usuario |
| 35 | 1.22 | sirena de alarma (tono) | | 122 | 1.47 | añadir administrador |
| 39 | 0.32 | alerta grave, tecla errónea (tono) | | 123 | 2.11 | añadir usuarios comunes |
| 40 | 1.22 | do-do-da-da (tono) | | 124 | 2.43 | introduzca la información de desbloqueo |
| 45 | 3.39 | Batería insuficiente, por favor reemplace la batería | | 125 | 1.41 | eliminar un usuario |
| 46 | 1.47 | Se agregó con éxito | | 126 | 1.98 | eliminar usuarios por número |
| 47 | 1.22 | Falló la adición | | 127 | 1.73 | especifica la eliminación |
| 48 | 1.66 | El sistema está bloqueado | | 128 | 2.18 | por favor, introduzca el número |
| 49 | 1.79 | El almacenamiento está lleno | | 129 | 2.75 | no se puede eliminar el administrador único |
| 51 | 1.66 | El usuario ya existe | | 130 | 1.41 | falló la verificación |
| 52 | 1.09 | Desbloqueado | | 131 | 2.24 | por favor introduzca de nuevo |
| 53 | 0.70 | Apagado | | 132 | 1.73 | contraseña inconsistente |
| 54 | 1.02 | Desbloqueado | | 134 | 1.41 | usuarios vacíos |
| 58 | 0.90 | Números | | 141 | 2.50 | introduzca la información de desbloqueo |
| 59 | 1.41 | Conexión exitosa | | 142 | 2.37 | restaurar la configuración de fabrica |
| 60 | 0.32 | cerrar app (tono) | | 143 | 1.73 | configuración inalámbrica |
| 61 | 0.38 | vibración grave corta (tono) | | 144 | 1.60 | salir de la configuración |
| 62 | 1.34 | Ajustes de función | | 146 | 2.43 | pulse la tecla almohadilla para confirmar |
| 63 | 1.34 | Alarma anti-robo | | 148 | 1.73 | establecer correctamente |
| 64 | 1.54 | Encender la alarma | | 150 | 0.96 | en conexión |
| 65 | 1.41 | Apagar la alarma | | 151 | 0.90 | exitoso |
| 66 | 1.79 | Modo normalmente abierto | | 163 | 0.90 | fracaso |
| 67 | 0.83 | Exitoso | | 164 | 1.54 | activar configuración |
| | | | | 168 | 1.66 | cerrar la configuración |
| 169 | 1.54 | Desbloqueo combinado | | 180 | 1.34 | desbloqueo normal |
| 181 | 2.30 | huellas dactilares y contraseña | | 182 | 1.98 | huellas dactilares y tarjeta |
| 187 | 1.54 | contraseña y tarjeta | | 188 | 1.98 | verificar huellas dactilares |
| 189 | 1.79 | contraseña de verificación | | 220 | 1.92 | por favor verifique la tarjeta |
| 221–230 | ~1.7 | por favor presione cero…nueve | | 232 | 1.34 | por favor presione |
| 233 | 1.15 | huella dactilar | | 234 | 0.90 | contraseña |
| 248 | 0.70 | tarjeta | | 250 | 0.90 | fracaso |

Huecos de ids observados: 80–89 (dígitos = 71–79 + 90), 95–119, 133,
135–140, 145, 147, 149, 152–162, 165–167, 170–179, 183–186, 190–219, 231,
235–247, 249 — probablemente reservados o usados por otros packs (pack 0
tiene 108 clips, pack 7 tiene 98 + id 0).

---

## 4sex. Manual OEM: árbol de menús de voz y mapa evento→id (2026-08-14) ✅

Fuente: fotos del manual impreso (inglés), 4 páginas, legibles. El manual
llama "Voice broadcast" a cada prompt. Datos de contexto: estado de fábrica
password `123456` (se borra tras el primer alta o conexión APP); reset de
fábrica alternativo = botón Set trasero 5 s.

### Árbol de menús (OEM) mapeado a prompt_ids del pack español

```
ENTRADA AL MENÚ: iluminar teclado + presionar "*#" (o botón Set trasero)
  → id 17 "por favor verifique el administrador" → auth → menú principal

MENÚ PRINCIPAL — broadcast COMPUESTO (dígito + ítem, uno tras otro):
  "1"(72)+id121 añadir usuario
  "2"(73)+id125 eliminar un usuario
  "3"(74)+id70  ajuste del bloqueo (= System settings)
  "4"(75)+id143 configuración inalámbrica

  [1] AÑADIR:
      "1"+id122 añadir administrador / "2"+id123 añadir usuarios comunes
      → id 122/123 + id 124 "introduzca la información de desbloqueo"
      → enroll huella/tarjeta/clave (claves de 4 a 10 dígitos)
      → éxito id 46 "se agregó con éxito" / fallo id 47 / id 51 "ya existe"

  [2] ELIMINAR:
      "1"+id126 eliminar por número / "2"+id127 por identificación
      → id 128 "introduzca el número" → # confirma
      → COMPUESTO: id 58 "número" + dígitos (71-79/90) + id 69 (borrado
        exitoso — por eso zh 删除成功 ≠ 67 operación exitosa)
      → id 129 si es el único administrador

  [3] AJUSTES DE LA CERRADURA (System settings):
      "1"+id62  ajustes de función
         → "1"+id66 modo normalmente abierto → id 164 ON / id 168 OFF
           (con modo pasaje ON, * bloquea o APP)
         → "2"+id169 desbloqueo combinado
           → "1"+id180 simple / "2"+id181 huella+clave /
             "3"+id182 huella+tarjeta / "4"+id187 clave+tarjeta
           (al activar un combo, el desbloqueo simple queda inválido)
         → "3"+id63 alarma anti-robo → id 64 ON / id 65 OFF
      "2"+id91  control de volumen
         → "1"+id92 alto / "2"+id93 medio / "3"+id94 bajo / "4"+id120 mute
           (confirma: en pack ES id120 = silenciar)
      "3"+id142 restaurar fábrica
         → + id 145 "presione # para confirmar" — ¡id 145 NO EXISTE en el
           pack español! Su texto fue grabado en el id 146 español
           (ver resolución */# abajo)

  [4] RED: id 143 → id 150 "en conexión" → id 59 éxito / id 250 fallo red

SALIDA: id 144 "salir de la configuración"
FÁBRICA: id 0 "registre la contraseña del administrador" (setup inicial)
SUELTOS: id 16 timbre (tecla #), id 48 sistema bloqueado, id 45 batería
baja, id 5 acceso denegado, id 130 verificación fallida, id 23/24 bloqueo
de seguridad on/off.
```

### Resolución del misterio */# (id 145/146)

El manual (reset de fábrica, método 1) dice textual: *"Restore factory
settings, please press # to confirm"*. En zh/en eso es id 142 + **id 145**.
El pack español nunca grabó el 145: grabó su texto ("pulse la tecla
almohadilla para confirmar") en el **id 146**. Consecuencias:

- **No es bug funcional en español**: el flujo pide # y se confirma con #.
- El pack español **carece** del prompt zh/en 146 ("presione * para salir",
  usado p.ej. para bloquear en modo pasaje). Si nuestro retrofit lo
  necesita, hay que generarlo (ver pipeline TTS abajo).
- En zh/en, 146 = "press the asterisk to exit".

### Decisiones derivadas

1. **Idiomas**: v1 = español (paridad D2 con el uso real del usuario).
   La arquitectura deja el pack como tabla + blobs independiente → agregar
   en/zh después es solo cambiar el pack embebido. El OEM solo expone
   zh/en en su menú de idiomas (ids 101/102/120, ausentes en español);
   nuestro retrofit puede elegir idioma por config/MQTT en vez de menú.
   Nota: los 10 packs completos ocupan ~5 MB ADPCM — si el módulo S3 tiene
   flash ≥ 8 MB se podrían embeber varios.
2. **Prompts compuestos confirmados como patrón central del OEM**
   (dígito + ítem de menú; "número" + dígitos + resultado) → la cola de
   ids del `audio_player` no es opcional, es EL mecanismo.
3. **Prompts faltantes/erróneos en español** (145, "* para salir", y los
   textos mejorables 53/134/141/164/168/250) → se generan con TTS offline
   (pipeline abajo), misma calidad de pipeline que el resto del pack.

### Pipeline TTS offline → clip compatible con el pack (para PC Ubuntu)

El OEM codificó todo con FFmpeg (Lavf58.29.100): IMA-ADPCM 8 kHz mono,
4 bits, block_align 256 (505 samples/bloque). Reproducir el formato es
trivial:

```bash
# 1) generar voz (edge-tts gratis, calidad neural):
pip install edge-tts
edge-tts --voice es-ES-ElviraNeural \
  --text "Pulse la tecla asterisco para salir" -o tts_tmp.mp3

# 2) codificar EXACTAMENTE como el OEM:
ffmpeg -i tts_tmp.mp3 -ar 8000 -ac 1 -c:a adpcm_ima_wav -block_size 256 \
  prompt_145_es.wav
```

El builder del pack (a escribir en la fase de código) inserta el clip y
agrega su entrada de 9 bytes a la tabla. **TTS on-device (espeak-ng en el
S3) queda descartado para v1**: calidad robótica y ~200 KB de RAM por algo
que se resuelve mejor offline; si la flota MQTT algún día necesita mensajes
dinámicos hablados, la vía sensata es renderizar TTS en el servidor y
empujar el WAV por MQTT/OTA, no sintetizar en el lock.

---

## 4sep. Decisiones de arquitectura v1 (2026-08-14) ✅

### Pipeline TTS homologado

`prompt_145_es.wav` generado por el usuario (edge-tts + ffmpeg) verificado
byte a byte contra el formato OEM: codec 0x11 IMA-ADPCM, 8 kHz, mono,
4 bits, block_align 256, 505 samples/bloque — idéntico. Única diferencia:
`byterate` del header (16000 vs 4055; cambio de cálculo de ffmpeg, no lo
lee ningún decoder). **Podemos fabricar prompts nuevos/corregidos.**

### Actualización de duty: ISR vs DMA

- MCPWM y LEDC **no tienen camino de DMA** en el ESP32-S3.
- **v1 = ISR de timer a 32 kHz** escribiendo 2 registros de duty
  (~0,5 µs por ISR → ~1,5% de un core; FreeRTOS convive con tasas muy
  mayores). Clave: la portadora de 156,25 kHz la genera el hardware — si la
  ISR se retrasa, el duty se aplica un período de portadora más tarde
  (6,4 µs); no hay glitch ni artefacto audible. La cerradura está ociosa
  el 99,9% del tiempo.
- **Plan B (v2, si el scope o la carga CPU lo piden):** I2S0 como motor de
  bitstream genérico (no como codec): LUT de 256 patrones PWM + DMA de I2S;
  en modo LCD paralelo saca D0/D1 en contrafase → diferencial puro con CPU
  ≈ 0. Más complejo (empaque de bits, doble buffer). La interfaz del driver
  se diseña para que el backend (ISR vs I2S-DMA) sea intercambiable.

### Almacenamiento de los clips en la flash del S3

- **v0/harness:** `EMBED_FILES` de ESP-IDF (arrays binarios) — cero partes
  móviles para iterar.
- **v1:** **partición cruda `audio`** con formato propio derivado del OEM
  (header + entradas de 9 bytes `[addr u32][size u32][id u8]` + blobs ADPCM
  contiguos, headers WAV eliminados — los parámetros de bloque son
  constantes, van hardcodeados). NO filesystem: la tabla ES el índice.
  - Lectura con `esp_partition_read` al reproducir: clip máx. ~14 KB → se
    carga entero a RAM y se decodifica desde ahí (streaming interno).
  - Actualizable por OTA **independiente del firmware**; soporta varios
    packs (es/en…) como secciones con base distinta; idioma por NVS/config.
- **Builder offline (Python):** CSV de ids + WAVs (extraídos del dump +
  TTS nuevos) → imagen de partición. Se flashea con esptool u OTA.

### Mapa de módulos y flujo de datos

```
audio_assets    id → (offset,size) en la tabla de la partición; carga blob
audio_decoder   IMA-ADPCM bloques 256B → PCM s16 @ 8 kHz (predictor+step)
audio_player    task FreeRTOS: cola de pedidos (cada pedido = lista de ids,
                menús compuestos), decodifica + upsample ×4 → ring 32 kHz
                s16 (2048 muestras = 64 ms = 4 KB); aplica volumen digital
audio_driver    MCPWM 156,25 kHz/10 bit complementario + ISR 32 kHz
                (ISR tonta: lee s16 → escala → 2 escrituras de registro);
                SD GPIO lead 30 ms / tail 16 ms; rampa anti-pop 5 ms
API             audio_init(cfg) / audio_play(id) / audio_play_seq(ids,n) /
                audio_say_number(n) / audio_set_volume(0-100) / audio_stop()
                / audio_is_busy()
```

RAM total estimada: ≤ 20 KB (clip 14 KB + ring 4 KB + estados). Sin PSRAM.
Volumen 100% = 3,3 Vpp/rama = paridad OEM. Volumen <100% = feature extra
de la flota (el OEM lo tiene como menú: ids 91-94 + 120).

---

## 4oct. `audio_decoder` IMPLEMENTADO y validado (2026-08-14) ✅

Primer módulo de `components/audio/`, validado por software (D4) sin
hardware: compilado con gcc en host y comparado **bit-exacto contra ffmpeg**
(el mismo encoder del OEM) sobre el corpus completo.

- **Resultado: 1004/1004 clips del dump decodifican byte a byte idénticos
  a ffmpeg**, incluyendo el clip TTS generado por el usuario
  (`prompt_145_es.wav`, 48 bloques, 3,03 s) — el pipeline TTS queda
  homologado también en decodificación.
- Hallazgos de formato (afectan al builder del pack):
  - El dump mezcla **dos layouts WAV**: con chunk `fact` (payload en
    offset 60) y sin `fact` (payload en offset 44). El builder debe
    caminar los chunks RIFF, no asumir offset fijo.
  - Los 1004 clips tienen payload **múltiplo exacto de 256** (bloques
    completos); el decoder igual tolera bloque final corto.
- Bugs encontrados por la validación (y corregidos):
  1. **Sample perdido por bloque** (504 en vez de 505): el nibble alto del
     último byte de cada bloque se descartaba al saltar al siguiente header.
     Regla: el bloque solo se da por agotado cuando no hay nibble pendiente.
  2. **Variante del diff**: ffmpeg usa la forma multiplicativa
     `((2*delta+1)*step)>>3` (no la suma de shifts). Diferencia de ±1 LSB —
     inaudible, pero la bit-exactitud da validación total.
- API: `audio_decoder_init/read/rewind` + helpers `total_samples` y
  `duration_ms` O(1). Sin malloc, sin float, ~40 bytes de estado, pensado
  para alimentar el ring buffer del `audio_player` en chunks arbitrarios
  (testeado con pedidos de 773 samples que cortan bloques y bytes).
- Archivos: `components/audio/include/audio_decoder.h`,
  `components/audio/src/audio_decoder.c`, `CMakeLists.txt`.

---

## 4nov. `audio_driver` + `audio_player` + harness `audio_test` (2026-08-14) ✅

Librería completa v0 + app IDF de prueba por consola (`audio_test.zip`,
autocontenida: incluye `components/audio/` y 28 clips reales del pack 07
embebidos por `EMBED_FILES`, ~110 KB).

### audio_driver (v0, ISR)

- MCPWM complementario: gen_p HIGH@TEZ/LOW@CMP; gen_n por módulo de
  dead-time con 0 ticks + invert_output (contrafase exacta; no hay medio
  puente, así que dead-time real no aplica).
- Portadora 156,25 kHz / 10 bits (160 MHz/1024); **fallback automático a
  40 MHz/256 ticks (8 bits)** si el clock de 160 M no está disponible.
- Duty por ISR de gptimer a **32 kHz exactos** (8 MHz/250). Solo la ISR
  toca el compare post-init (sin contención). Volumen (0..100) y rampa
  anti-pop (~4 ms, Q8) en punto fijo dentro de la ISR.
- Ring = FreeRTOS StreamBuffer (ISR-safe, 128 ms). Underrun → silencio +
  contador (`under` en consola).
- Secuencia SD completa en ISR: lead 30 ms (silencio a duty 50%) → rampa →
  audio → al drenar: rampa abajo → tail 16 ms → SD LOW. Tiempos = medidos
  OEM (scope).

### audio_player

- Task + cola (4 pedidos). Cada pedido = **lista de ids encadenados sin
  gap** (patrón de menús compuestos del OEM).
- Pipeline por clip: blob → decoder (8 kHz) → upsample ×4 lineal entero →
  driver_write bloqueante.
- Clips inyectados por **callback `get_blob`** → el player no sabe de dónde
  salen los bytes (v0: arrays; v1: partición `audio`).
- `audio_say_number(n)`: dígitos id 71-79 + 90 (0="cero", 9→id90).

### Harness audio_test (consola UART 115200)

`play <id>` / `seq <id> [id...]` / `say <n>` / `vol <0-100>` / `stop` /
`under` / `clips`. Pines default: PWM_P=GPIO4, PWM_N=GPIO5, SD=GPIO6
(editables arriba de main.c). Build: `idf.py set-target esp32s3 &&
idf.py build flash monitor`.

### Debug de banco (iteración 2 del harness)

- Build IDF v6.1 master: Kconfig `*_ISR_IRAM_SAFE` → `*_ISR_CACHE_SAFE`;
  drivers separados (`esp_driver_mcpwm/gptimer/gpio`); APIs variádicas de
  acciones MCPWM removidas → versión de acción única con struct.
- Pitfall zip: mtimes futuras → loop infinito "Re-running CMake". Fix del
  lado del usuario: `find . -exec touch {} +`; el zip se regenera con
  fechas normalizadas (2024-06-01).
- Bug WDT ×2: (a) `tone` bypaseaba la máquina de estados → con tone
  activo, un `play` llenaba el ring y nunca drenaba (colgaba en STOPPING);
  (b) `vTaskDelay(pdMS_TO_TICKS(5))` = 0 ticks a 100 Hz → busy-loop que
  mataba IDLE0. Fix: override de tone al FINAL de la ISR (la máquina
  siempre corre) + delay de 20 ms en el player.
- Capturas `rep 13 0` (20 µs/div y 200 µs/div): **modulación de duty
  visible y contrafase espejada confirmada en GPIO4/GPIO5** (el fix del
  gen_n funciona en hardware).
- **Bug ramp=0 en clips cortos (el más fino de la etapa)**: el ring
  (4096 samples = 128 ms) supera al beep (126 ms) → el player escribía el
  clip entero durante el lead sin bloquearse y llamaba `stop()` →
  STOPPING con rampa=0 (solo subía en RUNNING) → el clip drenaba en
  **silencio total con timing de SD perfecto**. `rep` lo enmascaraba:
  como el player se bloquea en el ring lleno, nunca llama `stop()` y el
  estado sí alcanza RUNNING. Lo delataron las capturas post-RC: `play 13`
  sin envolvente vs `rep 13 0` con envolvente limpia. Fix: la rampa sube
  siempre que la ISR consuma un sample real (RUNNING o STOPPING), y un
  stop durante el lead respeta el wake-up de 30 ms antes de drenar
  (paridad OEM: audio 30 ms tras SD↑).
- El LPF del Math del Rigol no baja de 1,5 MHz → inútil para reconstruir
  audio de una portadora de 156 kHz (la deja pasar con −0,2 dB). Métodos
  alternativos sin RC: (a) FFT del Math en banda base 0–5 kHz — el PWM ya
  contiene el tono modulador en su espectro base; (b) modo de adquisición
  High Resolution a 0,5–1 ms/div (promedia y reconstruye la envolvente);
  (c) exportar CSV (20 ms/div, trigger en CH3=SD) y correr
  **`analizar_pwm_csv.py`** (diezmado por bloques + Butterworth 4to orden
  fc=4 kHz cero fase; mide tono/Vpp/duración/diferencial/lead/tail).
  Script validado contra señal sintética: recuperó 1010 Hz / 126,7 ms /
  3,19 V / dif 6,37 V / lead 29,7 ms / tail 15,6 ms de un beep simulado de
  1011,3 Hz / 126 ms / 3,23 V / 30/16 ms. (Detalle: el padding previo al
  filtrado debe ser con la MEDIA local del borde, no con el sample de
  borde — un PWM crudo vale 0 o 3,3 V instantáneos y el escalón de 1,65 V
  ensucia ~2 ms en cada extremo.)

### Banco 2026-08-15: suite completa OK + carga activa en la red AOUT (PENDIENTE diferido)

- **Suite funcional aprobada** (build IDF 6.1 ✓): `play 13`, `play 2`,
  `say 123` (ids 72/73/74), `seq 232 233`, `vol 70`, `under` → todos
  audibles, timing perfecto, SD lead/tail sincronizado con el audio,
  `underruns: 0`. Reposo a GND confirmado a la salida del RC (captura
  `audio_idle_gpio_gnd.png`).
- **Síntoma**: suena bajo y "con recorte". El recorte es ESPERADO por
  diseño (Av=8,2 → umbral de clip ≈ vol 12; todo vol ≥70 clipea, paridad
  OEM). Lo anómalo es la caída de nivel: envolvente del beep post-RC
  **1,96 Vpp sin ampli → 0,86 Vpp con ampli (×0,44, −7,2 dB)**; con
  R serie 1k y Rin=100k debía ser −0,09 dB → existe una carga de
  **~780 Ω** sobre el nodo AOUT.
- **Aislamiento ejecutado** (capturas del 2026-08-15):
  - `audio_vdd_amp_3v3.png`: VDD del ampli PLANO durante el beep →
    sag de fuente descartado (Test C ✗).
  - `audio_c1_INP_c2_GPIO.png`: GPIO pre-1k con swing completo ~3 V y
    mesetas firmes con el ampli conectado → la fuente NO se cae; la
    carga está aguas abajo de la 1k (Test A). DC del nodo ~1,2–1,6 V.
  - Masas devkit↔lock ≈ 0 Ω (Test D ✗). Resistencias RC medidas: 980 Ω ✓.
  - Test E (placa APAGADA, nodo→GND, ambos sentidos): arranca ~100k y
    sube a 2–5 MΩ (carga de caps) → **no hay camino resistivo estático**;
    la carga de ~780 Ω es ACTIVA (solo existe con la placa energizada).
  - Punto de inyección: red AOUT, ANTES del Cin de 100 nF (correcto).
  - El "diodo" que vio el compañero en el pin de audio del CPU: una
    clamp asimétrica aplanaría la envolvente; la caída es proporcional
    y simétrica → lo más probable es que sea la estructura ESD del pin
    vista en modo diodo, no la causa.
- **Sospechosos restantes** (ambos requieren rework ya planificado):
  1. Etapa de salida AOUT del FR8018H activa en reset (driver VMID /
     FET de descarga anti-pop) — es la hipótesis de trabajo.
  2. Componentes ocultos en la red AOUT bajo el PCB azul del teclado
     (no trazable sin desoldarlo).
- **Decisión (acordada)**: DIFERIR la investigación a la desoldadura del
  CPU. Justificación: objetivos del stage cumplidos; la calibración fina
  de nivel se hace igualmente en VO±/parlante al final; ambos sospechosos
  quedan accesibles con el mismo rework ya planificado. Ver P-AUDIO-1 en §6.

### Validación pendiente en lab (A/B vs capturas OEM)

1. `play 13`: 1,0113 kHz / ~120 ms / ≈3,3 Vpp — modulación y contrafase ya
   confirmadas a nivel GPIO (pre-RC) con `rep 13 0`; falta medir post-RC
   en pad INP y comparar con `audio_keypressed_beep.png` (vía RC físico o
   CSV + `analizar_pwm_csv.py`).
2. Math CH2−CH1 → contrafase ~6 Vpp; comparar con `audio_math_ch2-ch1.png`
   (a nivel GPIO el diferencial reconstruido da ~6,4 V en el script).
3. SD lead/tail 30/16 ms vs `audio_cs_*.png` (medible también por CSV/CH3).
   ✅ 2026-08-15: CS sincronizado con el audio en la suite completa.
4. `play 48`: nivel de voz ≈3,1 Vpp vs `audio_msg_48_vpp.png`.
5. VO±/parlante: igualar swing OEM (calibración fina de volumen).
   Hacerla DESPUÉS de resolver P-AUDIO-1 (la carga de ~780 Ω recorta la
   entrada −7 dB; calibrar ahora dejaría el gain corrido cuando se quite).
6. `play 35` en loop + polling MIFARE simultáneo (EMI 13,56 MHz).
7. Verificar reposo a GND: tras boot y tras cada `play`, GPIO4/5 quietos en
   0 V; `diag` debe mostrar `pwm: off(GND)`. ✅ 2026-08-15 (captura
   `audio_idle_gpio_gnd.png`).
8. Portadora: verificar 156,25 kHz a 2 µs/div (el usuario midió "136 kHz"
   sobre fuzz submuestreado; pendiente de confirmación).

---

## 5. Catálogo de eventos sonoros (a completar con evidencia)

| Evento | ¿Clip en flash? | Dirección / longitud | Formato | Estado |
|---|---|---|---|---|
| Boot / inicialización | probable | — | — | pendiente |
| Tecla presionada (beep) | ¿flash o sintetizado? | — | — | pendiente |
| Enroll huella (guía de voz) | probable | — | — | pendiente |
| Verify OK | probable | — | — | pendiente |
| Verify fail | probable | — | — | pendiente |
| Tarjeta OK / rechazada | probable | — | — | pendiente |
| (agregar los que aparezcan) | | | | |

---

## 6. Riesgos / preguntas abiertas

1. **Dump inestable** (hashes que difieren) → revisar reset sostenido,
   NRSTPD a GND, contacto del clip. Plan B: otro clip / desoldar.
2. **Prompts sin header + sniffer insuficiente** (p.ej. CPU lee todo el
   bloque de audio una sola vez al boot y cachea en RAM) → en ese caso las
   ráfagas no discriminan clips; recurrir a entropía + Ghidra (fase 6 plan C).
3. **Formato exótico** (PDM 1-bit, ADPCM propietario) → la fase 5 (scope en
   +IN) es la referencia de tasa y forma de onda para decodificar.
4. **Reset con supervisor externo** en placa (pulsa el reset
   periódicamente) → se detecta como contención intermitente en el dump.
5. **P-AUDIO-1: carga activa de ~780 Ω en la red AOUT** (2026-08-15,
   diferida). Con el ampli conectado, la envolvente post-RC cae ×0,44
   (1,96→0,86 Vpp). Descartados: sag de VDD, masas, fuente GPIO, camino
   resistivo estático. Sospechosos: etapa AOUT del FR8018H activa en
   reset (hipótesis de trabajo) o componentes ocultos bajo el PCB del
   teclado. **Reingreso tras desoldar el CPU**: (a) repetir `rep 13 0`
   y medir envolvente post-RC — debe volver a ~1,96 Vpp; (b) si persiste,
   levantar el PCB del teclado y barrer la red AOUT; (c) si se resuelve,
   repetir la calibración de nivel (ítem 5 de §4nov) y la comparación
   A/B de Vpp. Test opcional previo al rework (5 min, sin desoldar):
   medir DC de la red AOUT con placa energizada, CPU en reset y ESP32
   en reposo — si ≠ 0 V, algo está polarizando activamente esa red.
   → **Ejecutado 2026-08-15: 0,0000 V en ambos lados de la 1k del RC** →
   no hay polarización activa en DC; la hipótesis de trabajo pasa a ser un
   **pull-down activo** (FET de descarga anti-pop del AOUT del FR8018H en
   reset, u oculto bajo el PCB del teclado) que carga solo la señal AC.

---

## 7. Entregables de la etapa

1. `zb25vq64_dump.bin` verificado (3× SHA256) + notas del procedimiento.
2. `docs/stage4/` capturas: ráfagas SPI por evento + fotos de scope (SD, +IN).
3. Tabla evento→clip (§5) completa + clips extraídos en WAV.
4. `components/audio/` + harness de consola, con paridad OEM documentada.
5. Actualización de `PROJECT_NOTES.md` (§5 Stage 4, §6 si aplica) y commit.

---

## 8. Referencias cruzadas

- `PROJECT_NOTES.md` §2.1 (inventario), §4 D1–D9, §5 Stage 4, §6.2 (dump).
- `HANDOFF_Mifare_NZ3801.md` §4 (bus compartido, gating del sniffer, NRSTPD).
- Datasheet: `CS8302M DataSheet.pdf` (ChipSourceTek, MSOP-8L).
- Sniffer: https://github.com/jjsch-dev/spi_sniffer_pio
- flashrom: https://flashrom.org (programador `ch341a_spi`, 2 MHz fijo;
  chip forzado `W25Q64BV/W25Q64CV/W25Q64FV` para lectura del ZB25VQ64C —
  en flashrom ≥1.5 la antigua entrada `W25Q64.V` fue renombrada).
