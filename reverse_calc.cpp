#include <iostream>
#include <cmath>
#include <iomanip>

// Formula from official code
double calculate_star_rating(double base_performance) {
    if (base_performance <= 0.00001) return 0.0;
    constexpr double PERFORMANCE_BASE_MULTIPLIER = 1.14;
    constexpr double STAR_RATING_MULTIPLIER = 0.0265;
    constexpr double divisor = 100000.0 / std::pow(2.0, 1.0 / 1.1);
    return std::cbrt(PERFORMANCE_BASE_MULTIPLIER) * STAR_RATING_MULTIPLIER *
           (std::cbrt(divisor * base_performance) + 4.0);
}

// Reverse: from star rating to base performance
double reverse_star_rating(double star_rating) {
    constexpr double PERFORMANCE_BASE_MULTIPLIER = 1.14;
    constexpr double STAR_RATING_MULTIPLIER = 0.0265;
    constexpr double divisor = 100000.0 / std::pow(2.0, 1.0 / 1.1);

    double term = star_rating / (std::cbrt(PERFORMANCE_BASE_MULTIPLIER) * STAR_RATING_MULTIPLIER);
    double inner = term - 4.0;
    return std::pow(inner, 3.0) / divisor;
}

double difficulty_to_performance(double rating) {
    return std::pow(5.0 * std::max(1.0, rating / 0.0675) - 4.0, 3.0) / 100000.0;
}

double performance_to_difficulty(double performance) {
    double term = std::cbrt(performance * 100000.0) + 4.0;
    return 0.0675 * term / 5.0;
}

int main() {
    std::cout << std::fixed << std::setprecision(6);

    // Current values
    double aim_rating = 2.86343;
    double speed_rating = 3.22607;
    double current_star = 6.269361;
    double target_star = 6.80;

    // Calculate current combined performance
    double current_aim_perf = difficulty_to_performance(aim_rating);
    double current_speed_perf = difficulty_to_performance(speed_rating);
    double current_combined = std::pow(
        std::pow(current_aim_perf, 1.1) + std::pow(current_speed_perf, 1.1),
        1.0 / 1.1
    );

    std::cout << "=== Current Values ===" << std::endl;
    std::cout << "Aim rating: " << aim_rating << std::endl;
    std::cout << "Speed rating: " << speed_rating << std::endl;
    std::cout << "Aim performance: " << current_aim_perf << std::endl;
    std::cout << "Speed performance: " << current_speed_perf << std::endl;
    std::cout << "Combined performance: " << current_combined << std::endl;
    std::cout << "Star rating: " << current_star << std::endl;
    std::cout << "Star rating (calculated): " << calculate_star_rating(current_combined) << std::endl;
    std::cout << std::endl;

    // Target values
    double target_combined = reverse_star_rating(target_star);
    std::cout << "=== Target Values ===" << std::endl;
    std::cout << "Target star rating: " << target_star << std::endl;
    std::cout << "Target combined performance: " << target_combined << std::endl;
    std::cout << "Verify: " << calculate_star_rating(target_combined) << std::endl;
    std::cout << std::endl;

    // Calculate multiplier needed
    double multiplier = target_combined / current_combined;
    std::cout << "=== Analysis ===" << std::endl;
    std::cout << "Multiplier needed: " << multiplier << std::endl;
    std::cout << "Percentage increase: " << ((multiplier - 1.0) * 100.0) << "%" << std::endl;

    return 0;
}
