#ifdef USE_ROSU_PP_SERVICE

#include "services/rosu_pp_client.h"
#include "debug_settings.h"

#include <grpcpp/grpcpp.h>
#include <pp.grpc.pb.h>

#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <fstream>
#include <algorithm>
#include <cctype>

namespace services {

// Mod bitflags (matches osu! API)
namespace mods {
    constexpr uint32_t NF = 1;
    constexpr uint32_t EZ = 2;
    constexpr uint32_t TD = 4;
    constexpr uint32_t HD = 8;
    constexpr uint32_t HR = 16;
    constexpr uint32_t SD = 32;
    constexpr uint32_t DT = 64;
    constexpr uint32_t RX = 128;
    constexpr uint32_t HT = 256;
    constexpr uint32_t NC = 576;  // DT + NC flag
    constexpr uint32_t FL = 1024;
    constexpr uint32_t AT = 2048;
    constexpr uint32_t SO = 4096;
    constexpr uint32_t AP = 8192;
    constexpr uint32_t PF = 16416; // SD + PF flag
}

class RosuPpClient::Impl {
public:
    explicit Impl(const std::string& address) {
        auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
        stub_ = pp::v1::PerformanceService::NewStub(channel);
        address_ = address;
        spdlog::info("[RosuPpClient] Created client for {}", address);
    }

    bool is_connected() const {
        return stub_ != nullptr;
    }

    std::optional<RosuDifficultyAttrs> calculate_difficulty(
        const pp::v1::BeatmapSource& beatmap,
        std::optional<RosuGameMode> mode,
        const RosuDifficultySettings& settings
    ) {
        auto start = std::chrono::steady_clock::now();

        spdlog::info("[RosuPp] CalculateDifficulty: mode={}, mods={}, size={}b",
            mode ? static_cast<int>(*mode) : 0, settings.mods, beatmap.content().size());

        pp::v1::DifficultyRequest request;
        *request.mutable_beatmap() = beatmap;

        if (mode) {
            request.set_mode(static_cast<pp::v1::GameMode>(*mode));
        }

        apply_difficulty_settings(request.mutable_settings(), settings);

        pp::v1::DifficultyResponse response;
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(30));

        auto status = stub_->CalculateDifficulty(&context, request, &response);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();

        if (!status.ok()) {
            spdlog::error("[RosuPp] CalculateDifficulty FAILED in {}ms: code={} msg={}",
                elapsed, static_cast<int>(status.error_code()), status.error_message());
            return std::nullopt;
        }

        auto result = parse_difficulty_response(response);
        spdlog::info("[RosuPp] CalculateDifficulty OK {}ms: ★{:.2f} combo={}",
            elapsed, result.stars, result.max_combo);

        return result;
    }

    std::optional<RosuPerformanceAttrs> calculate_performance(
        const pp::v1::BeatmapSource& beatmap,
        std::optional<RosuGameMode> mode,
        const RosuDifficultySettings& settings,
        const RosuScoreParams& score
    ) {
        auto start = std::chrono::steady_clock::now();

        spdlog::info("[RosuPp] CalculatePerformance: mode={} mods={} acc={:.2f}% combo={} miss={} size={}b",
            mode ? static_cast<int>(*mode) : 0,
            settings.mods,
            score.accuracy.value_or(100.0),
            score.combo.value_or(0),
            score.misses.value_or(0),
            beatmap.content().size());

        pp::v1::PerformanceRequest request;
        *request.mutable_beatmap() = beatmap;

        if (mode) {
            request.set_mode(static_cast<pp::v1::GameMode>(*mode));
        }

        apply_difficulty_settings(request.mutable_difficulty_settings(), settings);
        apply_score_params(request.mutable_score(), score);

        pp::v1::PerformanceResponse response;
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(30));

        auto status = stub_->CalculatePerformance(&context, request, &response);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();

        if (!status.ok()) {
            spdlog::error("[RosuPp] CalculatePerformance FAILED in {}ms: code={} msg={}",
                elapsed, static_cast<int>(status.error_code()), status.error_message());
            return std::nullopt;
        }

