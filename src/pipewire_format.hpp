#pragma once

#include <cstdint>

#include <spa/param/audio/format-utils.h>

#include "audio_format.hpp"

namespace wfa {

// Builds the EnumFormat param for audio:: format into `buffer`.
inline const spa_pod* build_audio_format(std::uint8_t* buffer, std::uint32_t size)
{
    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_S16_LE;
    info.rate = audio::rate;
    info.channels = audio::channels;
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;

    spa_pod_builder builder{};
    spa_pod_builder_init(&builder, buffer, size);
    return spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info);
}

} // namespace wfa
