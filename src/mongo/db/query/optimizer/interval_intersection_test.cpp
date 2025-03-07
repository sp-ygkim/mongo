/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <string>
#include <vector>

#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/metadata_factory.h"
#include "mongo/db/query/optimizer/opt_phase_manager.h"
#include "mongo/db/query/optimizer/rewrites/const_eval.h"
#include "mongo/db/query/optimizer/utils/interval_utils.h"
#include "mongo/db/query/optimizer/utils/unit_test_pipeline_utils.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/processinfo.h"

namespace mongo::optimizer {
namespace {

ABT optimizedQueryPlan(const std::string& query,
                       const opt::unordered_map<std::string, IndexDefinition>& indexes) {
    PrefixId prefixId;
    std::string scanDefName = "coll";
    Metadata metadata = {{{scanDefName, createScanDef({}, indexes)}}};
    ABT translated =
        translatePipeline(metadata, "[{$match: " + query + "}]", scanDefName, prefixId);

    auto phaseManager = makePhaseManager({OptPhase::MemoSubstitutionPhase,
                                          OptPhase::MemoExplorationPhase,
                                          OptPhase::MemoImplementationPhase},
                                         prefixId,
                                         metadata,
                                         DebugInfo::kDefaultForTests);

    ABT optimized = translated;
    phaseManager.getHints()._disableScan = true;
    phaseManager.optimize(optimized);
    return optimized;
}

TEST(IntervalIntersection, SingleFieldIntersection) {
    opt::unordered_map<std::string, IndexDefinition> testIndex = {
        {"index1", makeIndexDefinition("a0", CollationOp::Ascending, /*Not multikey*/ false)}};

    const std::string q1Text = "{a0: {$gt:14, $lt:21}}";
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "BinaryJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip []\n"
        "|   |   limitSkip:\n"
        "|   |       limit: 1\n"
        "|   |       skip: 0\n"
        "|   Seek [ridProjection: rid_0, {'<root>': scan_0}, coll]\n"
        "|   |   BindBlock:\n"
        "|   |       [scan_0]\n"
        "|   |           Source []\n"
        "|   RefBlock: \n"
        "|       Variable [rid_0]\n"
        "IndexScan [{'<rid>': rid_0}, scanDefName: coll, indexDefName: index1, interval: "
        "{(Const [14], Const [21])}]\n"
        "    BindBlock:\n"
        "        [rid_0]\n"
        "            Source []\n",
        optimizedQueryPlan(q1Text, testIndex));

    const std::string q2Text = "{$and: [{a0: {$gt:14}}, {a0: {$lt: 21}}]}";
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "BinaryJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip []\n"
        "|   |   limitSkip:\n"
        "|   |       limit: 1\n"
        "|   |       skip: 0\n"
        "|   Seek [ridProjection: rid_0, {'<root>': scan_0}, coll]\n"
        "|   |   BindBlock:\n"
        "|   |       [scan_0]\n"
        "|   |           Source []\n"
        "|   RefBlock: \n"
        "|       Variable [rid_0]\n"
        "IndexScan [{'<rid>': rid_0}, scanDefName: coll, indexDefName: index1, interval: "
        "{(Const [14], Const [21])}]\n"
        "    BindBlock:\n"
        "        [rid_0]\n"
        "            Source []\n",
        optimizedQueryPlan(q2Text, testIndex));