        auto result = parse_performance_response(response);
        if (result) {
            spdlog::info("[RosuPp] CalculatePerformance OK in {}ms: pp={:.2f} stars={:.2f} aim={:.1f} speed={:.1f} acc={:.1f}",
                elapsed, result->pp, result->stars, result->pp_aim, result->pp_speed, result->pp_acc);

            // Verbose logging with full response details
            if (debug::Settings::instance().verbose_rosu_pp.load()) {
                spdlog::info("[RosuPp-DEBUG] <<< Full response: pp={:.4f} stars={:.4f} max_combo={} "
                    "pp_aim={:.4f} pp_speed={:.4f} pp_acc={:.4f} pp_flashlight={:.4f} "
                    "eff_miss={:.2f} ar={:.2f} od={:.2f} cs={:.2f} hp={:.2f}",
                    result->pp, result->stars, result->max_combo,
                    result->pp_aim, result->pp_speed, result->pp_acc, result->pp_flashlight,
                    result->effective_miss_count,
                    result->difficulty.ar, result->difficulty.od, result->difficulty.cs, result->difficulty.hp);
            }
        }

        return result;
    }

    std::vector<RosuPerformanceAttrs> calculate_batch(
        const pp::v1::BeatmapSource& beatmap,
        std::optional<RosuGameMode> mode,
        const RosuDifficultySettings& settings,
        const std::vector<RosuScoreParams>& scores
    ) {
        auto start = std::chrono::steady_clock::now();

        spdlog::info("[RosuPp] CalculateBatch: mode={} mods={} batch={} size={}b",
            mode ? static_cast<int>(*mode) : 0, settings.mods, scores.size(), beatmap.content().size());

        pp::v1::BatchRequest request;

        for (const auto& score : scores) {
            auto* req = request.add_requests();
            *req->mutable_beatmap() = beatmap;

            if (mode) {
                req->set_mode(static_cast<pp::v1::GameMode>(*mode));
            }

            apply_difficulty_settings(req->mutable_difficulty_settings(), settings);
            apply_score_params(req->mutable_score(), score);
        }

        pp::v1::BatchResponse response;
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(60));

        auto status = stub_->CalculateBatch(&context, request, &response);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();

        if (!status.ok()) {
            spdlog::error("[RosuPp] CalculateBatch FAILED in {}ms: code={} msg={}",
                elapsed, static_cast<int>(status.error_code()), status.error_message());
            return {};
        }

        std::vector<RosuPerformanceAttrs> results;
        results.reserve(response.responses_size());

        for (const auto& resp : response.responses()) {
            if (auto attrs = parse_performance_response(resp)) {
                results.push_back(*attrs);
            } else {
                results.push_back({});  // Empty result for failed calculation
            }
        }

        spdlog::info("[RosuPp] CalculateBatch OK in {}ms: {} results, stars={:.2f}",
            elapsed, results.size(), results.empty() ? 0.0 : results[0].stars);

        // Verbose logging with all PP values
        if (debug::Settings::instance().verbose_rosu_pp.load() && !results.empty()) {
            std::string pp_list;
            for (size_t i = 0; i < results.size(); ++i) {
                if (i > 0) pp_list += ", ";
                pp_list += fmt::format("{:.2f}", results[i].pp);
            }
            spdlog::info("[RosuPp-DEBUG] <<< Batch PP values: [{}]", pp_list);
        }

        return results;
    }

