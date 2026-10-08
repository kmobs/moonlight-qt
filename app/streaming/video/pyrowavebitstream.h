#pragma once

#include <Limelight.h>
#include "pyrowaveframing.h"
#include <cstring>
#include <new>
#include <vector>

// RTP fragments may split PyroWave blocks, so parse the complete decode unit.
inline bool assemblePyroWaveDecodeUnit(PDECODE_UNIT du, std::vector<uint32_t>& words,
                                       std::vector<PyroWaveFraming::Segment>* segments = nullptr)
{
    constexpr int MaxFrameBytes = 64 * 1024 * 1024;
    if (!du || du->fullLength < 8 || du->fullLength > MaxFrameBytes ||
            du->fullLength % sizeof(uint32_t) != 0) {
        return false;
    }

    size_t length = 0;
    for (auto entry = du->bufferList; entry; entry = entry->next) {
        if (!entry->data || entry->length <= 0 ||
                size_t(entry->length) > size_t(du->fullLength) - length) {
            return false;
        }
        length += entry->length;
    }
    if (length != size_t(du->fullLength)) {
        return false;
    }
    try {
        words.resize(length / sizeof(uint32_t));
        if (segments) {
            segments->clear();
            size_t offset = 0;
            for (auto entry = du->bufferList; entry; entry = entry->next) {
                segments->push_back({offset, size_t(entry->length), entry->bufferType == BUFFER_TYPE_LOST,
                                     entry->bufferType == BUFFER_TYPE_RECORD_START});
                offset += entry->length;
            }
        }
    }
    catch (const std::bad_alloc&) {
        return false;
    }
    auto output = reinterpret_cast<uint8_t*>(words.data());
    for (auto entry = du->bufferList; entry; entry = entry->next) {
        std::memcpy(output, entry->data, entry->length);
        output += entry->length;
    }
    return true;
}
