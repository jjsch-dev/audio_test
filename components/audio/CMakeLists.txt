# IDF 6.x: los drivers son componentes individuales (el paraguas "driver"
# ya no los expone). En IDF <= 5.3 usar REQUIRES driver en su lugar.
idf_component_register(
    SRCS "src/audio_decoder.c"
         "src/audio_driver.c"
         "src/audio_player.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_mcpwm esp_driver_gptimer esp_driver_gpio
    PRIV_REQUIRES freertos log
)
