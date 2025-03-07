/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include "mongo/platform/basic.h"

#include "mongo/db/query/sbe_stage_builder_helpers.h"

#include <iterator>
#include <numeric>

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/branch.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/hash_agg.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/stages/traverse.h"
#include "mongo/db/exec/sbe/stages/union.h"
#include "mongo/db/exec/sbe/stages/unwind.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/matcher/matcher_type_set.h"
#include "mongo/db/query/sbe_stage_builder.h"
#include "mongo/db/storage/execution_context.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo::stage_builder {

std::unique_ptr<sbe::EExpression> makeUnaryOp(sbe::EPrimUnary::Op unaryOp,
                                              std::unique_ptr<sbe::EExpression> operand) {
    return sbe::makeE<sbe::EPrimUnary>(unaryOp, std::move(operand));
}

std::unique_ptr<sbe::EExpression> makeNot(std::unique_ptr<sbe::EExpression> e) {
    return makeUnaryOp(sbe::EPrimUnary::logicNot, std::move(e));
}

std::unique_ptr<sbe::EExpression> makeBinaryOp(sbe::EPrimBinary::Op binaryOp,
                                               std::unique_ptr<sbe::EExpression> lhs,
                                               std::unique_ptr<sbe::EExpression> rhs,
                                               std::unique_ptr<sbe::EExpression> collator) {
    using namespace std::literals;

    if (collator && sbe::EPrimBinary::isComparisonOp(binaryOp)) {
        return sbe::makeE<sbe::EPrimBinary>(
            binaryOp, std::move(lhs), std::move(rhs), std::move(collator));
    } else {
        return sbe::makeE<sbe::EPrimBinary>(binaryOp, std::move(lhs), std::move(rhs));
    }
}

std::unique_ptr<sbe::EExpression> makeBinaryOp(sbe::EPrimBinary::Op binaryOp,
                                               std::unique_ptr<sbe::EExpression> lhs,
                                               std::unique_ptr<sbe::EExpression> rhs,
                                               sbe::RuntimeEnvironment* env) {
    invariant(env);

    auto collatorSlot = env->getSlotIfExists("collator"_sd);
    auto collatorVar = collatorSlot ? sbe::makeE<sbe::EVariable>(*collatorSlot) : nullptr;

    return makeBinaryOp(binaryOp, std::move(lhs), std::move(rhs), std::move(collatorVar));
}

std::unique_ptr<sbe::EExpression> makeIsMember(std::unique_ptr<sbe::EExpression> input,
                                               std::unique_ptr<sbe::EExpression> arr,
                                               std::unique_ptr<sbe::EExpression> collator) {
    if (collator) {
        return makeFunction("collIsMember", std::move(collator), std::move(input), std::move(arr));
    } else {
        return makeFunction("isMember", std::move(input), std::move(arr));
    }
}

std::unique_ptr<sbe::EExpression> makeIsMember(std::unique_ptr<sbe::EExpression> input,
                                               std::unique_ptr<sbe::EExpression> arr,
                                               sbe::RuntimeEnvironment* env) {
    invariant(env);

    auto collatorSlot = env->getSlotIfExists("collator"_sd);
    auto collatorVar = collatorSlot ? sbe::makeE<sbe::EVariable>(*collatorSlot) : nullptr;

    return makeIsMember(std::move(input), std::move(arr), std::move(collatorVar));
}
std::unique_ptr<sbe::EExpression> generateNullOrMissingExpr(const sbe::EExpression& expr) {
    return makeBinaryOp(sbe::EPrimBinary::fillEmpty,
                        makeFunction("typeMatch",
                                     expr.clone(),
                                     makeConstant(sbe::value::TypeTags::NumberInt64,
                                                  sbe::value::bitcastFrom<int64_t>(
                                                      getBSONTypeMask(BSONType::jstNULL) |
                                                      getBSONTypeMask(BSONType::Undefined)))),
                        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean,
                                                   sbe::value::bitcastFrom<bool>(true)));
}

std::unique_ptr<sbe::EExpression> generateNullOrMissing(const sbe::EVariable& var) {
    return generateNullOrMissingExpr(var);
}

std::unique_ptr<sbe::EExpression> generateNullOrMissing(const sbe::FrameId frameId,
                                                        const sbe::value::SlotId slotId) {
    sbe::EVariable var{frameId, slotId};
    return generateNullOrMissing(var);
}

std::unique_ptr<sbe::EExpression> generateNullOrMissing(std::unique_ptr<sbe::EExpression> arg) {
    return generateNullOrMissingExpr(*arg);
}

std::unique_ptr<sbe::EExpression> generateNonNumericCheck(const sbe::EVariable& var) {
    return makeNot(makeFunction("isNumber", var.clone()));
}

std::unique_ptr<sbe::EExpression> generateLongLongMinCheck(const sbe::EVariable& var) {
    return makeBinaryOp(
        sbe::EPrimBinary::logicAnd,
        makeFunction("typeMatch",
                     var.clone(),
                     makeConstant(sbe::value::TypeTags::NumberInt64,
                                  sbe::value::bitcastFrom<int64_t>(
                                      MatcherTypeSet{BSONType::NumberLong}.getBSONTypeMask()))),
        makeBinaryOp(sbe::EPrimBinary::eq,
                     var.clone(),
                     sbe::makeE<sbe::EConstant>(
                         sbe::value::TypeTags::NumberInt64,
                         sbe::value::bitcastFrom<int64_t>(std::numeric_limits<int64_t>::min()))));
}

std::unique_ptr<sbe::EExpression> generateNaNCheck(const sbe::EVariable& var) {
    return makeFunction("isNaN", var.clone());
}

std::unique_ptr<sbe::EExpression> generateInfinityCheck(const sbe::EVariable& var) {
    return makeFunction("isInfinity"_sd, var.clone());
}

std::unique_ptr<sbe::EExpression> generateNonPositiveCheck(const sbe::EVariable& var) {
    return makeBinaryOp(sbe::EPrimBinary::EPrimBinary::lessEq,
                        var.clone(),
                        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                   sbe::value::bitcastFrom<int32_t>(0)));
}

std::unique_ptr<sbe::EExpression> generatePositiveCheck(const sbe::EExpression& expr) {
    return makeBinaryOp(sbe::EPrimBinary::EPrimBinary::greater,
                        expr.clone(),
                        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                   sbe::value::bitcastFrom<int32_t>(0)));
}

std::unique_ptr<sbe::EExpression> generateNegativeCheck(const sbe::EVariable& var) {
    return makeBinaryOp(sbe::EPrimBinary::EPrimBinary::less,
                        var.clone(),
                        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                   sbe::value::bitcastFrom<int32_t>(0)));
}

