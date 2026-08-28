#pragma once

// Frame-time driven quality control.  This module is intentionally independent
// of the renderer backend: it consumes a measured GPU frame time and returns a
// small, serialisable QualityState for the renderer to apply next frame.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace Halcyon::Renderer::Quality {

enum class RayTracingMode : std::uint8_t {
    Off = 0,
    HalfResolution = 1,
    FullResolution = 2,
    Disabled = Off,
    Half = HalfResolution,
    Full = FullResolution,
};
using RayTracingQuality = RayTracingMode;

// Levels are ordered from lowest quality (0) to highest quality.  Keeping the
// order explicit lets the controller move exactly one notch at a time.
enum class QualityKnob : std::uint8_t {
    InternalResolution = 0,
    ShadowResolution = 1,
    ShadowUpdatePeriod = 2,
    GeometryLod = 3,
    RayTracing = 4,
    Count = 5,
    Resolution = InternalResolution,
    Shadow = ShadowResolution,
    LOD = GeometryLod,
    RT = RayTracing,
};

enum class AdjustmentDirection : std::uint8_t {
    Downgrade,
    Upgrade,
};

[[nodiscard]] constexpr const char* toString(QualityKnob knob) noexcept {
    switch (knob) {
    case QualityKnob::InternalResolution: return "internal resolution";
    case QualityKnob::ShadowResolution: return "shadow resolution";
    case QualityKnob::ShadowUpdatePeriod: return "shadow update period";
    case QualityKnob::GeometryLod: return "geometry LOD";
    case QualityKnob::RayTracing: return "ray tracing";
    case QualityKnob::Count: break;
    }
    return "unknown quality knob";
}

[[nodiscard]] constexpr const char* toString(AdjustmentDirection direction) noexcept {
    return direction == AdjustmentDirection::Downgrade ? "downgrade" : "upgrade";
}

[[nodiscard]] constexpr const char* toString(RayTracingMode mode) noexcept {
    switch (mode) {
    case RayTracingMode::Off: return "off";
    case RayTracingMode::HalfResolution: return "half resolution";
    case RayTracingMode::FullResolution: return "full resolution";
    }
    return "unknown";
}

// Canonical quality ladders.  They are public so a UI can present the exact
// values used by the controller rather than rounded labels.
inline constexpr std::array<float, 5> kInternalResolutionScales{
    0.60f, 0.70f, 0.80f, 0.90f, 1.00f};
inline constexpr std::array<float, 3> kShadowResolutionScales{
    0.50f, 0.75f, 1.00f};
inline constexpr std::array<std::uint32_t, 3> kShadowUpdatePeriods{
    4u, 2u, 1u};
inline constexpr std::array<float, 4> kGeometryLodBiases{
    2.00f, 1.00f, 0.50f, 0.00f};
inline constexpr std::array<RayTracingMode, 3> kRayTracingModes{
    RayTracingMode::Off, RayTracingMode::HalfResolution, RayTracingMode::FullResolution};

struct QualityState {
    // Public level fields make the state cheap to copy into a FramePacket and
    // straightforward to serialise.  Use the accessors below when a physical
    // value is needed.
    std::uint8_t internalResolutionLevel = 4;
    std::uint8_t shadowResolutionLevel = 2;
    std::uint8_t shadowUpdateLevel = 2;
    std::uint8_t geometryLodLevel = 3;
    std::uint8_t rayTracingLevel = 0; // RT is conservatively disabled by default.

    [[nodiscard]] float internalResolutionScale() const noexcept {
        return kInternalResolutionScales[internalResolutionLevel < kInternalResolutionScales.size()
                                             ? internalResolutionLevel
                                             : kInternalResolutionScales.size() - 1u];
    }
    [[nodiscard]] float resolutionScale() const noexcept { return internalResolutionScale(); }
    [[nodiscard]] float renderScale() const noexcept { return internalResolutionScale(); }
    [[nodiscard]] float shadowResolutionScale() const noexcept {
        return kShadowResolutionScales[shadowResolutionLevel < kShadowResolutionScales.size()
                                           ? shadowResolutionLevel
                                           : kShadowResolutionScales.size() - 1u];
    }
    [[nodiscard]] float shadowScale() const noexcept { return shadowResolutionScale(); }
    [[nodiscard]] std::uint32_t shadowUpdatePeriod() const noexcept {
        return kShadowUpdatePeriods[shadowUpdateLevel < kShadowUpdatePeriods.size()
                                        ? shadowUpdateLevel
                                        : kShadowUpdatePeriods.size() - 1u];
    }
    [[nodiscard]] float geometryLodBias() const noexcept {
        return kGeometryLodBiases[geometryLodLevel < kGeometryLodBiases.size()
                                      ? geometryLodLevel
                                      : kGeometryLodBiases.size() - 1u];
    }
    [[nodiscard]] float lod() const noexcept { return geometryLodBias(); }
    [[nodiscard]] float lodBias() const noexcept { return geometryLodBias(); }
    [[nodiscard]] RayTracingMode rayTracing() const noexcept {
        return kRayTracingModes[rayTracingLevel < kRayTracingModes.size()
                                    ? rayTracingLevel
                                    : kRayTracingModes.size() - 1u];
    }
    [[nodiscard]] RayTracingMode rayTracingMode() const noexcept { return rayTracing(); }
    [[nodiscard]] RayTracingMode rtMode() const noexcept { return rayTracing(); }