    const std::string q3Text =
        "{$or: [{$and: [{a0: {$gt:9, $lt:999}}, {a0: {$gt: 0, $lt: 12}}]}, {$and: [{a0: {$gt:40, "
        "$lt:997}}, {a0: {$gt:0, $lt: 44}}]}]}";
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "BinaryJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip []\n"
        "|   |   limitSkip:\n"
        "|   |       limit: 1\n"
        "|   |       skip: 0\n"
        "|   Seek [ridProjection: rid_0, {'<root>': scan_0}, coll]\n"
        "|   |   BindBlock:\n"
        "|   |       [scan_0]\n"
        "|   |           Source []\n"
        "|   RefBlock: \n"
        "|       Variable [rid_0]\n"
        "GroupBy []\n"
        "|   |   groupings: \n"
        "|   |       RefBlock: \n"
        "|   |           Variable [rid_0]\n"
        "|   aggregations: \n"
        "Union []\n"
        "|   |   BindBlock:\n"
        "|   |       [rid_0]\n"
        "|   |           Source []\n"
        "|   IndexScan [{'<rid>': rid_0}, scanDefName: coll, indexDefName: index1, interval: {(Co"
        "nst [40], Const [44])}]\n"
        "|       BindBlock:\n"
        "|           [rid_0]\n"
        "|               Source []\n"
        "IndexScan [{'<rid>': rid_0}, scanDefName: coll, indexDefName: index1, interval: {(Const "
        "[9], Const [12])}]\n"
        "    BindBlock:\n"
        "        [rid_0]\n"
        "            Source []\n",
        optimizedQueryPlan(q3Text, testIndex));

    // Contradiction: empty interval.
    const std::string q4Text = "{$and: [{a0: {$gt:20}}, {a0: {$lt: 20}}]}";
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [scan_0]\n"
        "|           Const [Nothing]\n"
        "LimitSkip []\n"
        "|   limitSkip:\n"
        "|       limit: 0\n"
        "|       skip: 0\n"
        "CoScan []\n",
        optimizedQueryPlan(q4Text, testIndex));

    // Contradiction: one conjunct non-empty, one conjunct empty.
    const std::string q5Text =
        "{$or: [{$and: [{a0: {$gt:9}}, {a0: {$lt: 12}}]}, {$and: [{a0: {$gt:44}}, {a0: {$lt: "
        "40}}]}]}";
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "BinaryJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip []\n"
        "|   |   limitSkip:\n"
        "|   |       limit: 1\n"
        "|   |       skip: 0\n"
        "|   Seek [ridProjection: rid_0, {'<root>': scan_0}, coll]\n"
        "|   |   BindBlock:\n"
        "|   |       [scan_0]\n"
        "|   |           Source []\n"
        "|   RefBlock: \n"
        "|       Variable [rid_0]\n"
        "IndexScan [{'<rid>': rid_0}, scanDefName: coll, indexDefName: index1, interval: "
        "{(Const [9], Const [12])}]\n"
        "    BindBlock:\n"
        "        [rid_0]\n"
        "            Source []\n",
        optimizedQueryPlan(q5Text, testIndex));

    // Contradiction: both conjuncts empty, whole disjunct empty.
    const std::string q6Text =
        "{$or: [{$and: [{a0: {$gt:15}}, {a0: {$lt: 10}}]}, {$and: [{a0: {$gt:44}}, {a0: {$lt: "
        "40}}]}]}";
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [scan_0]\n"
        "|           Const [Nothing]\n"
        "LimitSkip []\n"
        "|   limitSkip:\n"
        "|       limit: 0\n"
        "|       skip: 0\n"
        "CoScan []\n",
        optimizedQueryPlan(q6Text, testIndex));

    // Contradiction.
    const std::string q7Text =
        "{$or: [{$and: [{a0: {$gt:12}}, {a0: {$lt: 12}}]}, {$and: [{a0: {$gte:42}}, {a0: {$lt: "
        "42}}]}]}";
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [scan_0]\n"
        "|           Const [Nothing]\n"
        "LimitSkip []\n"
        "|   limitSkip:\n"
        "|       limit: 0\n"
        "|       skip: 0\n"
        "CoScan []\n",
        optimizedQueryPlan(q7Text, testIndex));
}