std::unique_ptr<sbe::EExpression> generateNonObjectCheck(const sbe::EVariable& var) {
    return makeNot(makeFunction("isObject", var.clone()));
}

std::unique_ptr<sbe::EExpression> generateNonStringCheck(const sbe::EExpression& expr) {
    return makeNot(makeFunction("isString", expr.clone()));
}

std::unique_ptr<sbe::EExpression> generateNullishOrNotRepresentableInt32Check(
    const sbe::EVariable& var) {
    auto numericConvert32 =
        sbe::makeE<sbe::ENumericConvert>(var.clone(), sbe::value::TypeTags::NumberInt32);
    return makeBinaryOp(sbe::EPrimBinary::logicOr,
                        generateNullOrMissing(var),
                        makeNot(makeFunction("exists", std::move(numericConvert32))));
}

std::unique_ptr<sbe::EExpression> generateNonTimestampCheck(const sbe::EVariable& var) {
    return makeNot(makeFunction("isTimestamp", var.clone()));
}

template <>
std::unique_ptr<sbe::EExpression> buildMultiBranchConditional(
    std::unique_ptr<sbe::EExpression> defaultCase) {
    return defaultCase;
}

std::unique_ptr<sbe::EExpression> buildMultiBranchConditionalFromCaseValuePairs(
    std::vector<CaseValuePair> caseValuePairs, std::unique_ptr<sbe::EExpression> defaultValue) {
    return std::accumulate(
        std::make_reverse_iterator(std::make_move_iterator(caseValuePairs.end())),
        std::make_reverse_iterator(std::make_move_iterator(caseValuePairs.begin())),
        std::move(defaultValue),
        [](auto&& expression, auto&& caseValuePair) {
            return buildMultiBranchConditional(std::move(caseValuePair), std::move(expression));
        });
}

std::unique_ptr<sbe::PlanStage> makeLimitTree(std::unique_ptr<sbe::PlanStage> inputStage,
                                              PlanNodeId planNodeId,
                                              long long limit) {
    return sbe::makeS<sbe::LimitSkipStage>(std::move(inputStage), limit, boost::none, planNodeId);
}

std::unique_ptr<sbe::PlanStage> makeLimitCoScanTree(PlanNodeId planNodeId, long long limit) {
    return sbe::makeS<sbe::LimitSkipStage>(
        sbe::makeS<sbe::CoScanStage>(planNodeId), limit, boost::none, planNodeId);
}

std::unique_ptr<sbe::EExpression> makeFillEmptyFalse(std::unique_ptr<sbe::EExpression> e) {
    using namespace std::literals;
    return makeBinaryOp(sbe::EPrimBinary::fillEmpty,
                        std::move(e),
                        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean,
                                                   sbe::value::bitcastFrom<bool>(false)));
}

std::unique_ptr<sbe::EExpression> makeVariable(sbe::value::SlotId slotId) {
    return sbe::makeE<sbe::EVariable>(slotId);
}

std::unique_ptr<sbe::EExpression> makeVariable(sbe::FrameId frameId, sbe::value::SlotId slotId) {
    return sbe::makeE<sbe::EVariable>(frameId, slotId);
}

std::unique_ptr<sbe::EExpression> makeMoveVariable(sbe::FrameId frameId,
                                                   sbe::value::SlotId slotId) {
    return sbe::makeE<sbe::EVariable>(frameId, slotId, true);
}

std::unique_ptr<sbe::EExpression> makeFillEmptyNull(std::unique_ptr<sbe::EExpression> e) {
    using namespace std::literals;
    return makeBinaryOp(sbe::EPrimBinary::fillEmpty,
                        std::move(e),
                        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0));
}

std::unique_ptr<sbe::EExpression> makeFillEmptyUndefined(std::unique_ptr<sbe::EExpression> e) {
    using namespace std::literals;
    return makeBinaryOp(sbe::EPrimBinary::fillEmpty,
                        std::move(e),
                        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::bsonUndefined, 0));
}

std::unique_ptr<sbe::EExpression> makeNothingArrayCheck(
    std::unique_ptr<sbe::EExpression> isArrayInput, std::unique_ptr<sbe::EExpression> otherwise) {
    using namespace std::literals;
    return sbe::makeE<sbe::EIf>(makeFunction("isArray"_sd, std::move(isArrayInput)),
                                sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Nothing, 0),
                                std::move(otherwise));
}

std::unique_ptr<sbe::EExpression> generateShardKeyBinding(
    const sbe::MatchPath& keyPatternField,
    sbe::value::FrameIdGenerator& frameIdGenerator,
    std::unique_ptr<sbe::EExpression> inputExpr,
    int level) {
    invariant(level >= 0);

    auto makeGetFieldKeyPattern = [&](std::unique_ptr<sbe::EExpression> slot) {
        return makeFillEmptyNull(makeFunction(
            "getField"_sd, std::move(slot), sbe::makeE<sbe::EConstant>(keyPatternField[level])));
    };

    if (level == keyPatternField.numParts() - 1) {
        auto frameId = frameIdGenerator.generate();
        auto bindSlot = sbe::makeE<sbe::EVariable>(frameId, 0);
        return sbe::makeE<sbe::ELocalBind>(
            frameId,
            sbe::makeEs(makeGetFieldKeyPattern(std::move(inputExpr))),
            makeNothingArrayCheck(bindSlot->clone(), bindSlot->clone()));
    }

    auto frameId = frameIdGenerator.generate();
    auto nextSlot = sbe::makeE<sbe::EVariable>(frameId, 0);
    auto shardKeyBinding =
        generateShardKeyBinding(keyPatternField, frameIdGenerator, nextSlot->clone(), level + 1);

    return sbe::makeE<sbe::ELocalBind>(
        frameId,
        sbe::makeEs(makeGetFieldKeyPattern(inputExpr->clone())),
        makeNothingArrayCheck(nextSlot->clone(), std::move(shardKeyBinding)));
}

EvalStage makeLimitCoScanStage(PlanNodeId planNodeId, long long limit) {
    return {makeLimitCoScanTree(planNodeId, limit), sbe::makeSV()};
}

std::pair<sbe::value::SlotId, EvalStage> projectEvalExpr(
    EvalExpr expr,
    EvalStage stage,
    PlanNodeId planNodeId,
    sbe::value::SlotIdGenerator* slotIdGenerator) {
    // If expr's value is already in a slot, return the slot.
    if (expr.hasSlot()) {
        return {*expr.getSlot(), std::move(stage)};
    }

    // If expr's value is an expression, create a ProjectStage to evaluate the expression
    // into a slot.
    auto slot = slotIdGenerator->generate();
    stage = makeProject(std::move(stage), planNodeId, slot, expr.extractExpr());
    return {slot, std::move(stage)};
}