    [[nodiscard]] static constexpr QualityState highestQuality() noexcept {
        return QualityState{static_cast<std::uint8_t>(kInternalResolutionScales.size() - 1u),
                             static_cast<std::uint8_t>(kShadowResolutionScales.size() - 1u),
                             static_cast<std::uint8_t>(kShadowUpdatePeriods.size() - 1u),
                             static_cast<std::uint8_t>(kGeometryLodBiases.size() - 1u),
                             static_cast<std::uint8_t>(kRayTracingModes.size() - 1u)};
    }
    [[nodiscard]] static constexpr QualityState conservative() noexcept {
        return QualityState{};
    }
    [[nodiscard]] static constexpr QualityState lowestQuality() noexcept {
        return QualityState{0, 0, 0, 0, 0};
    }

    friend constexpr bool operator==(const QualityState&, const QualityState&) noexcept = default;
};

// Cost and quality scores are absolute values per level.  Costs are in GPU
// milliseconds; scores are arbitrary monotonic units.  The controller uses
// differences between adjacent levels, so a project can replace this model
// with measurements from its own scenes.
struct QualityCostModel {
    std::array<double, 5> internalResolutionGpuMs{5.0, 6.2, 7.7, 9.5, 12.0};
    std::array<double, 5> internalResolutionQuality{0.55, 0.68, 0.81, 0.92, 1.00};

    std::array<double, 3> shadowResolutionGpuMs{0.35, 0.58, 0.90};
    std::array<double, 3> shadowResolutionQuality{0.60, 0.82, 1.00};

    std::array<double, 3> shadowUpdateGpuMs{0.20, 0.36, 0.58};
    std::array<double, 3> shadowUpdateQuality{0.66, 0.84, 1.00};

    std::array<double, 4> geometryLodGpuMs{0.42, 0.60, 0.80, 1.00};
    std::array<double, 4> geometryLodQuality{0.56, 0.73, 0.88, 1.00};

    std::array<double, 3> rayTracingGpuMs{0.0, 1.60, 3.20};
    std::array<double, 3> rayTracingQuality{0.64, 0.84, 1.00};
};

struct FrameBudgetConfig {
    double targetFrameTimeMs = 16.667;
    double gpuBudgetFraction = 0.90; // 90% of target is reserved for GPU work.
    double upgradeHeadroomFraction = 0.80;
    std::uint32_t downgradeAfterFrames = 8;
    std::uint32_t upgradeAfterFrames = 120;
    std::uint32_t adjustmentCooldownFrames = 60;
    std::size_t maxDecisionLogEntries = 256;
    QualityState initialQuality{};
    QualityCostModel costModel{};
};

struct QualityDecision {
    std::uint64_t decisionId = 0;
    std::uint64_t frameIndex = 0;
    AdjustmentDirection direction = AdjustmentDirection::Downgrade;
    QualityKnob knob = QualityKnob::InternalResolution;
    std::uint8_t fromLevel = 0;
    std::uint8_t toLevel = 0;
    QualityState before{};
    QualityState after{};
    double measuredGpuMs = 0.0;
    double budgetMs = 0.0;
    // For a downgrade this is the estimated saving; for an upgrade it is the
    // estimated extra GPU cost.  The corresponding quality delta is positive.
    double expectedGpuSavingsMs = 0.0;
    double expectedGpuCostMs = 0.0;
    double qualityLoss = 0.0;
    double qualityGain = 0.0;
    double score = 0.0;
    // Filled on the first sample after the adjustment.  It is signed in the
    // natural direction (positive means the expected improvement occurred).
    double actualGpuDeltaMs = 0.0;
    bool actualMeasured = false;
    std::string reason;
};

struct FrameBudgetUpdate {
    std::uint64_t frameIndex = 0;
    double measuredGpuMs = 0.0;
    double gpuBudgetMs = 0.0;
    bool adjusted = false;
    QualityState quality{};
    std::uint32_t overBudgetStreak = 0;
    std::uint32_t underBudgetStreak = 0;
    std::optional<QualityDecision> decision;
};

