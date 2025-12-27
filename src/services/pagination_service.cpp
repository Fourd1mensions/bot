#include "services/pagination_service.h"

namespace services {

std::optional<NavigationAction> PaginationService::parse_button_action(const std::string& button_id) {
    // Handle both prefixed (lb_prev, rs_next) and unprefixed (first, prev) formats
    if (button_id.ends_with("_first") || button_id == "first") {
        return NavigationAction::First;
    }
    if (button_id.ends_with("_prev") || button_id == "prev") {
        return NavigationAction::Previous;
    }
    if (button_id.ends_with("_next") || button_id == "next") {
        return NavigationAction::Next;
    }
    if (button_id.ends_with("_last") || button_id == "last") {
        return NavigationAction::Last;
    }
    return std::nullopt;
}

bool PaginationService::navigate(IPaginable& state, NavigationAction action, size_t jump_target) {
    size_t current = state.get_current_position();
    size_t total = state.get_total_items();

    if (total == 0) {
        return false;
    }

    size_t new_pos = current;

    switch (action) {
        case NavigationAction::First:
            new_pos = 0;
            break;

        case NavigationAction::Previous:
            if (current > 0) {
                new_pos = current - 1;
            }
            break;

        case NavigationAction::Next:
            if (current < total - 1) {
                new_pos = current + 1;
            }
            break;

        case NavigationAction::Last:
            new_pos = total - 1;
            break;

        case NavigationAction::JumpTo:
            // jump_target is 1-indexed from user input
            if (jump_target >= 1 && jump_target <= total) {
                new_pos = jump_target - 1;
            }
            break;
    }

    if (new_pos != current) {
        state.set_current_position(new_pos);
        return true;
    }

    return false;
}

bool PaginationService::navigate_by_button(IPaginable& state, const std::string& button_id) {
    auto action = parse_button_action(button_id);
    if (!action) {
        return false;
    }
    return navigate(state, *action);
}

} // namespace services