EvalStage makeProject(EvalStage stage,
                      sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> projects,
                      PlanNodeId planNodeId) {
    auto outSlots = stage.extractOutSlots();
    for (auto& [slot, _] : projects) {
        outSlots.push_back(slot);
    }

    return {sbe::makeS<sbe::ProjectStage>(
                stage.extractStage(planNodeId), std::move(projects), planNodeId),
            std::move(outSlots)};
}

EvalStage makeLoopJoin(EvalStage left,
                       EvalStage right,
                       PlanNodeId planNodeId,
                       const sbe::value::SlotVector& lexicalEnvironment) {
    // If 'left' and 'right' are both null, we just return null. If one of 'left'/'right' is null
    // and the other is non-null, return whichever one is non-null.
    if (left.stageIsNull()) {
        return right;
    } else if (right.stageIsNull()) {
        return left;
    }

    auto outerProjects = left.getOutSlots();
    auto outerCorrelated = left.getOutSlots();

    outerCorrelated.insert(
        outerCorrelated.end(), lexicalEnvironment.begin(), lexicalEnvironment.end());

    auto outSlots = left.extractOutSlots();
    outSlots.insert(outSlots.end(), right.getOutSlots().begin(), right.getOutSlots().end());

    return {sbe::makeS<sbe::LoopJoinStage>(left.extractStage(planNodeId),
                                           right.extractStage(planNodeId),
                                           std::move(outerProjects),
                                           std::move(outerCorrelated),
                                           nullptr,
                                           planNodeId),
            std::move(outSlots)};
}

EvalStage makeUnwind(EvalStage inputEvalStage,
                     sbe::value::SlotIdGenerator* slotIdGenerator,
                     PlanNodeId planNodeId,
                     bool preserveNullAndEmptyArrays) {
    auto unwindSlot = slotIdGenerator->generate();
    auto unwindStage = sbe::makeS<sbe::UnwindStage>(inputEvalStage.extractStage(planNodeId),
                                                    inputEvalStage.getOutSlots().front(),
                                                    unwindSlot,
                                                    slotIdGenerator->generate(),
                                                    preserveNullAndEmptyArrays,
                                                    planNodeId);
    return {std::move(unwindStage), sbe::makeSV(unwindSlot)};
}

EvalStage makeBranch(EvalStage thenStage,
                     EvalStage elseStage,
                     std::unique_ptr<sbe::EExpression> ifExpr,
                     sbe::value::SlotVector thenVals,
                     sbe::value::SlotVector elseVals,
                     sbe::value::SlotVector outputVals,
                     PlanNodeId planNodeId) {
    auto branchStage = sbe::makeS<sbe::BranchStage>(thenStage.extractStage(planNodeId),
                                                    elseStage.extractStage(planNodeId),
                                                    std::move(ifExpr),
                                                    std::move(thenVals),
                                                    std::move(elseVals),
                                                    outputVals,
                                                    planNodeId);
    return {std::move(branchStage), std::move(outputVals)};
}

EvalStage makeTraverse(EvalStage outer,
                       EvalStage inner,
                       sbe::value::SlotId inField,
                       sbe::value::SlotId outField,
                       sbe::value::SlotId outFieldInner,
                       std::unique_ptr<sbe::EExpression> foldExpr,
                       std::unique_ptr<sbe::EExpression> finalExpr,
                       PlanNodeId planNodeId,
                       boost::optional<size_t> nestedArraysDepth,
                       const sbe::value::SlotVector& lexicalEnvironment) {
    sbe::value::SlotVector outerCorrelated = lexicalEnvironment;
    for (auto slot : outer.getOutSlots()) {
        if (slot != inField) {
            outerCorrelated.push_back(slot);
        }
    }

    auto outSlots = outer.extractOutSlots();
    outSlots.push_back(outField);

    return {sbe::makeS<sbe::TraverseStage>(outer.extractStage(planNodeId),
                                           inner.extractStage(planNodeId),
                                           inField,
                                           outField,
                                           outFieldInner,
                                           std::move(outerCorrelated),
                                           std::move(foldExpr),
                                           std::move(finalExpr),
                                           planNodeId,
                                           nestedArraysDepth),
            std::move(outSlots)};
}

EvalStage makeLimitSkip(EvalStage input,
                        PlanNodeId planNodeId,
                        boost::optional<long long> limit,
                        boost::optional<long long> skip) {
    return EvalStage{
        sbe::makeS<sbe::LimitSkipStage>(input.extractStage(planNodeId), limit, skip, planNodeId),
        input.extractOutSlots()};
}

EvalStage makeUnion(std::vector<EvalStage> inputStages,
                    std::vector<sbe::value::SlotVector> inputVals,
                    sbe::value::SlotVector outputVals,
                    PlanNodeId planNodeId) {
    sbe::PlanStage::Vector branches;
    branches.reserve(inputStages.size());
    for (auto& inputStage : inputStages) {
        branches.emplace_back(inputStage.extractStage(planNodeId));
    }
    return EvalStage{sbe::makeS<sbe::UnionStage>(
                         std::move(branches), std::move(inputVals), outputVals, planNodeId),
                     outputVals};
}

EvalStage makeHashAgg(EvalStage stage,
                      sbe::value::SlotVector gbs,
                      sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> aggs,
                      boost::optional<sbe::value::SlotId> collatorSlot,
                      bool allowDiskUse,
                      PlanNodeId planNodeId) {
    stage.setOutSlots(gbs);
    for (auto& [slot, _] : aggs) {
        stage.addOutSlot(slot);
    }

    stage.setStage(sbe::makeS<sbe::HashAggStage>(stage.extractStage(planNodeId),
                                                 std::move(gbs),
                                                 std::move(aggs),
                                                 sbe::makeSV(),
                                                 true /* optimized close */,
                                                 collatorSlot,
                                                 allowDiskUse,
                                                 planNodeId));
    return stage;
}

EvalStage makeMkBsonObj(EvalStage stage,
                        sbe::value::SlotId objSlot,
                        boost::optional<sbe::value::SlotId> rootSlot,
                        boost::optional<sbe::MakeObjFieldBehavior> fieldBehavior,
                        std::vector<std::string> fields,
                        std::vector<std::string> projectFields,
                        sbe::value::SlotVector projectVars,
                        bool forceNewObject,
                        bool returnOldObject,
                        PlanNodeId planNodeId) {
    stage.setStage(sbe::makeS<sbe::MakeBsonObjStage>(stage.extractStage(planNodeId),
                                                     objSlot,
                                                     rootSlot,
                                                     fieldBehavior,
                                                     std::move(fields),
                                                     std::move(projectFields),
                                                     std::move(projectVars),
                                                     forceNewObject,
                                                     returnOldObject,
                                                     planNodeId));
    stage.addOutSlot(objSlot);

    return stage;
}

