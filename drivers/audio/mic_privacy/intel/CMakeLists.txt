# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_INTEL_ADSP_MIC_PRIVACY mic_privacy.c)
