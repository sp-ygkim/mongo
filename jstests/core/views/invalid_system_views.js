/**
 * Tests that invalid view definitions in system.views do not impact valid commands on existing
 * collections.
 *
 * @tags: [
 *   assumes_unsharded_collection,
 *   # applyOps is not available on mongos.
 *   assumes_against_mongod_not_mongos,
 *   assumes_superuser_permissions,
 *   requires_non_retryable_commands,
 *   requires_non_retryable_writes,
 *   # applyOps uses the oplog that require replication support
 *   requires_replication,
 *   # The drop of offending views may not happen on the donor after a committed migration.
 *   tenant_migration_incompatible,
 *   uses_compact,
 * ]
 */

(function() {
"use strict";
const isMongos = db.runCommand({isdbgrid: 1}).isdbgrid;
const isStandalone = !isMongos && !db.runCommand({hello: 1}).hasOwnProperty("setName");

function runTest(badViewDefinition) {
    let viewsDB = db.getSiblingDB("invalid_system_views");
    assert.commandWorked(viewsDB.dropDatabase());

    // Create a regular collection, then insert an invalid view into system.views.
    assert.commandWorked(viewsDB.collection.insert({x: 1}));
    assert.commandWorked(viewsDB.runCommand({create: "collection2"}));
    assert.commandWorked(viewsDB.runCommand({create: "collection3"}));
    assert.commandWorked(viewsDB.collection.createIndex({x: 1}));
    assert.commandWorked(viewsDB.createCollection("system.views"));
    assert.commandWorked(
        viewsDB.adminCommand(
            {applyOps: [{op: "i", ns: viewsDB.getName() + ".system.views", o: badViewDefinition}]}),
        "failed to insert " + tojson(badViewDefinition));

    // Test that a command involving views properly fails with a views-specific error code.
    assert.commandFailedWithCode(
        viewsDB.runCommand({listCollections: 1}),
        ErrorCodes.InvalidViewDefinition,
        "listCollections should have failed in the presence of an invalid view");

    // Helper function to create a message to use if an assertion fails.
    function makeErrorMessage(msg) {
        return msg +
            " should work on a valid, existing collection, despite the presence of bad views" +
            " in system.views";
    }

    if (!isMongos) {
        // Commands that run on existing regular collections should not be impacted by the
        // presence of invalid views. However, applyOps doesn't work on mongos.
        assert.commandWorked(
            db.adminCommand(  //
                {applyOps: [{op: "c", ns: "invalid_system_views.$cmd", o: {drop: "collection3"}}]}),
            makeErrorMessage("applyOps"));
    }

    assert.commandWorked(viewsDB.collection.insert({y: "baz"}), makeErrorMessage("insert"));

    assert.commandWorked(viewsDB.collection.update({y: "baz"}, {$set: {y: "qux"}}),
                         makeErrorMessage("update"));

    assert.commandWorked(viewsDB.collection.remove({y: "baz"}), makeErrorMessage("remove"));

    assert.commandWorked(
        viewsDB.runCommand({findAndModify: "collection", query: {x: 1}, update: {x: 2}}),
        makeErrorMessage("findAndModify with update"));

    assert.commandWorked(
        viewsDB.runCommand({findAndModify: "collection", query: {x: 2}, remove: true}),
        makeErrorMessage("findAndModify with remove"));

    const lookup = {
        $lookup: {from: "collection2", localField: "_id", foreignField: "_id", as: "match"}
    };
    assert.commandWorked(
        viewsDB.runCommand({aggregate: "collection", pipeline: [lookup], cursor: {}}),
        makeErrorMessage("aggregate with $lookup"));

    const graphLookup = {
            $graphLookup: {
                from: "collection2",
                startWith: "$_id",
                connectFromField: "_id",
                connectToField: "_id",
                as: "match"
            }
        };
    assert.commandWorked(
        viewsDB.runCommand({aggregate: "collection", pipeline: [graphLookup], cursor: {}}),
        makeErrorMessage("aggregate with $graphLookup"));

    assert.commandWorked(viewsDB.runCommand({dropIndexes: "collection", index: "x_1"}),
                         makeErrorMessage("dropIndexes"));

    assert.commandWorked(viewsDB.collection.createIndex({x: 1}), makeErrorMessage("createIndexes"));

    // Only standalone mode supports the reIndex command.
    if (isStandalone) {
        assert.commandWorked(viewsDB.collection.reIndex(), makeErrorMessage("reIndex"));
    }

    const storageEngine = jsTest.options().storageEngine;
    if (isMongos || storageEngine === "inMemory") {
        print("Not testing compact command on mongos or ephemeral storage engine");
    } else {
        assert.commandWorked(viewsDB.runCommand({compact: "collection", force: true}),
                             makeErrorMessage("compact"));
    }

    assert.commandWorked(
        viewsDB.runCommand(
            {collMod: "collection", validator: {x: {$type: "string"}}, validationAction: "warn"}),
        makeErrorMessage("collMod"));

    const renameCommand = {
        renameCollection: "invalid_system_views.collection",
        to: "invalid_system_views.collection2",
        dropTarget: true
    };
    assert.commandWorked(viewsDB.adminCommand(renameCommand), makeErrorMessage("renameCollection"));

    assert.commandWorked(viewsDB.runCommand({drop: "collection2"}), makeErrorMessage("drop"));

    // Drop the offending view so that the validate hook succeeds.
    assert.commandWorked(viewsDB.adminCommand(
        {applyOps: [{op: "d", ns: viewsDB.getName() + ".system.views", o: badViewDefinition}]}));
}

runTest({_id: "invalid_system_views.badViewStringPipeline", viewOn: "collection", pipeline: "bad"});
runTest(
    {_id: "invalid_system_views.badViewEmptyObjectPipeline", viewOn: "collection", pipeline: {}});
runTest({_id: "invalid_system_views.badViewNumericalPipeline", viewOn: "collection", pipeline: 7});
runTest({
    _id: "invalid_system_views.badViewArrayWithIntegerPipeline",
    viewOn: "collection",
    pipeline: [1]
});
runTest({
    _id: "invalid_system_views.badViewArrayWithEmptyArrayPipeline",
    viewOn: "collection",
    pipeline: [[]]
});
runTest({_id: 7, viewOn: "collection", pipeline: []});
runTest({_id: "invalid_system_views.embedded\0null", viewOn: "collection", pipeline: []});
runTest({_id: "invalidNotFullyQualifiedNs", viewOn: "collection", pipeline: []});
runTest({_id: "invalid_system_views.missingViewOnField", pipeline: []});
}());