EvalExprStagePair generateUnion(std::vector<EvalExprStagePair> branches,
                                BranchFn branchFn,
                                PlanNodeId planNodeId,
                                sbe::value::SlotIdGenerator* slotIdGenerator) {
    sbe::PlanStage::Vector stages;
    std::vector<sbe::value::SlotVector> inputs;
    stages.reserve(branches.size());
    inputs.reserve(branches.size());

    for (size_t i = 0; i < branches.size(); i++) {
        auto [slot, stage] = [&]() {
            auto& [expr, stage] = branches[i];

            if (!branchFn || i + 1 == branches.size()) {
                return projectEvalExpr(
                    std::move(expr), std::move(stage), planNodeId, slotIdGenerator);
            }

            return branchFn(std::move(expr), std::move(stage), planNodeId, slotIdGenerator);
        }();

        stages.emplace_back(stage.extractStage(planNodeId));
        inputs.emplace_back(sbe::makeSV(slot));
    }

    auto outputSlot = slotIdGenerator->generate();
    auto unionStage = sbe::makeS<sbe::UnionStage>(
        std::move(stages), std::move(inputs), sbe::makeSV(outputSlot), planNodeId);
    EvalStage outputStage{std::move(unionStage), sbe::makeSV(outputSlot)};

    return {outputSlot, std::move(outputStage)};
}

EvalExprStagePair generateSingleResultUnion(std::vector<EvalExprStagePair> branches,
                                            BranchFn branchFn,
                                            PlanNodeId planNodeId,
                                            sbe::value::SlotIdGenerator* slotIdGenerator) {
    auto [unionEvalExpr, unionEvalStage] =
        generateUnion(std::move(branches), std::move(branchFn), planNodeId, slotIdGenerator);
    return {std::move(unionEvalExpr),
            EvalStage{makeLimitTree(unionEvalStage.extractStage(planNodeId), planNodeId),
                      unionEvalStage.extractOutSlots()}};
}

EvalExprStagePair generateShortCircuitingLogicalOp(sbe::EPrimBinary::Op logicOp,
                                                   std::vector<EvalExprStagePair> branches,
                                                   PlanNodeId planNodeId,
                                                   sbe::value::SlotIdGenerator* slotIdGenerator,
                                                   const FilterStateHelper& stateHelper) {
    invariant(logicOp == sbe::EPrimBinary::logicAnd || logicOp == sbe::EPrimBinary::logicOr);

    if (!branches.empty() && logicOp == sbe::EPrimBinary::logicOr) {
        // OR does not support index tracking, so we must ensure that state from the last branch
        // holds only boolean value.
        // NOTE: There is no technical reason for that. We could support index tracking for OR
        // expression, but this would differ from the existing behaviour.
        auto& [expr, _] = branches.back();
        expr = stateHelper.makeState(stateHelper.getBool(expr.extractExpr()));
    }

    // For AND and OR, if 'branches' only has one element, we can just return branches[0].
    if (branches.size() == 1) {
        return std::move(branches[0]);
    }

    bool exprOnlyBranches = true;
    for (const auto& [expr, stage] : branches) {
        if (!stage.stageIsNull()) {
            exprOnlyBranches = false;
            break;
        }
    }

    if (exprOnlyBranches) {
        std::unique_ptr<sbe::EExpression> exprOnlyOp;
        for (int32_t i = branches.size() - 1; i >= 0; i--) {
            auto& [expr, _] = branches[i];
            auto stateExpr = stateHelper.getBool(expr.extractExpr());
            if (exprOnlyOp) {
                exprOnlyOp = makeBinaryOp(logicOp, std::move(stateExpr), std::move(exprOnlyOp));
            } else {
                exprOnlyOp = std::move(stateExpr);
            }
        }
        return {EvalExpr{std::move(exprOnlyOp)}, EvalStage{}};
    }

    // Prepare to create limit-1/union with N branches (where N is the number of operands). Each
    // branch will be evaluated from left to right until one of the branches produces a value. The
    // first N-1 branches have a FilterStage to control whether they produce a value. If a branch's
    // filter condition is true, the branch will produce a value and the remaining branches will not
    // be evaluated. In other words, the evaluation process will "short-circuit". If a branch's
    // filter condition is false, the branch will not produce a value and the evaluation process
    // will continue. The last branch doesn't have a FilterStage and will always produce a value.
    auto branchFn = [logicOp, &stateHelper](EvalExpr expr,
                                            EvalStage stage,
                                            PlanNodeId planNodeId,
                                            sbe::value::SlotIdGenerator* slotIdGenerator) {
        // Create a FilterStage for each branch (except the last one). If a branch's filter
        // condition is true, it will "short-circuit" the evaluation process. For AND, short-
        // circuiting should happen if an operand evalautes to false. For OR, short-circuiting
        // should happen if an operand evaluates to true.
        // Set up an output value to be returned if short-circuiting occurs. For AND, when
        // short-circuiting occurs, the output returned should be false. For OR, when short-
        // circuiting occurs, the output returned should be true.
        auto filterExpr = stateHelper.getBool(expr.extractExpr());
        if (logicOp == sbe::EPrimBinary::logicAnd) {
            filterExpr = makeNot(std::move(filterExpr));
        }
        stage = makeFilter<false>(std::move(stage), std::move(filterExpr), planNodeId);

        auto resultSlot = slotIdGenerator->generate();
        auto resultValue = stateHelper.makeState(logicOp == sbe::EPrimBinary::logicOr);
        stage = makeProject(std::move(stage), planNodeId, resultSlot, std::move(resultValue));

        return std::make_pair(resultSlot, std::move(stage));
    };

    return generateSingleResultUnion(std::move(branches), branchFn, planNodeId, slotIdGenerator);
}

std::pair<sbe::value::SlotId, std::unique_ptr<sbe::PlanStage>> generateVirtualScan(
    sbe::value::SlotIdGenerator* slotIdGenerator,
    sbe::value::TypeTags arrTag,
    sbe::value::Value arrVal) {
    // The value passed in must be an array.
    invariant(sbe::value::isArray(arrTag));

    // Make an EConstant expression for the array.
    auto arrayExpression = sbe::makeE<sbe::EConstant>(arrTag, arrVal);

    // Build the unwind/project/limit/coscan subtree.
    auto projectSlot = slotIdGenerator->generate();
    auto unwindSlot = slotIdGenerator->generate();
    auto unwind = sbe::makeS<sbe::UnwindStage>(
        sbe::makeProjectStage(makeLimitCoScanTree(kEmptyPlanNodeId, 1),
                              kEmptyPlanNodeId,
                              projectSlot,
                              std::move(arrayExpression)),
        projectSlot,
        unwindSlot,
        slotIdGenerator->generate(),  // We don't need an index slot but must to provide it.
        false,                        // Don't preserve null and empty arrays.
        kEmptyPlanNodeId);

    // Return the UnwindStage and its output slot. The UnwindStage can be used as an input
    // to other PlanStages.
    return {unwindSlot, std::move(unwind)};
}

