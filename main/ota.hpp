#pragma once
#include "esp_partition.h"
#include <functional>
#include <string>

namespace app {
struct FirmwareUpdate {
    const esp_partition_t* partition = nullptr;
    std::string image_id;
};
// Caller holds knife::Update. Only returns a candidate after signature validation.
bool firmware_receive(size_t size, const std::function<int(uint8_t*, size_t)>& read, FirmwareUpdate& update,
                      std::string& error);
void firmware_validation_start();
const char* firmware_state();
std::string firmware_image_id();
} // namespace app