TEST(IntervalIntersection, MultiFieldIntersection) {
    std::vector<TestIndexField> indexFields{{"a0", CollationOp::Ascending, false},
                                            {"b0", CollationOp::Ascending, false}};

    opt::unordered_map<std::string, IndexDefinition> testIndex = {
        {"index1", makeCompositeIndexDefinition(indexFields, false /*isMultiKey*/)}};

    // Note those are queries below are contradictions.

    const std::string q1Text =
        "{$and: [{a0: {$gt: 11}}, {a0: {$lt: 14}}, {b0: {$gt: 21}}, {b0: {$lt: 12}}]}";
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [scan_0]\n"
        "|           Const [Nothing]\n"
        "LimitSkip []\n"
        "|   limitSkip:\n"
        "|       limit: 0\n"
        "|       skip: 0\n"
        "CoScan []\n",
        optimizedQueryPlan(q1Text, testIndex));

    const std::string q2Text =
        "{$and: [{a0: {$gt: 14}}, {a0: {$lt: 11}}, {b0: {$gt: 12}}, {b0: {$lt: 21}}]}";
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [scan_0]\n"
        "|           Const [Nothing]\n"
        "LimitSkip []\n"
        "|   limitSkip:\n"
        "|       limit: 0\n"
        "|       skip: 0\n"
        "CoScan []\n",
        optimizedQueryPlan(q2Text, testIndex));

    const std::string q3Text =
        "{$and: [{a0: {$gt: 14}}, {a0: {$lt: 11}}, {b0: {$gt: 21}}, {b0: {$lt: 12}}]}";
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [scan_0]\n"
        "|           Const [Nothing]\n"
        "LimitSkip []\n"
        "|   limitSkip:\n"
        "|       limit: 0\n"
        "|       skip: 0\n"
        "CoScan []\n",
        optimizedQueryPlan(q3Text, testIndex));

    const std::string q4Text = "{$and: [{a0: 42}, {b0: {$gt: 21}}, {b0: {$lt: 12}}]}";
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [scan_0]\n"
        "|           Const [Nothing]\n"
        "LimitSkip []\n"
        "|   limitSkip:\n"
        "|       limit: 0\n"
        "|       skip: 0\n"
        "CoScan []\n",
        optimizedQueryPlan(q4Text, testIndex));
}