std::pair<sbe::value::SlotVector, std::unique_ptr<sbe::PlanStage>> generateVirtualScanMulti(
    sbe::value::SlotIdGenerator* slotIdGenerator,
    int numSlots,
    sbe::value::TypeTags arrTag,
    sbe::value::Value arrVal) {
    using namespace std::literals;

    invariant(numSlots >= 1);

    // Generate a mock scan with a single output slot.
    auto [scanSlot, scanStage] = generateVirtualScan(slotIdGenerator, arrTag, arrVal);

    // Create a ProjectStage that will read the data from 'scanStage' and split it up
    // across multiple output slots.
    sbe::value::SlotVector projectSlots;
    sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> projections;
    for (int32_t i = 0; i < numSlots; ++i) {
        projectSlots.emplace_back(slotIdGenerator->generate());
        projections.emplace(
            projectSlots.back(),
            makeFunction("getElement"_sd,
                         sbe::makeE<sbe::EVariable>(scanSlot),
                         sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                    sbe::value::bitcastFrom<int32_t>(i))));
    }

    return {std::move(projectSlots),
            sbe::makeS<sbe::ProjectStage>(
                std::move(scanStage), std::move(projections), kEmptyPlanNodeId)};
}

std::pair<sbe::value::TypeTags, sbe::value::Value> makeValue(const BSONObj& bo) {
    return sbe::value::copyValue(sbe::value::TypeTags::bsonObject,
                                 sbe::value::bitcastFrom<const char*>(bo.objdata()));
}

std::pair<sbe::value::TypeTags, sbe::value::Value> makeValue(const BSONArray& ba) {
    return sbe::value::copyValue(sbe::value::TypeTags::bsonArray,
                                 sbe::value::bitcastFrom<const char*>(ba.objdata()));
}

std::pair<sbe::value::TypeTags, sbe::value::Value> makeValue(const Value& val) {
    // TODO: Either make this conversion unnecessary by changing the value representation in
    // ExpressionConstant, or provide a nicer way to convert directly from Document/Value to
    // sbe::Value.
    BSONObjBuilder bob;
    val.addToBsonObj(&bob, ""_sd);
    auto obj = bob.done();
    auto be = obj.objdata();
    auto end = be + obj.objsize();
    return sbe::bson::convertFrom<false>(be + 4, end, 0);
}

uint32_t dateTypeMask() {
    return (getBSONTypeMask(sbe::value::TypeTags::Date) |
            getBSONTypeMask(sbe::value::TypeTags::Timestamp) |
            getBSONTypeMask(sbe::value::TypeTags::ObjectId) |
            getBSONTypeMask(sbe::value::TypeTags::bsonObjectId));
}

EvalStage IndexStateHelper::makeTraverseCombinator(
    EvalStage outer,
    EvalStage inner,
    sbe::value::SlotId inputSlot,
    sbe::value::SlotId outputSlot,
    sbe::value::SlotId innerOutputSlot,
    PlanNodeId planNodeId,
    sbe::value::FrameIdGenerator* frameIdGenerator) const {
    // Fold expression is executed only when array has more then 1 element. It increments index
    // value on each iteration. During this process index is paired with false value. Once the
    // predicate evaluates to true, false value of index is changed to true. Final expression of
    // traverse stage detects that now index is paired with true value and it means that we have
    // found an index of array element where predicate evaluates to true.
    //
    // First step is to increment index. Fold expression is always executed when index stored in
    // 'outputSlot' is encoded as a false value. This means that to increment index, we should
    // subtract 1 from it.
    auto frameId = frameIdGenerator->generate();
    auto advancedIndex = sbe::makeE<sbe::EPrimBinary>(
        sbe::EPrimBinary::sub, sbe::makeE<sbe::EVariable>(outputSlot), makeConstant(ValueType, 1));
    auto binds = sbe::makeEs(std::move(advancedIndex));
    sbe::EVariable advancedIndexVar{frameId, 0};

    // In case the predicate in the inner branch of traverse returns true, we want pair
    // incremented index with true value. This will tell final expression of traverse that we
    // have found a matching element and iteration can be stopped.
    // The expression below express the following function: f(x) = abs(x) - 1. This function
    // converts false value to a true value because f(- index - 2) = index + 1 (take a look at
    // the comment for the 'IndexStateHelper' class for encoding description).
    auto indexWithTrueValue =
        sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::sub,
                                     makeFunction("abs", advancedIndexVar.clone()),
                                     makeConstant(ValueType, 1));

    // Finally, we check if the predicate in the inner branch returned true. If that's the case,
    // we pair incremented index with true value. Otherwise, it stays paired with false value.
    auto foldExpr = sbe::makeE<sbe::EIf>(FilterStateHelper::getBool(innerOutputSlot),
                                         std::move(indexWithTrueValue),
                                         advancedIndexVar.clone());

    foldExpr = sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(foldExpr));

    return makeTraverse(std::move(outer),
                        std::move(inner),
                        inputSlot,
                        outputSlot,
                        innerOutputSlot,
                        std::move(foldExpr),
                        FilterStateHelper::getBool(outputSlot),
                        planNodeId,
                        1);
}

std::unique_ptr<FilterStateHelper> makeFilterStateHelper(bool trackIndex) {
    if (trackIndex) {
        return std::make_unique<IndexStateHelper>();
    }
    return std::make_unique<BooleanStateHelper>();
}

sbe::value::SlotVector makeIndexKeyOutputSlotsMatchingParentReqs(
    const BSONObj& indexKeyPattern,
    sbe::IndexKeysInclusionSet parentIndexKeyReqs,
    sbe::IndexKeysInclusionSet childIndexKeyReqs,
    sbe::value::SlotVector childOutputSlots) {
    tassert(5308000,
            "'childIndexKeyReqs' had fewer bits set than 'parentIndexKeyReqs'",
            parentIndexKeyReqs.count() <= childIndexKeyReqs.count());
    sbe::value::SlotVector newIndexKeySlots;

    size_t slotIdx = 0;
    for (size_t indexFieldNumber = 0;
         indexFieldNumber < static_cast<size_t>(indexKeyPattern.nFields());
         ++indexFieldNumber) {
        if (parentIndexKeyReqs.test(indexFieldNumber)) {
            newIndexKeySlots.push_back(childOutputSlots[slotIdx]);
        }

        if (childIndexKeyReqs.test(indexFieldNumber)) {
            ++slotIdx;
        }
    }

    return newIndexKeySlots;
}

