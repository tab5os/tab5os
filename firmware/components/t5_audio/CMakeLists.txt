# Audio (plan 7.1-7.3): ES8388 out, ES7210 in, mixer, echo cancellation.
# dsp.c is pure C (host-tested); audio_hw.c is the only file that touches
# esp_codec_dev and I2S.
include(${CMAKE_CURRENT_LIST_DIR}/portable.cmake)
idf_component_register(
    SRCS ${T5_AUDIO_PORTABLE_SRCS} "audio.c" "audio_hw.c" "decode.c" "media.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES t5_core
    PRIV_REQUIRES t5_i2c t5_bsp t5_pins t5_power t5_storage esp_driver_i2s esp_timer heap espressif__esp_codec_dev
)