private:
    void apply_difficulty_settings(pp::v1::DifficultySettings* proto, const RosuDifficultySettings& settings) {
        if (settings.mods != 0) {
            proto->set_mods(settings.mods);
        }
        if (settings.clock_rate) {
            proto->set_clock_rate(*settings.clock_rate);
        }
        if (settings.ar) {
            proto->set_ar(*settings.ar);
        }
        if (settings.cs) {
            proto->set_cs(*settings.cs);
        }
        if (settings.od) {
            proto->set_od(*settings.od);
        }
        if (settings.hp) {
            proto->set_hp(*settings.hp);
        }
        if (settings.lazer) {
            proto->set_lazer(true);
        }
    }

    void apply_score_params(pp::v1::ScoreParams* proto, const RosuScoreParams& score) {
        if (score.combo) {
            proto->set_combo(*score.combo);
        }
        if (score.accuracy) {
            proto->set_accuracy(*score.accuracy);
        }
        if (score.misses) {
            proto->set_misses(*score.misses);
        }
        if (score.n300) {
            proto->set_n300(*score.n300);
        }
        if (score.n100) {
            proto->set_n100(*score.n100);
        }
        if (score.n50) {
            proto->set_n50(*score.n50);
        }
        if (score.n_geki) {
            proto->set_n_geki(*score.n_geki);
        }
        if (score.n_katu) {
            proto->set_n_katu(*score.n_katu);
        }
        // Use fastest hit result priority for batch calculations
        proto->set_hitresult_priority(pp::v1::HIT_RESULT_PRIORITY_FASTEST);
    }

    RosuDifficultyAttrs parse_difficulty_response(const pp::v1::DifficultyResponse& resp) {
        RosuDifficultyAttrs attrs;
        attrs.stars = resp.stars();
        attrs.max_combo = resp.max_combo();

        if (resp.has_osu()) {
            const auto& osu = resp.osu();
            attrs.aim = osu.aim();
            attrs.speed = osu.speed();
            attrs.flashlight = osu.flashlight();
            attrs.ar = osu.ar();
            attrs.od = osu.od();
            attrs.hp = osu.hp();
            attrs.cs = osu.cs();
            attrs.n_circles = osu.n_circles();
            attrs.n_sliders = osu.n_sliders();
            attrs.n_spinners = osu.n_spinners();
        }

        return attrs;
    }

    std::optional<RosuPerformanceAttrs> parse_performance_response(const pp::v1::PerformanceResponse& resp) {
        RosuPerformanceAttrs attrs;
        attrs.pp = resp.pp();
        attrs.stars = resp.stars();
        attrs.max_combo = resp.max_combo();

        if (resp.has_osu()) {
            const auto& osu = resp.osu();
            attrs.pp_aim = osu.pp_aim();
            attrs.pp_speed = osu.pp_speed();
            attrs.pp_acc = osu.pp_acc();
            attrs.pp_flashlight = osu.pp_flashlight();
            attrs.effective_miss_count = osu.effective_miss_count();

            if (osu.has_difficulty()) {
                attrs.difficulty = parse_difficulty_response_from_osu(osu.difficulty());
            }
        }

        return attrs;
    }

    RosuDifficultyAttrs parse_difficulty_response_from_osu(const pp::v1::OsuDifficultyAttributes& osu) {
        RosuDifficultyAttrs attrs;
        attrs.aim = osu.aim();
        attrs.speed = osu.speed();
        attrs.flashlight = osu.flashlight();
        attrs.ar = osu.ar();
        attrs.od = osu.od();
        attrs.hp = osu.hp();
        attrs.cs = osu.cs();
        attrs.n_circles = osu.n_circles();
        attrs.n_sliders = osu.n_sliders();
        attrs.n_spinners = osu.n_spinners();
        return attrs;
    }

    std::unique_ptr<pp::v1::PerformanceService::Stub> stub_;
    std::string address_;
};

// Public interface implementation

RosuPpClient::RosuPpClient(const std::string& address)
    : impl_(std::make_unique<Impl>(address)) {}

RosuPpClient::~RosuPpClient() = default;

RosuPpClient::RosuPpClient(RosuPpClient&&) noexcept = default;
RosuPpClient& RosuPpClient::operator=(RosuPpClient&&) noexcept = default;

bool RosuPpClient::is_connected() const {
    return impl_ && impl_->is_connected();
}