sbe::value::SlotId StageBuilderState::getGlobalVariableSlot(Variables::Id variableId) {
    if (auto it = data->variableIdToSlotMap.find(variableId);
        it != data->variableIdToSlotMap.end()) {
        return it->second;
    }

    auto slotId = data->env->registerSlot(
        sbe::value::TypeTags::Nothing, 0, false /* owned */, slotIdGenerator);
    data->variableIdToSlotMap.emplace(variableId, slotId);
    return slotId;
}

/**
 * Callback function that logs a message and uasserts if it detects a corrupt index key. An index
 * key is considered corrupt if it has no corresponding Record.
 */
void indexKeyCorruptionCheckCallback(OperationContext* opCtx,
                                     sbe::value::SlotAccessor* snapshotIdAccessor,
                                     sbe::value::SlotAccessor* indexKeyAccessor,
                                     sbe::value::SlotAccessor* indexKeyPatternAccessor,
                                     const RecordId& rid,
                                     const NamespaceString& nss) {
    // Having a recordId but no record is only an issue when we are not ignoring prepare conflicts.
    if (opCtx->recoveryUnit()->getPrepareConflictBehavior() == PrepareConflictBehavior::kEnforce) {
        tassert(5113700, "Should have snapshot id accessor", snapshotIdAccessor);
        auto currentSnapshotId = opCtx->recoveryUnit()->getSnapshotId();
        auto [snapshotIdTag, snapshotIdVal] = snapshotIdAccessor->getViewOfValue();
        const auto msgSnapshotIdTag = snapshotIdTag;
        tassert(5113701,
                str::stream() << "SnapshotId is of wrong type: " << msgSnapshotIdTag,
                snapshotIdTag == sbe::value::TypeTags::NumberInt64);
        auto snapshotId = sbe::value::bitcastTo<uint64_t>(snapshotIdVal);

        // If we have a recordId but no corresponding record, this means that said record has been
        // deleted. This can occur during yield, in which case the snapshot id would be incremented.
        // If, on the other hand, the current snapshot id matches that of the recordId, this
        // indicates an error as no yield could have taken place.
        if (snapshotId == currentSnapshotId.toNumber()) {
            tassert(5113703, "Should have index key accessor", indexKeyAccessor);
            tassert(5113704, "Should have key pattern accessor", indexKeyPatternAccessor);

            auto [ksTag, ksVal] = indexKeyAccessor->getViewOfValue();
            auto [kpTag, kpVal] = indexKeyPatternAccessor->getViewOfValue();

            const auto msgKsTag = ksTag;
            tassert(5113706,
                    str::stream() << "KeyString is of wrong type: " << msgKsTag,
                    ksTag == sbe::value::TypeTags::ksValue);

            const auto msgKpTag = kpTag;
            tassert(5113707,
                    str::stream() << "Index key pattern is of wrong type: " << msgKpTag,
                    kpTag == sbe::value::TypeTags::bsonObject);

            auto keyString = sbe::value::getKeyStringView(ksVal);
            tassert(5113708, "KeyString does not exist", keyString);

            BSONObj bsonKeyPattern(sbe::value::bitcastTo<const char*>(kpVal));
            auto bsonKeyString = KeyString::toBson(*keyString, Ordering::make(bsonKeyPattern));
            auto hydratedKey = IndexKeyEntry::rehydrateKey(bsonKeyPattern, bsonKeyString);

            LOGV2_ERROR_OPTIONS(
                5113709,
                {logv2::UserAssertAfterLog(ErrorCodes::DataCorruptionDetected)},
                "Erroneous index key found with reference to non-existent record id. Consider "
                "dropping and then re-creating the index and then running the validate command "
                "on the collection.",
                "namespace"_attr = nss,
                "recordId"_attr = rid,
                "indexKeyData"_attr = hydratedKey);
        }
    }
}

/**
 * Callback function that returns true if a given index key is valid, false otherwise. An index key
 * is valid if either the snapshot id of the underlying index scan matches the current snapshot id,
 * or that the index keys are still part of the underlying index.
 */
bool indexKeyConsistencyCheckCallback(OperationContext* opCtx,
                                      StringMap<const IndexAccessMethod*> iamTable,
                                      sbe::value::SlotAccessor* snapshotIdAccessor,
                                      sbe::value::SlotAccessor* indexIdAccessor,
                                      sbe::value::SlotAccessor* indexKeyAccessor,
                                      const CollectionPtr& collection,
                                      const Record& nextRecord) {
    if (snapshotIdAccessor) {
        auto currentSnapshotId = opCtx->recoveryUnit()->getSnapshotId();
        auto [snapshotIdTag, snapshotIdVal] = snapshotIdAccessor->getViewOfValue();
        const auto msgSnapshotIdTag = snapshotIdTag;
        tassert(5290704,
                str::stream() << "SnapshotId is of wrong type: " << msgSnapshotIdTag,
                snapshotIdTag == sbe::value::TypeTags::NumberInt64);

        auto snapshotId = sbe::value::bitcastTo<uint64_t>(snapshotIdVal);
        if (currentSnapshotId.toNumber() != snapshotId) {
            tassert(5290707, "Should have index key accessor", indexKeyAccessor);
            tassert(5290714, "Should have index id accessor", indexIdAccessor);

            auto [indexIdTag, indexIdVal] = indexIdAccessor->getViewOfValue();
            auto [ksTag, ksVal] = indexKeyAccessor->getViewOfValue();

            const auto msgIndexIdTag = indexIdTag;
            tassert(5290708,
                    str::stream() << "Index name is of wrong type: " << msgIndexIdTag,
                    sbe::value::isString(indexIdTag));

            const auto msgKsTag = ksTag;
            tassert(5290710,
                    str::stream() << "KeyString is of wrong type: " << msgKsTag,
                    ksTag == sbe::value::TypeTags::ksValue);

            auto keyString = sbe::value::getKeyStringView(ksVal);
            auto indexId = sbe::value::getStringView(indexIdTag, indexIdVal);
            tassert(5290712, "KeyString does not exist", keyString);

            auto it = iamTable.find(indexId);
            tassert(5290713,
                    str::stream() << "IndexAccessMethod not found for index " << indexId,
                    it != iamTable.end());

            auto iam = it->second->asSortedData();
            tassert(5290709,
                    str::stream() << "Expected to find SortedDataIndexAccessMethod for index "
                                  << indexId,
                    iam);

            auto& executionCtx = StorageExecutionContext::get(opCtx);
            auto keys = executionCtx.keys();
            SharedBufferFragmentBuilder pooledBuilder(
                KeyString::HeapBuilder::kHeapAllocatorDefaultBytes);

            // There's no need to compute the prefixes of the indexed fields that cause the
            // index to be multikey when ensuring the keyData is still valid.
            KeyStringSet* multikeyMetadataKeys = nullptr;
            MultikeyPaths* multikeyPaths = nullptr;

            iam->getKeys(opCtx,
                         collection,
                         pooledBuilder,
                         nextRecord.data.toBson(),
                         InsertDeleteOptions::ConstraintEnforcementMode::kEnforceConstraints,
                         SortedDataIndexAccessMethod::GetKeysContext::kValidatingKeys,
                         keys.get(),
                         multikeyMetadataKeys,
                         multikeyPaths,
                         nextRecord.id);

            return keys->count(*keyString);
        }
    }
    return true;
}