TEST(IntervalIntersection, VariableIntervals) {
    const auto constFold = ConstEval::constFold;

    {
        auto interval =
            IntervalReqExpr::make<IntervalReqExpr::Disjunction>(IntervalReqExpr::NodeVector{
                IntervalReqExpr::make<IntervalReqExpr::Conjunction>(IntervalReqExpr::NodeVector{
                    IntervalReqExpr::make<IntervalReqExpr::Atom>(IntervalRequirement{
                        BoundRequirement(true /*inclusive*/, make<Variable>("v1")),
                        BoundRequirement::makePlusInf()}),
                    IntervalReqExpr::make<IntervalReqExpr::Atom>(IntervalRequirement{
                        BoundRequirement(false /*inclusive*/, make<Variable>("v2")),
                        BoundRequirement::makePlusInf()})})});

        auto result = intersectDNFIntervals(interval, constFold);
        ASSERT_TRUE(result);

        // (max(v1, v2), +inf) U [v2 >= v1 ? MaxKey : v1, max(v1, v2)]
        ASSERT_EQ(
            "{\n"
            "    {\n"
            "        {[If [] BinaryOp [Gte] Variable [v2] Variable [v1] Const [maxKey] Variable "
            "[v1], If [] BinaryOp [Gte] Variable [v1] Variable [v2] Variable [v1] Variable [v2]]}\n"
            "    }\n"
            " U \n"
            "    {\n"
            "        {(If [] BinaryOp [Gte] Variable [v1] Variable [v2] Variable [v1] Variable "
            "[v2], Const [maxKey]]}\n"
            "    }\n"
            "}\n",
            ExplainGenerator::explainIntervalExpr(*result));

        // Make sure repeated intersection does not change the result.
        auto result1 = intersectDNFIntervals(*result, constFold);
        ASSERT_TRUE(result1);
        ASSERT_TRUE(*result == *result1);
    }

    {
        auto interval =
            IntervalReqExpr::make<IntervalReqExpr::Disjunction>(IntervalReqExpr::NodeVector{
                IntervalReqExpr::make<IntervalReqExpr::Conjunction>(IntervalReqExpr::NodeVector{
                    IntervalReqExpr::make<IntervalReqExpr::Atom>(IntervalRequirement{
                        BoundRequirement(true /*inclusive*/, make<Variable>("v1")),
                        BoundRequirement(true /*inclusive*/, make<Variable>("v3"))}),
                    IntervalReqExpr::make<IntervalReqExpr::Atom>(IntervalRequirement{
                        BoundRequirement(true /*inclusive*/, make<Variable>("v2")),
                        BoundRequirement(true /*inclusive*/, make<Variable>("v4"))})})});

        auto result = intersectDNFIntervals(interval, constFold);
        ASSERT_TRUE(result);

        // [v1, v3] ^ [v2, v4] -> [max(v1, v2), min(v3, v4)]
        ASSERT_EQ(
            "{\n"
            "    {\n"
            "        {[If [] BinaryOp [Gte] Variable [v1] Variable [v2] Variable [v1] Variable "
            "[v2], If [] BinaryOp [Lte] Variable [v3] Variable [v4] Variable [v3] Variable [v4]]}\n"
            "    }\n"
            "}\n",
            ExplainGenerator::explainIntervalExpr(*result));

        // Make sure repeated intersection does not change the result.
        auto result1 = intersectDNFIntervals(*result, constFold);
        ASSERT_TRUE(result1);
        ASSERT_TRUE(*result == *result1);
    }

    {
        auto interval =
            IntervalReqExpr::make<IntervalReqExpr::Disjunction>(IntervalReqExpr::NodeVector{
                IntervalReqExpr::make<IntervalReqExpr::Conjunction>(IntervalReqExpr::NodeVector{
                    IntervalReqExpr::make<IntervalReqExpr::Atom>(IntervalRequirement{
                        BoundRequirement(false /*inclusive*/, make<Variable>("v1")),
                        BoundRequirement(true /*inclusive*/, make<Variable>("v3"))}),
                    IntervalReqExpr::make<IntervalReqExpr::Atom>(IntervalRequirement{
                        BoundRequirement(true /*inclusive*/, make<Variable>("v2")),
                        BoundRequirement(true /*inclusive*/, make<Variable>("v4"))})})});

        auto result = intersectDNFIntervals(interval, constFold);
        ASSERT_TRUE(result);

        ASSERT_EQ(
            "{\n"
            "    {\n"
            "        {[If [] BinaryOp [Gte] Variable [v1] Variable [v2] Const [maxKey] Variable "
            "[v2], If [] BinaryOp [Lte] If [] BinaryOp [Gte] Variable [v1] Variable [v2] Variable "
            "[v1] Variable [v2] If [] BinaryOp [Lte] Variable [v3] Variable [v4] Variable [v3] "
            "Variable [v4] If [] BinaryOp [Gte] Variable [v1] Variable [v2] Variable [v1] Variable "
            "[v2] If [] BinaryOp [Lte] Variable [v3] Variable [v4] Variable [v3] Variable [v4]]}\n"
            "    }\n"
            " U \n"
            "    {\n"
            "        {(If [] BinaryOp [Gte] Variable [v1] Variable [v2] Variable [v1] Variable "
            "[v2], If [] BinaryOp [Lte] Variable [v3] Variable [v4] Variable [v3] Variable [v4]]}\n"
            "    }\n"
            "}\n",
            ExplainGenerator::explainIntervalExpr(*result));

        // Make sure repeated intersection does not change the result.
        auto result1 = intersectDNFIntervals(*result, constFold);
        ASSERT_TRUE(result1);
        ASSERT_TRUE(*result == *result1);
    }

    {
        auto interval =
            IntervalReqExpr::make<IntervalReqExpr::Disjunction>(IntervalReqExpr::NodeVector{
                IntervalReqExpr::make<IntervalReqExpr::Conjunction>(IntervalReqExpr::NodeVector{
                    IntervalReqExpr::make<IntervalReqExpr::Atom>(IntervalRequirement{
                        BoundRequirement(false /*inclusive*/, make<Variable>("v1")),
                        BoundRequirement(true /*inclusive*/, make<Variable>("v3"))}),
                    IntervalReqExpr::make<IntervalReqExpr::Atom>(IntervalRequirement{
                        BoundRequirement(true /*inclusive*/, make<Variable>("v2")),
                        BoundRequirement(false /*inclusive*/, make<Variable>("v4"))})})});

        auto result = intersectDNFIntervals(interval, constFold);
        ASSERT_TRUE(result);

        ASSERT_EQ(
            "{\n"
            "    {\n"
            "        {[If [] BinaryOp [Gte] Variable [v1] Variable [v2] Const [maxKey] Variable "
            "[v2], If [] BinaryOp [Lte] If [] BinaryOp [Gte] Variable [v1] Variable [v2] Variable "
            "[v1] Variable [v2] If [] BinaryOp [Lte] Variable [v3] Variable [v4] Variable [v3] "
            "Variable [v4] If [] BinaryOp [Gte] Variable [v1] Variable [v2] Variable [v1] Variable "
            "[v2] If [] BinaryOp [Lte] Variable [v3] Variable [v4] Variable [v3] Variable [v4]]}\n"
            "    }\n"
            " U \n"
            "    {\n"
            "        {[If [] BinaryOp [Gte] If [] BinaryOp [Gte] Variable [v1] Variable [v2] "
            "Variable [v1] Variable [v2] If [] BinaryOp [Lte] Variable [v3] Variable [v4] Variable "
            "[v3] Variable [v4] If [] BinaryOp [Gte] Variable [v1] Variable [v2] Variable [v1] "
            "Variable [v2] If [] BinaryOp [Lte] Variable [v3] Variable [v4] Variable [v3] Variable "
            "[v4], If [] BinaryOp [Lte] Variable [v4] Variable [v3] Const [minKey] Variable "
            "[v3]]}\n"
            "    }\n"
            " U \n"
            "    {\n"
            "        {(If [] BinaryOp [Gte] Variable [v1] Variable [v2] Variable [v1] Variable "
            "[v2], If [] BinaryOp [Lte] Variable [v3] Variable [v4] Variable [v3] Variable [v4])}\n"
            "    }\n"
            "}\n",
            ExplainGenerator::explainIntervalExpr(*result));

        // Make sure repeated intersection does not change the result.
        auto result1 = intersectDNFIntervals(*result, constFold);
        ASSERT_TRUE(result1);
        ASSERT_TRUE(*result == *result1);
    }
}

