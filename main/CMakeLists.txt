# SPDX-FileCopyrightText: 2026 Keir Finlow-Bates
#
# SPDX-License-Identifier: MIT

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES driver fatfs wear_levelling esp_partition vfs led_strip
)
