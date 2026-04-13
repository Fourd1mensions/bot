#pragma once

#include <cstddef>

/**
 * Interface for paginated state objects.
 * Allows unified handling of navigation across different state types.
 */
class IPaginable {
public:
    virtual ~IPaginable() = default;

    virtual size_t get_current_position() const = 0;
    virtual void set_current_position(size_t pos) = 0;
    virtual size_t get_total_items() const = 0;
};
