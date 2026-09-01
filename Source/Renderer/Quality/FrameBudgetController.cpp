#include "FrameBudgetController.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace Halcyon::Renderer::Quality
{

namespace
{

template <typename T, std::size_t N>
void sanitizeArray(std::array<T, N>& values, T fallback) noexcept
{
    for (auto& value : values)
    {
        if (!std::isfinite(static_cast<double>(value)) || value < T{0})
        {
            value = fallback;
        }
    }
}

[[nodiscard]] constexpr std::size_t knobIndex(QualityKnob knob) noexcept
{
    return static_cast<std::size_t>(knob);
}

} // namespace

FrameBudgetController::FrameBudgetController()
        : FrameBudgetController(FrameBudgetConfig{})
{
}

FrameBudgetController::FrameBudgetController(FrameBudgetConfig config)
        : config_(std::move(config))
{
    normalizeConfig();
    gpuBudgetMs_ = config_.targetFrameTimeMs * config_.gpuBudgetFraction;
    reset(config_.initialQuality);
}

void FrameBudgetController::normalizeConfig()
{
    if (!std::isfinite(config_.targetFrameTimeMs) || config_.targetFrameTimeMs <= 0.0)
    {
        config_.targetFrameTimeMs = 16.667;
    }
    if (!std::isfinite(config_.gpuBudgetFraction) || config_.gpuBudgetFraction <= 0.0 ||
        config_.gpuBudgetFraction > 1.0)
    {
        config_.gpuBudgetFraction = 0.90;
    }
    if (!std::isfinite(config_.upgradeHeadroomFraction) || config_.upgradeHeadroomFraction <= 0.0 ||
        config_.upgradeHeadroomFraction >= 1.0)
    {
        config_.upgradeHeadroomFraction = 0.80;
    }
    config_.downgradeAfterFrames = std::max<std::uint32_t>(1u, config_.downgradeAfterFrames);
    config_.upgradeAfterFrames = std::max<std::uint32_t>(1u, config_.upgradeAfterFrames);
    // A zero cooldown is useful in deterministic unit tests and is therefore
    // accepted; the plan's production default remains 60 frames.
    if (config_.maxDecisionLogEntries == 0)
    {
        config_.maxDecisionLogEntries = 1;
    }

    auto& model = config_.costModel;
    sanitizeArray(model.internalResolutionGpuMs, 0.0);
    sanitizeArray(model.internalResolutionQuality, 0.0);
    sanitizeArray(model.shadowResolutionGpuMs, 0.0);
    sanitizeArray(model.shadowResolutionQuality, 0.0);
    sanitizeArray(model.shadowUpdateGpuMs, 0.0);
    sanitizeArray(model.shadowUpdateQuality, 0.0);
    sanitizeArray(model.geometryLodGpuMs, 0.0);
    sanitizeArray(model.geometryLodQuality, 0.0);
    sanitizeArray(model.rayTracingGpuMs, 0.0);
    sanitizeArray(model.rayTracingQuality, 0.0);
}

void FrameBudgetController::normalizeQuality() noexcept
{
    quality_.internalResolutionLevel = static_cast<std::uint8_t>(std::min<std::size_t>(
        quality_.internalResolutionLevel, kInternalResolutionScales.size() - 1u));
    quality_.shadowResolutionLevel = static_cast<std::uint8_t>(
        std::min<std::size_t>(quality_.shadowResolutionLevel, kShadowResolutionScales.size() - 1u));
    quality_.shadowUpdateLevel = static_cast<std::uint8_t>(
        std::min<std::size_t>(quality_.shadowUpdateLevel, kShadowUpdatePeriods.size() - 1u));
    quality_.geometryLodLevel = static_cast<std::uint8_t>(
        std::min<std::size_t>(quality_.geometryLodLevel, kGeometryLodBiases.size() - 1u));
    quality_.rayTracingLevel = static_cast<std::uint8_t>(
        std::min<std::size_t>(quality_.rayTracingLevel, kRayTracingModes.size() - 1u));
}

void FrameBudgetController::reset()
{
    reset(config_.initialQuality);
}

void FrameBudgetController::reset(QualityState state)
{
    quality_ = state;
    normalizeQuality();
    gpuBudgetMs_ = config_.targetFrameTimeMs * config_.gpuBudgetFraction;
    frameIndex_ = 0;
    hasFrame_ = false;
    overBudgetStreak_ = 0;
    underBudgetStreak_ = 0;
    hasAdjustment_.fill(false);
    lastAdjustmentFrame_.fill(0);
    decisionLog_.clear();
    nextDecisionId_ = 1;
    pendingMeasurement_ = false;
    pendingDecisionId_ = 0;
    pendingFrame_ = 0;
    pendingBaselineGpuMs_ = 0.0;
}

void FrameBudgetController::setConfig(FrameBudgetConfig config)
{
    const auto preservedQuality = quality_;
    config_ = std::move(config);
    normalizeConfig();
    quality_ = preservedQuality;
    normalizeQuality();
    gpuBudgetMs_ = config_.targetFrameTimeMs * config_.gpuBudgetFraction;
    overBudgetStreak_ = underBudgetStreak_ = 0;
    hasAdjustment_.fill(false);
    pendingMeasurement_ = false;
    if (decisionLog_.size() > config_.maxDecisionLogEntries)
    {
        const auto first =
            decisionLog_.end() - static_cast<std::ptrdiff_t>(config_.maxDecisionLogEntries);
        decisionLog_.erase(decisionLog_.begin(), first);
    }
}

void FrameBudgetController::setQuality(QualityState state)
{
    quality_ = state;
    normalizeQuality();
    overBudgetStreak_ = underBudgetStreak_ = 0;
    hasAdjustment_.fill(false);
    pendingMeasurement_ = false;
}

std::uint8_t FrameBudgetController::currentLevel(QualityKnob knob) const noexcept
{
    return level(knob);
}

bool FrameBudgetController::canDowngrade(QualityKnob knob) const noexcept
{
    return level(knob) > 0;
}

bool FrameBudgetController::canUpgrade(QualityKnob knob) const noexcept
{
    return level(knob) < maxLevel(knob);
}

bool FrameBudgetController::isCoolingDown(QualityKnob knob) const noexcept
{
    return coolingDown(knob);
}

void FrameBudgetController::setCostModel(QualityCostModel model)
{
    config_.costModel = std::move(model);
    normalizeConfig();
}

FrameBudgetUpdate FrameBudgetController::update(double gpuFrameMs)
{
    const auto nextFrame = hasFrame_ ? frameIndex_ + 1u : 0u;
    return updateInternal(nextFrame, gpuFrameMs);
}

FrameBudgetUpdate FrameBudgetController::update(std::uint64_t frameIndex, double gpuFrameMs)
{
    return updateInternal(frameIndex, gpuFrameMs);
}

void FrameBudgetController::recordActualMeasurement(std::uint64_t frameIndex, double gpuFrameMs)
{
    if (!pendingMeasurement_ || frameIndex <= pendingFrame_)
    {
        return;
    }
    const double delta = pendingDirection_ == AdjustmentDirection::Downgrade
                             ? pendingBaselineGpuMs_ - gpuFrameMs
                             : gpuFrameMs - pendingBaselineGpuMs_;
    for (auto& decision : decisionLog_)
    {
        if (decision.decisionId == pendingDecisionId_)
        {
            decision.actualGpuDeltaMs = delta;
            decision.actualMeasured = true;
            break;
        }
    }
    pendingMeasurement_ = false;
}

FrameBudgetUpdate FrameBudgetController::updateInternal(std::uint64_t frameIndex, double gpuFrameMs)
{
    frameIndex_ = frameIndex;
    hasFrame_ = true;

    FrameBudgetUpdate update;
    update.frameIndex = frameIndex_;
    update.measuredGpuMs = gpuFrameMs;
    update.gpuBudgetMs = gpuBudgetMs_;
    update.quality = quality_;
    update.overBudgetStreak = overBudgetStreak_;
    update.underBudgetStreak = underBudgetStreak_;

    // A missing timestamp must not cause an arbitrary quality change.  Keep
    // the last state and expose the invalid sample to the caller through the
    // measured value.
    if (!std::isfinite(gpuFrameMs) || gpuFrameMs < 0.0)
    {
        return update;
    }
    recordActualMeasurement(frameIndex, gpuFrameMs);

    if (gpuFrameMs > gpuBudgetMs_)
    {
        overBudgetStreak_ =
            std::min<std::uint32_t>(overBudgetStreak_ + 1u, config_.downgradeAfterFrames);
        underBudgetStreak_ = 0;
    }
    else if (gpuFrameMs < gpuBudgetMs_ * config_.upgradeHeadroomFraction)
    {
        underBudgetStreak_ =
            std::min<std::uint32_t>(underBudgetStreak_ + 1u, config_.upgradeAfterFrames);
        overBudgetStreak_ = 0;
    }
    else
    {
        overBudgetStreak_ = 0;
        underBudgetStreak_ = 0;
    }

    std::optional<Candidate> candidate;
    AdjustmentDirection direction = AdjustmentDirection::Downgrade;
    if (overBudgetStreak_ >= config_.downgradeAfterFrames)
    {
        direction = AdjustmentDirection::Downgrade;
        candidate = chooseCandidate(direction);
    }
    else if (underBudgetStreak_ >= config_.upgradeAfterFrames)
    {
        direction = AdjustmentDirection::Upgrade;
        candidate = chooseCandidate(direction);
    }

    if (!candidate.has_value())
    {
        update.quality = quality_;
        update.overBudgetStreak = overBudgetStreak_;
        update.underBudgetStreak = underBudgetStreak_;
        return update;
    }

    const Candidate selected = *candidate;
    const QualityState before = quality_;
    setLevel(selected.knob, selected.to);
    const QualityState after = quality_;
    lastAdjustmentFrame_[knobIndex(selected.knob)] = frameIndex_;
    hasAdjustment_[knobIndex(selected.knob)] = true;

    QualityDecision decision;
    decision.decisionId = nextDecisionId_++;
    decision.frameIndex = frameIndex_;
    decision.direction = direction;
    decision.knob = selected.knob;
    decision.fromLevel = selected.from;
    decision.toLevel = selected.to;
    decision.before = before;
    decision.after = after;
    decision.measuredGpuMs = gpuFrameMs;
    decision.budgetMs = gpuBudgetMs_;
    decision.expectedGpuSavingsMs =
        direction == AdjustmentDirection::Downgrade ? selected.savings : 0.0;
    decision.expectedGpuCostMs = direction == AdjustmentDirection::Upgrade ? selected.cost : 0.0;
    decision.qualityLoss =
        direction == AdjustmentDirection::Downgrade ? selected.qualityDelta : 0.0;
    decision.qualityGain = direction == AdjustmentDirection::Upgrade ? selected.qualityDelta : 0.0;
    decision.score = selected.score;

    std::ostringstream reason;
    if (direction == AdjustmentDirection::Downgrade)
    {
        reason << toString(direction) << " " << toString(selected.knob) << ": GPU frame time "
               << gpuFrameMs << " ms exceeded " << gpuBudgetMs_ << " ms for "
               << config_.downgradeAfterFrames << " consecutive frames";
    }
    else
    {
        reason << toString(direction) << " " << toString(selected.knob) << ": GPU frame time "
               << gpuFrameMs << " ms stayed below "
               << (gpuBudgetMs_ * config_.upgradeHeadroomFraction) << " ms for "
               << config_.upgradeAfterFrames << " consecutive frames";
    }
    reason << " (level " << static_cast<unsigned>(selected.from) << " -> "
           << static_cast<unsigned>(selected.to) << ", score " << selected.score << ')';
    decision.reason = reason.str();

    decisionLog_.push_back(decision);
    if (decisionLog_.size() > config_.maxDecisionLogEntries)
    {
        decisionLog_.erase(decisionLog_.begin());
    }
    pendingMeasurement_ = true;
    pendingDecisionId_ = decision.decisionId;
    pendingFrame_ = frameIndex_;
    pendingBaselineGpuMs_ = gpuFrameMs;
    pendingDirection_ = direction;

    // Require a fresh run of samples after every adjustment.  This hysteresis
    // avoids repeatedly changing quality while a timestamp is settling.
    overBudgetStreak_ = 0;
    underBudgetStreak_ = 0;

    update.adjusted = true;
    update.quality = quality_;
    update.overBudgetStreak = overBudgetStreak_;
    update.underBudgetStreak = underBudgetStreak_;
    update.decision = decision;
    return update;
}

std::optional<FrameBudgetController::Candidate> FrameBudgetController::chooseCandidate(
    AdjustmentDirection direction) const
{
    std::optional<Candidate> best;
    for (std::uint8_t raw = 0; raw < static_cast<std::uint8_t>(QualityKnob::Count); ++raw)
    {
        const auto knob = static_cast<QualityKnob>(raw);
        if (coolingDown(knob))
        {
            continue;
        }
        const auto from = level(knob);
        const auto maximum = maxLevel(knob);
        if (direction == AdjustmentDirection::Downgrade && from == 0)
        {
            continue;
        }
        if (direction == AdjustmentDirection::Upgrade && from >= maximum)
        {
            continue;
        }
        const auto to = direction == AdjustmentDirection::Downgrade
                            ? static_cast<std::uint8_t>(from - 1u)
                            : static_cast<std::uint8_t>(from + 1u);
        const double fromCost = gpuCost(knob, from);
        const double toCost = gpuCost(knob, to);
        const double fromQuality = qualityScore(knob, from);
        const double toQuality = qualityScore(knob, to);
        Candidate candidate;
        candidate.knob = knob;
        candidate.from = from;
        candidate.to = to;
        if (direction == AdjustmentDirection::Downgrade)
        {
            candidate.savings = std::max(0.0, fromCost - toCost);
            candidate.qualityDelta = std::max(0.0, fromQuality - toQuality);
            candidate.score = candidate.savings / std::max(candidate.qualityDelta,
                                                      std::numeric_limits<double>::epsilon());
        }
        else
        {
            candidate.cost = std::max(0.0, toCost - fromCost);
            candidate.qualityDelta = std::max(0.0, toQuality - fromQuality);
            candidate.score = candidate.qualityDelta /
                              std::max(candidate.cost, std::numeric_limits<double>::epsilon());
        }

        // A zero-cost transition is still a valid fallback when a project has
        // not supplied profiling data yet; its score is simply zero.
        if (direction == AdjustmentDirection::Upgrade && candidate.qualityDelta <= 0.0)
        {
            continue;
        }
        if (!best.has_value() || candidate.score > best->score + 1e-12)
        {
            best = candidate;
        }
    }
    return best;
}

std::uint8_t FrameBudgetController::level(QualityKnob knob) const noexcept
{
    switch (knob)
    {
        case QualityKnob::InternalResolution:
            return quality_.internalResolutionLevel;
        case QualityKnob::ShadowResolution:
            return quality_.shadowResolutionLevel;
        case QualityKnob::ShadowUpdatePeriod:
            return quality_.shadowUpdateLevel;
        case QualityKnob::GeometryLod:
            return quality_.geometryLodLevel;
        case QualityKnob::RayTracing:
            return quality_.rayTracingLevel;
        case QualityKnob::Count:
            break;
    }
    return 0;
}

void FrameBudgetController::setLevel(QualityKnob knob, std::uint8_t value) noexcept
{
    value = std::min(value, maxLevel(knob));
    switch (knob)
    {
        case QualityKnob::InternalResolution:
            quality_.internalResolutionLevel = value;
            break;
        case QualityKnob::ShadowResolution:
            quality_.shadowResolutionLevel = value;
            break;
        case QualityKnob::ShadowUpdatePeriod:
            quality_.shadowUpdateLevel = value;
            break;
        case QualityKnob::GeometryLod:
            quality_.geometryLodLevel = value;
            break;
        case QualityKnob::RayTracing:
            quality_.rayTracingLevel = value;
            break;
        case QualityKnob::Count:
            break;
    }
}

std::uint8_t FrameBudgetController::maxLevel(QualityKnob knob) const noexcept
{
    switch (knob)
    {
        case QualityKnob::InternalResolution:
            return static_cast<std::uint8_t>(kInternalResolutionScales.size() - 1u);
        case QualityKnob::ShadowResolution:
            return static_cast<std::uint8_t>(kShadowResolutionScales.size() - 1u);
        case QualityKnob::ShadowUpdatePeriod:
            return static_cast<std::uint8_t>(kShadowUpdatePeriods.size() - 1u);
        case QualityKnob::GeometryLod:
            return static_cast<std::uint8_t>(kGeometryLodBiases.size() - 1u);
        case QualityKnob::RayTracing:
            return static_cast<std::uint8_t>(kRayTracingModes.size() - 1u);
        case QualityKnob::Count:
            break;
    }
    return 0;
}

bool FrameBudgetController::coolingDown(QualityKnob knob) const noexcept
{
    const auto i = knobIndex(knob);
    if (!hasAdjustment_[i] || config_.adjustmentCooldownFrames == 0)
    {
        return false;
    }
    if (frameIndex_ < lastAdjustmentFrame_[i])
    {
        return false;
    }
    return frameIndex_ - lastAdjustmentFrame_[i] < config_.adjustmentCooldownFrames;
}

double FrameBudgetController::gpuCost(QualityKnob knob, std::uint8_t value) const noexcept
{
    const auto& model = config_.costModel;
    switch (knob)
    {
        case QualityKnob::InternalResolution:
            return model.internalResolutionGpuMs[std::min<std::size_t>(
                value, model.internalResolutionGpuMs.size() - 1u)];
        case QualityKnob::ShadowResolution:
            return model.shadowResolutionGpuMs[std::min<std::size_t>(
                value, model.shadowResolutionGpuMs.size() - 1u)];
        case QualityKnob::ShadowUpdatePeriod:
            return model.shadowUpdateGpuMs[std::min<std::size_t>(
                value, model.shadowUpdateGpuMs.size() - 1u)];
        case QualityKnob::GeometryLod:
            return model
                .geometryLodGpuMs[std::min<std::size_t>(value, model.geometryLodGpuMs.size() - 1u)];
        case QualityKnob::RayTracing:
            return model
                .rayTracingGpuMs[std::min<std::size_t>(value, model.rayTracingGpuMs.size() - 1u)];
        case QualityKnob::Count:
            break;
    }
    return 0.0;
}

double FrameBudgetController::qualityScore(QualityKnob knob, std::uint8_t value) const noexcept
{
    const auto& model = config_.costModel;
    switch (knob)
    {
        case QualityKnob::InternalResolution:
            return model.internalResolutionQuality[std::min<std::size_t>(
                value, model.internalResolutionQuality.size() - 1u)];
        case QualityKnob::ShadowResolution:
            return model.shadowResolutionQuality[std::min<std::size_t>(
                value, model.shadowResolutionQuality.size() - 1u)];
        case QualityKnob::ShadowUpdatePeriod:
            return model.shadowUpdateQuality[std::min<std::size_t>(
                value, model.shadowUpdateQuality.size() - 1u)];
        case QualityKnob::GeometryLod:
            return model.geometryLodQuality[std::min<std::size_t>(
                value, model.geometryLodQuality.size() - 1u)];
        case QualityKnob::RayTracing:
            return model.rayTracingQuality[std::min<std::size_t>(
                value, model.rayTracingQuality.size() - 1u)];
        case QualityKnob::Count:
            break;
    }
    return 0.0;
}

const char* FrameBudgetController::knobName(QualityKnob knob) const noexcept
{
    return toString(knob);
}

} // namespace Halcyon::Renderer::Quality