std::unique_ptr<sbe::PlanStage> makeLoopJoinForFetch(std::unique_ptr<sbe::PlanStage> inputStage,
                                                     sbe::value::SlotId resultSlot,
                                                     sbe::value::SlotId recordIdSlot,
                                                     std::vector<std::string> fields,
                                                     sbe::value::SlotVector fieldSlots,
                                                     sbe::value::SlotId seekKeySlot,
                                                     sbe::value::SlotId snapshotIdSlot,
                                                     sbe::value::SlotId indexIdSlot,
                                                     sbe::value::SlotId indexKeySlot,
                                                     sbe::value::SlotId indexKeyPatternSlot,
                                                     const CollectionPtr& collToFetch,
                                                     StringMap<const IndexAccessMethod*> iamMap,
                                                     PlanNodeId planNodeId,
                                                     sbe::value::SlotVector slotsToForward) {
    // It is assumed that we are generating a fetch loop join over the main collection. If we are
    // generating a fetch over a secondary collection, it is the responsibility of a parent node
    // in the QSN tree to indicate which collection we are fetching over.
    tassert(6355301, "Cannot fetch from a collection that doesn't exist", collToFetch);

    sbe::ScanCallbacks callbacks(
        indexKeyCorruptionCheckCallback,
        [=](auto&& arg1, auto&& arg2, auto&& arg3, auto&& arg4, auto&& arg5, auto&& arg6) {
            return indexKeyConsistencyCheckCallback(
                arg1, std::move(iamMap), arg2, arg3, arg4, arg5, arg6);
        });

    // Scan the collection in the range [seekKeySlot, Inf).
    auto scanStage = sbe::makeS<sbe::ScanStage>(collToFetch->uuid(),
                                                resultSlot,
                                                recordIdSlot,
                                                snapshotIdSlot,
                                                indexIdSlot,
                                                indexKeySlot,
                                                indexKeyPatternSlot,
                                                boost::none,
                                                std::move(fields),
                                                std::move(fieldSlots),
                                                seekKeySlot,
                                                true,
                                                nullptr,
                                                planNodeId,
                                                std::move(callbacks));

    // Get the recordIdSlot from the outer side (e.g., IXSCAN) and feed it to the inner side,
    // limiting the result set to 1 row.
    return sbe::makeS<sbe::LoopJoinStage>(
        std::move(inputStage),
        sbe::makeS<sbe::LimitSkipStage>(std::move(scanStage), 1, boost::none, planNodeId),
        std::move(slotsToForward),
        sbe::makeSV(seekKeySlot, snapshotIdSlot, indexIdSlot, indexKeySlot, indexKeyPatternSlot),
        nullptr,
        planNodeId);
}

sbe::value::SlotId StageBuilderState::registerInputParamSlot(
    MatchExpression::InputParamId paramId) {
    auto it = data->inputParamToSlotMap.find(paramId);
    if (it != data->inputParamToSlotMap.end()) {
        // This input parameter id has already been tied to a particular runtime environment slot.
        // Just return that slot to the caller. This can happen if a query planning optimization or
        // rewrite chose to clone one of the input expressions from the user's query.
        return it->second;
    }

    auto slotId = data->env->registerSlot(
        sbe::value::TypeTags::Nothing, 0, false /* owned */, slotIdGenerator);
    data->inputParamToSlotMap.emplace(paramId, slotId);
    return slotId;
}


/**
 * Given a key pattern and an array of slots of equal size, builds an IndexKeyPatternTreeNode
 * representing the mapping between key pattern component and slot.
 *
 * Note that this will "short circuit" in cases where the index key pattern contains two components
 * where one is a subpath of the other. For example with the key pattern {a:1, a.b: 1}, the "a.b"
 * component will not be represented in the output tree. For the purpose of rehydrating index keys,
 * this is fine (and actually preferable).
 */
std::unique_ptr<IndexKeyPatternTreeNode> buildKeyPatternTree(const BSONObj& keyPattern,
                                                             const sbe::value::SlotVector& slots) {
    size_t i = 0;

    auto root = std::make_unique<IndexKeyPatternTreeNode>();
    for (auto&& elem : keyPattern) {
        auto* node = root.get();
        bool skipElem = false;

        sbe::MatchPath fr(elem.fieldNameStringData());
        for (FieldIndex j = 0; j < fr.numParts(); ++j) {
            const auto part = fr.getPart(j);
            if (auto it = node->children.find(part); it != node->children.end()) {
                node = it->second.get();
                if (node->indexKeySlot) {
                    // We're processing the a sub-path of a path that's already indexed.  We can
                    // bail out here since we won't use the sub-path when reconstructing the
                    // object.
                    skipElem = true;
                    break;
                }
            } else {
                node = node->emplace(part);
            }
        }

        if (!skipElem) {
            node->indexKeySlot = slots[i];
        }

        ++i;
    }

    return root;
}

/**
 * Given a root IndexKeyPatternTreeNode, this function will construct an SBE expression for
 * producing a partial object from an index key.
 *
 * For example, given the index key pattern {a.b: 1, x: 1, a.c: 1} and the index key
 * {"": 1, "": 2, "": 3}, the SBE expression would produce the object {a: {b:1, c: 3}, x: 2}.
 */
std::unique_ptr<sbe::EExpression> buildNewObjExpr(const IndexKeyPatternTreeNode* kpTree) {

    sbe::EExpression::Vector args;
    for (auto&& fieldName : kpTree->childrenOrder) {
        auto it = kpTree->children.find(fieldName);

        args.emplace_back(makeConstant(fieldName));
        if (it->second->indexKeySlot) {
            args.emplace_back(makeVariable(*it->second->indexKeySlot));
        } else {
            // The reason this is in an else branch is that in the case where we have an index key
            // like {a.b: ..., a: ...}, we've already made the logic for reconstructing the 'a'
            // portion, so the 'a.b' subtree can be skipped.
            args.push_back(buildNewObjExpr(it->second.get()));
        }
    }

    return sbe::makeE<sbe::EFunction>("newObj", std::move(args));
}

/**
 * Given a stage, and index key pattern a corresponding array of slot IDs, this function
 * add a ProjectStage to the tree which rehydrates the index key and stores the result in
 * 'resultSlot.'
 */
