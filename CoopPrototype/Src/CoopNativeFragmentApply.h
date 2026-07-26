#pragma once

#include "CoopNativeFragmentMaterializer.h"
#include "CoopNativeFragmentPayload.h"
#include "CoopNativeGameStateFragmentLocator.h"

#include <cstdint>
#include <string>

namespace CoopNativeFragmentApply
{
struct ApplyResult
{
    bool attempted = false;
    bool ok = false;
    bool enabled = false;
    bool noop = false;
    std::string reason;
    uint32_t targetNodes = 0;
    uint32_t nodeWrites = 0;
    uint32_t attrTokenWrites = 0;
    uint32_t attrValueWrites = 0;
    uint32_t attrPreservedNameTokens = 0;
    uint32_t attrUnsupported = 0;
    uint32_t childIndexWrites = 0;
    uint32_t childIndexPreserved = 0;
    uint32_t fullTransplantOps = 0;
    uint32_t allocationOps = 0;
    uint32_t entityAllocationOps = 0;
    uint32_t missingSubtreeInsertOps = 0;
    uint32_t newItemInsertOps = 0;
    uint32_t remapOps = 0;
    uint32_t transcodeOps = 0;
    uint32_t guards = 0;
};

ApplyResult TryApplySameShapeNoop(
    const CoopNativeFragmentPayload::ParsedPayload& source,
    const CoopNativeGameStateFragmentLocator::NativeInventoryFragmentBundle& target,
    const CoopNativeFragmentMaterializer::MaterializerPlan& materializerPlan);

ApplyResult TryApplySameShapeNumericAttributes(
    const CoopNativeFragmentPayload::ParsedPayload& source,
    const CoopNativeGameStateFragmentLocator::NativeInventoryFragmentBundle& target,
    const CoopNativeFragmentMaterializer::MaterializerPlan& materializerPlan);

ApplyResult TryApplyFullTransplantPlan(
    const CoopNativeFragmentPayload::ParsedPayload& source,
    const CoopNativeGameStateFragmentLocator::NativeInventoryFragmentBundle& target,
    const CoopNativeFragmentMaterializer::MaterializerPlan& materializerPlan);

std::string BuildApplyStatus(const ApplyResult& result);
}