class FrameBudgetController {
public:
    FrameBudgetController();
    explicit FrameBudgetController(FrameBudgetConfig config);

    void reset();
    void reset(QualityState state);
    void setConfig(FrameBudgetConfig config);
    [[nodiscard]] const FrameBudgetConfig& config() const noexcept { return config_; }

    [[nodiscard]] double gpuBudgetMs() const noexcept { return gpuBudgetMs_; }
    [[nodiscard]] const QualityState& quality() const noexcept { return quality_; }
    [[nodiscard]] std::uint8_t currentLevel(QualityKnob knob) const noexcept;
    [[nodiscard]] bool canDowngrade(QualityKnob knob) const noexcept;
    [[nodiscard]] bool canUpgrade(QualityKnob knob) const noexcept;
    [[nodiscard]] bool isCoolingDown(QualityKnob knob) const noexcept;
    [[nodiscard]] std::uint64_t frameIndex() const noexcept { return frameIndex_; }
    [[nodiscard]] std::uint32_t overBudgetStreak() const noexcept { return overBudgetStreak_; }
    [[nodiscard]] std::uint32_t underBudgetStreak() const noexcept { return underBudgetStreak_; }

    // update(gpuMs) advances an internal frame counter.  The overload with an
    // explicit frame index is useful when a capture/replay supplies its own
    // frame numbering.
    [[nodiscard]] FrameBudgetUpdate update(double gpuFrameMs);
    [[nodiscard]] FrameBudgetUpdate update(std::uint64_t frameIndex, double gpuFrameMs);
    [[nodiscard]] FrameBudgetUpdate observe(double gpuFrameMs) { return update(gpuFrameMs); }

    void setQuality(QualityState state);
    void setCostModel(QualityCostModel model);
    [[nodiscard]] const QualityCostModel& costModel() const noexcept { return config_.costModel; }

    [[nodiscard]] const std::vector<QualityDecision>& decisionLog() const noexcept {
        return decisionLog_;
    }
    [[nodiscard]] const std::vector<QualityDecision>& decisions() const noexcept {
        return decisionLog_;
    }
    void clearDecisionLog() noexcept { decisionLog_.clear(); }

private:
    struct Candidate {
        QualityKnob knob = QualityKnob::InternalResolution;
        std::uint8_t from = 0;
        std::uint8_t to = 0;
        double savings = 0.0;
        double cost = 0.0;
        double qualityDelta = 0.0;
        double score = -std::numeric_limits<double>::infinity();
    };

    [[nodiscard]] FrameBudgetUpdate updateInternal(std::uint64_t frameIndex,
                                                   double gpuFrameMs);
    [[nodiscard]] std::optional<Candidate> chooseCandidate(AdjustmentDirection direction) const;
    [[nodiscard]] std::uint8_t level(QualityKnob knob) const noexcept;
    void setLevel(QualityKnob knob, std::uint8_t value) noexcept;
    [[nodiscard]] std::uint8_t maxLevel(QualityKnob knob) const noexcept;
    [[nodiscard]] bool coolingDown(QualityKnob knob) const noexcept;
    [[nodiscard]] double gpuCost(QualityKnob knob, std::uint8_t level) const noexcept;
    [[nodiscard]] double qualityScore(QualityKnob knob, std::uint8_t level) const noexcept;
    [[nodiscard]] const char* knobName(QualityKnob knob) const noexcept;
    void normalizeConfig();
    void normalizeQuality() noexcept;
    void recordActualMeasurement(std::uint64_t frameIndex, double gpuFrameMs);

    FrameBudgetConfig config_{};
    QualityState quality_{};
    double gpuBudgetMs_ = 0.0;
    std::uint64_t frameIndex_ = 0;
    bool hasFrame_ = false;
    std::uint32_t overBudgetStreak_ = 0;
    std::uint32_t underBudgetStreak_ = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(QualityKnob::Count)> lastAdjustmentFrame_{};
    std::array<bool, static_cast<std::size_t>(QualityKnob::Count)> hasAdjustment_{};
    std::vector<QualityDecision> decisionLog_;
    std::uint64_t nextDecisionId_ = 1;

    bool pendingMeasurement_ = false;
    std::uint64_t pendingDecisionId_ = 0;
    std::uint64_t pendingFrame_ = 0;
    double pendingBaselineGpuMs_ = 0.0;
    AdjustmentDirection pendingDirection_ = AdjustmentDirection::Downgrade;
};

using QualityController = FrameBudgetController;
using QualitySettings = QualityState;

} // namespace Halcyon::Renderer::Quality
