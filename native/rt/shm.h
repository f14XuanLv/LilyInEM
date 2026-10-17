#pragma once

#include "core/protocol.h"

namespace lie::shm {

// Opens (or creates and initialises) both named mappings. Safe to call repeatedly.
bool open();
void close();
Control* control() noexcept;
World* world() noexcept;

} // namespace lie::shm