template <int N>
void updateResults(const bool lowInc,
                   const int low,
                   const bool highInc,
                   const int high,
                   std::bitset<N>& inclusion) {
    for (int v = 0; v < low + (lowInc ? 0 : 1); v++) {
        inclusion.set(v, false);
    }
    for (int v = high + (highInc ? 1 : 0); v < N; v++) {
        inclusion.set(v, false);
    }
}

template <int N>
class IntervalInclusionTransport {
public:
    using ResultType = std::bitset<N>;

    ResultType transport(const IntervalReqExpr::Atom& node) {
        const auto& expr = node.getExpr();
        const auto& lb = expr.getLowBound();
        const auto& hb = expr.getHighBound();

        std::bitset<N> result;
        result.flip();
        updateResults<N>(lb.isInclusive(),
                         lb.getBound().cast<Constant>()->getValueInt32(),
                         hb.isInclusive(),
                         hb.getBound().cast<Constant>()->getValueInt32(),
                         result);
        return result;
    }

    ResultType transport(const IntervalReqExpr::Conjunction& node,
                         std::vector<ResultType> childResults) {
        for (size_t index = 1; index < childResults.size(); index++) {
            childResults.front() &= childResults.at(index);
        }
        return childResults.front();
    }

    ResultType transport(const IntervalReqExpr::Disjunction& node,
                         std::vector<ResultType> childResults) {
        for (size_t index = 1; index < childResults.size(); index++) {
            childResults.front() |= childResults.at(index);
        }
        return childResults.front();
    }