std::optional<RosuDifficultyAttrs> RosuPpClient::calculate_difficulty(
    const std::string& osu_file_path,
    std::optional<RosuGameMode> mode,
    const RosuDifficultySettings& settings
) {
    spdlog::info("[RosuPp] Loading: {}", osu_file_path);

    std::ifstream file(osu_file_path, std::ios::binary);
    if (!file) {
        spdlog::error("[RosuPp] Failed to open .osu file: {}", osu_file_path);
        return std::nullopt;
    }
    std::vector<uint8_t> content((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());

    spdlog::info("[RosuPp] Loaded {}b", content.size());
    return calculate_difficulty_bytes(content, mode, settings);
}

std::optional<RosuDifficultyAttrs> RosuPpClient::calculate_difficulty_bytes(
    const std::vector<uint8_t>& content,
    std::optional<RosuGameMode> mode,
    const RosuDifficultySettings& settings
) {
    pp::v1::BeatmapSource beatmap;
    beatmap.set_content(content.data(), content.size());
    return impl_->calculate_difficulty(beatmap, mode, settings);
}

std::optional<RosuPerformanceAttrs> RosuPpClient::calculate_performance(
    const std::string& osu_file_path,
    std::optional<RosuGameMode> mode,
    const RosuDifficultySettings& settings,
    const RosuScoreParams& score
) {
    spdlog::info("[RosuPp] Loading for perf: {}", osu_file_path);

    std::ifstream file(osu_file_path, std::ios::binary);
    if (!file) {
        spdlog::error("[RosuPp] Failed to open .osu file: {}", osu_file_path);
        return std::nullopt;
    }
    std::vector<uint8_t> content((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());

    spdlog::info("[RosuPp] Loaded {}b -> gRPC", content.size());
    return calculate_performance_bytes(content, mode, settings, score);
}

std::optional<RosuPerformanceAttrs> RosuPpClient::calculate_performance_bytes(
    const std::vector<uint8_t>& content,
    std::optional<RosuGameMode> mode,
    const RosuDifficultySettings& settings,
    const RosuScoreParams& score
) {
    pp::v1::BeatmapSource beatmap;
    beatmap.set_content(content.data(), content.size());
    return impl_->calculate_performance(beatmap, mode, settings, score);
}

std::vector<RosuPerformanceAttrs> RosuPpClient::calculate_batch(
    const std::string& osu_file_path,
    std::optional<RosuGameMode> mode,
    const RosuDifficultySettings& settings,
    const std::vector<RosuScoreParams>& scores
) {
    spdlog::info("[RosuPp] Loading for batch ({}): {}", scores.size(), osu_file_path);

    std::ifstream file(osu_file_path, std::ios::binary);
    if (!file) {
        spdlog::error("[RosuPp] Failed to open .osu file: {}", osu_file_path);
        return {};
    }
    std::vector<uint8_t> content((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());

    spdlog::info("[RosuPp] Loaded {}b for batch", content.size());

    pp::v1::BeatmapSource beatmap;
    beatmap.set_content(content.data(), content.size());
    return impl_->calculate_batch(beatmap, mode, settings, scores);
}

uint32_t RosuPpClient::parse_mods(const std::string& mods_str) {
    uint32_t result = 0;
    std::string mods = mods_str;

    // Convert to uppercase
    std::transform(mods.begin(), mods.end(), mods.begin(),
        [](unsigned char c) { return std::toupper(c); });

    // Parse 2-character mod codes
    for (size_t i = 0; i + 1 < mods.length(); i += 2) {
        std::string mod = mods.substr(i, 2);

        if (mod == "NF") result |= mods::NF;
        else if (mod == "EZ") result |= mods::EZ;
        else if (mod == "TD") result |= mods::TD;
        else if (mod == "HD") result |= mods::HD;
        else if (mod == "HR") result |= mods::HR;
        else if (mod == "SD") result |= mods::SD;
        else if (mod == "DT") result |= mods::DT;
        else if (mod == "RX") result |= mods::RX;
        else if (mod == "HT") result |= mods::HT;
        else if (mod == "NC") result |= mods::NC;
        else if (mod == "FL") result |= mods::FL;
        else if (mod == "AT") result |= mods::AT;
        else if (mod == "SO") result |= mods::SO;
        else if (mod == "AP") result |= mods::AP;
        else if (mod == "PF") result |= mods::PF;
        else if (mod == "NM") { /* NoMod - do nothing */ }
    }

    return result;
}

std::optional<RosuGameMode> RosuPpClient::parse_mode(const std::string& mode) {
    std::string m = mode;
    std::transform(m.begin(), m.end(), m.begin(),
        [](unsigned char c) { return std::tolower(c); });

    if (m == "osu" || m == "std" || m == "standard" || m == "0") {
        return RosuGameMode::Osu;
    }
    if (m == "taiko" || m == "1") {
        return RosuGameMode::Taiko;
    }
    if (m == "catch" || m == "fruits" || m == "ctb" || m == "2") {
        return RosuGameMode::Catch;
    }
    if (m == "mania" || m == "3") {
        return RosuGameMode::Mania;
    }

    return std::nullopt;
}

} // namespace services

#endif // USE_ROSU_PP_SERVICE