std::unique_ptr<sbe::PlanStage> rehydrateIndexKey(std::unique_ptr<sbe::PlanStage> stage,
                                                  const BSONObj& indexKeyPattern,
                                                  PlanNodeId nodeId,
                                                  const sbe::value::SlotVector& indexKeySlots,
                                                  sbe::value::SlotId resultSlot) {
    auto kpTree = buildKeyPatternTree(indexKeyPattern, indexKeySlots);
    auto keyExpr = buildNewObjExpr(kpTree.get());

    return sbe::makeProjectStage(std::move(stage), nodeId, resultSlot, std::move(keyExpr));
}

/**
 * For covered projections, each of the projection field paths represent respective index key. To
 * rehydrate index keys into the result object, we first need to convert projection AST into
 * 'IndexKeyPatternTreeNode' structure. Context structure and visitors below are used for this
 * purpose.
 */
struct IndexKeysBuilderContext {
    // Contains resulting tree of index keys converted from projection AST.
    IndexKeyPatternTreeNode root;

    // Full field path of the currently visited projection node.
    std::vector<StringData> currentFieldPath;

    // Each projection node has a vector of field names. This stack contains indexes of the
    // currently visited field names for each of the projection nodes.
    std::vector<size_t> currentFieldIndex;
};

/**
 * Covered projections are always inclusion-only, so we ban all other operators.
 */
class IndexKeysBuilder : public projection_ast::ProjectionASTConstVisitor {
public:
    using projection_ast::ProjectionASTConstVisitor::visit;

    IndexKeysBuilder(IndexKeysBuilderContext* context) : _context{context} {}

    void visit(const projection_ast::ProjectionPositionalASTNode* node) final {
        tasserted(5474501, "Positional projection is not allowed in simple or covered projections");
    }

    void visit(const projection_ast::ProjectionSliceASTNode* node) final {
        tasserted(5474502, "$slice is not allowed in simple or covered projections");
    }

    void visit(const projection_ast::ProjectionElemMatchASTNode* node) final {
        tasserted(5474503, "$elemMatch is not allowed in simple or covered projections");
    }

    void visit(const projection_ast::ExpressionASTNode* node) final {
        tasserted(5474504, "Expressions are not allowed in simple or covered projections");
    }

    void visit(const projection_ast::MatchExpressionASTNode* node) final {
        tasserted(
            5474505,
            "$elemMatch / positional projection are not allowed in simple or covered projections");
    }

    void visit(const projection_ast::BooleanConstantASTNode* node) override {}

protected:
    IndexKeysBuilderContext* _context;
};

class IndexKeysPreBuilder final : public IndexKeysBuilder {
public:
    using IndexKeysBuilder::IndexKeysBuilder;
    using IndexKeysBuilder::visit;

    void visit(const projection_ast::ProjectionPathASTNode* node) final {
        _context->currentFieldIndex.push_back(0);
        _context->currentFieldPath.emplace_back(node->fieldNames().front());
    }
};

class IndexKeysInBuilder final : public IndexKeysBuilder {
public:
    using IndexKeysBuilder::IndexKeysBuilder;
    using IndexKeysBuilder::visit;

    void visit(const projection_ast::ProjectionPathASTNode* node) final {
        auto& currentIndex = _context->currentFieldIndex.back();
        currentIndex++;
        _context->currentFieldPath.back() = node->fieldNames()[currentIndex];
    }
};

class IndexKeysPostBuilder final : public IndexKeysBuilder {
public:
    using IndexKeysBuilder::IndexKeysBuilder;
    using IndexKeysBuilder::visit;

    void visit(const projection_ast::ProjectionPathASTNode* node) final {
        _context->currentFieldIndex.pop_back();
        _context->currentFieldPath.pop_back();
    }

    void visit(const projection_ast::BooleanConstantASTNode* constantNode) final {
        if (!constantNode->value()) {
            // Even though only inclusion is allowed in covered projection, there still can be
            // {_id: 0} component. We do not need to generate any nodes for it.
            return;
        }

        // Insert current field path into the index keys tree if it does not exist yet.
        auto* node = &_context->root;
        for (const auto& part : _context->currentFieldPath) {
            if (auto it = node->children.find(part); it != node->children.end()) {
                node = it->second.get();
            } else {
                node = node->emplace(part);
            }
        }
    }
};

IndexKeyPatternTreeNode buildPatternTree(const projection_ast::Projection& projection) {
    IndexKeysBuilderContext context;
    IndexKeysPreBuilder preVisitor{&context};
    IndexKeysInBuilder inVisitor{&context};
    IndexKeysPostBuilder postVisitor{&context};

    projection_ast::ProjectionASTConstWalker walker{&preVisitor, &inVisitor, &postVisitor};

    tree_walker::walk<true, projection_ast::ASTNode>(projection.root(), &walker);

    return std::move(context.root);
}

std::pair<std::unique_ptr<sbe::PlanStage>, sbe::value::SlotVector> projectTopLevelFields(
    std::unique_ptr<sbe::PlanStage> stage,
    const std::vector<std::string>& fields,
    sbe::value::SlotId resultSlot,
    PlanNodeId nodeId,
    sbe::value::SlotIdGenerator* slotIdGenerator) {
    // 'outputSlots' will match the order of 'fields'.
    sbe::value::SlotVector outputSlots;
    outputSlots.reserve(fields.size());

    sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> projects;
    for (size_t i = 0; i < fields.size(); ++i) {
        const auto& field = fields[i];
        auto slot = slotIdGenerator->generate();
        auto getFieldExpr =
            makeFunction("getField"_sd, makeVariable(resultSlot), makeConstant(field));
        projects.insert({slot, std::move(getFieldExpr)});
        outputSlots.emplace_back(slot);
    }

    if (!projects.empty()) {
        stage = sbe::makeS<sbe::ProjectStage>(std::move(stage), std::move(projects), nodeId);
    }

    return {std::move(stage), std::move(outputSlots)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, sbe::value::SlotVector> projectNothingToSlots(
    std::unique_ptr<sbe::PlanStage> stage,
    size_t n,
    PlanNodeId nodeId,
    sbe::value::SlotIdGenerator* slotIdGenerator) {
    if (n == 0) {
        return {std::move(stage), sbe::value::SlotVector{}};
    }

    auto outputSlots = slotIdGenerator->generateMultiple(n);

    sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> projects;
    for (size_t i = 0; i < n; ++i) {
        projects.insert(
            {outputSlots[i], sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Nothing, 0)});
    }

    stage = sbe::makeS<sbe::ProjectStage>(std::move(stage), std::move(projects), nodeId);

    return {std::move(stage), std::move(outputSlots)};
}
}  // namespace mongo::stage_builder
