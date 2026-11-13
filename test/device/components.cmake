# The firmware components the device tests build against: read by the
# project (CMakeLists.txt) and by main/CMakeLists.txt.
set(T5_DEVICE_TEST_COMPONENTS
    t5_core t5_pins t5_i2c t5_bsp t5_time t5_imu t5_storage t5_display t5_input t5_power t5_battery t5_bootfmt t5_ota t5_net t5_hid)
