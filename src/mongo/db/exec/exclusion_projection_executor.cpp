/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/exec/exclusion_projection_executor.h"

namespace mongo::projection_executor {

std::pair<BSONObj, bool> ExclusionNode::extractProjectOnFieldAndRename(const StringData& oldName,
                                                                       const StringData& newName) {
    BSONObjBuilder extractedExclusion;

    // Check for a projection directly on 'oldName'. For example, {oldName: 0}.
    if (auto it = _projectedFieldsSet.find(oldName); it != _projectedFieldsSet.end()) {
        extractedExclusion.append(newName, false);
        _projectedFieldsSet.erase(it);
        _projectedFields.remove(std::string(oldName));
    }

    // Check for a projection on subfields of 'oldName'. For example, {oldName: {a: 0, b: 0}}.
    if (auto it = _children.find(oldName); it != _children.end()) {
        extractedExclusion.append(newName, it->second->serialize(boost::none).toBson());
        _children.erase(it);
    }

    if (auto it = std::find(_orderToProcessAdditionsAndChildren.begin(),
                            _orderToProcessAdditionsAndChildren.end(),
                            oldName);
        it != _orderToProcessAdditionsAndChildren.end()) {
        _orderToProcessAdditionsAndChildren.erase(it);
    }

    return {extractedExclusion.obj(), _projectedFields.empty() && _children.empty()};
}
}  // namespace mongo::projection_executor
