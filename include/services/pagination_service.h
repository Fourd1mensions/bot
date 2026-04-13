#pragma once

#include <optional>
#include <string>
#include "state/ipaginable.h"

namespace services {

/**
 * Navigation action types for pagination.
 */
enum class NavigationAction {
    First,
    Previous,
    Next,
    Last,
    JumpTo
};

/**
 * Service for handling pagination logic.
 * Works with any type implementing IPaginable interface.
 */
class PaginationService {
public:
    /**
     * Parse a button ID into a navigation action.
     * @param button_id The button custom_id (e.g., "lb_prev", "rs_next")
     * @return The navigation action, or nullopt if not recognized
     */
    static std::optional<NavigationAction> parse_button_action(const std::string& button_id);

    /**
     * Calculate the new position based on navigation action.
     * @param state The paginable state object
     * @param action The navigation action to perform
     * @param jump_target Target position for JumpTo action (1-indexed)
     * @return true if position was changed, false otherwise
     */
    static bool navigate(IPaginable& state, NavigationAction action, size_t jump_target = 0);

    /**
     * Convenience method: parse button and navigate in one call.
     * @param state The paginable state object
     * @param button_id The button custom_id
     * @return true if position was changed, false otherwise
     */
    static bool navigate_by_button(IPaginable& state, const std::string& button_id);
};

} // namespace services