    ResultType computeInclusion(const IntervalReqExpr::Node& intervals) {
        return algebra::transport<false>(intervals, *this);
    }
};

template <int V>
int decode(int& permutation) {
    const int result = permutation % V;
    permutation /= V;
    return result;
}

template <int N>
void testInterval(int permutation) {
    const bool low1Inc = decode<2>(permutation);
    const int low1 = decode<N>(permutation);
    const bool high1Inc = decode<2>(permutation);
    const int high1 = decode<N>(permutation);
    const bool low2Inc = decode<2>(permutation);
    const int low2 = decode<N>(permutation);
    const bool high2Inc = decode<2>(permutation);
    const int high2 = decode<N>(permutation);

    auto interval = IntervalReqExpr::make<IntervalReqExpr::Disjunction>(IntervalReqExpr::NodeVector{
        IntervalReqExpr::make<IntervalReqExpr::Conjunction>(IntervalReqExpr::NodeVector{
            IntervalReqExpr::make<IntervalReqExpr::Atom>(IntervalRequirement{
                {low1Inc, Constant::int32(low1)}, {high1Inc, Constant::int32(high1)}}),
            IntervalReqExpr::make<IntervalReqExpr::Atom>(IntervalRequirement{
                {low2Inc, Constant::int32(low2)}, {high2Inc, Constant::int32(high2)}})})});

    auto result = intersectDNFIntervals(interval, ConstEval::constFold);
    std::bitset<N> inclusionActual;
    if (result) {
        // Since we are testing with constants, we should have at most one interval.
        ASSERT_TRUE(IntervalReqExpr::getSingularDNF(*result));

        IntervalInclusionTransport<N> transport;
        // Compute actual inclusion bitset based on the interval intersection code.
        inclusionActual = transport.computeInclusion(*result);
    }

    std::bitset<N> inclusionExpected;
    inclusionExpected.flip();

    // Compute ground truth.
    updateResults<N>(low1Inc, low1, high1Inc, high1, inclusionExpected);
    updateResults<N>(low2Inc, low2, high2Inc, high2, inclusionExpected);

    ASSERT_EQ(inclusionExpected, inclusionActual);
}

TEST(IntervalIntersection, IntervalPermutations) {
    static constexpr int N = 10;
    static constexpr int numPermutations = N * N * N * N * 2 * 2 * 2 * 2;

    /**
     * Test for interval intersection. Generate intervals with constants in the
     * range of [0, N), with random inclusion/exclusion of the endpoints. Intersect the intervals
     * and verify against ground truth.
     */
    const size_t numThreads = ProcessInfo::getNumCores();
    std::cout << "Testing " << numPermutations << " interval permutations using " << numThreads
              << " cores...\n";
    auto timeBegin = Date_t::now();

    AtomicWord<int> permutation(0);
    std::vector<stdx::thread> threads;
    for (size_t i = 0; i < numThreads; i++) {
        threads.emplace_back([&permutation]() {
            for (;;) {
                const int nextP = permutation.fetchAndAdd(1);
                if (nextP >= numPermutations) {
                    break;
                }
                testInterval<N>(nextP);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    const auto elapsed =
        (Date_t::now().toMillisSinceEpoch() - timeBegin.toMillisSinceEpoch()) / 1000.0;
    std::cout << "...done. Took: " << elapsed << " s.\n";
}

}  // namespace
}  // namespace mongo::optimizer
