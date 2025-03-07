# Copyright (C) 2022-present MongoDB, Inc.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the Server Side Public License, version 1,
# as published by MongoDB, Inc.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# Server Side Public License for more details.
#
# You should have received a copy of the Server Side Public License
# along with this program. If not, see
# <http://www.mongodb.com/licensing/server-side-public-license>.
#
# As a special exception, the copyright holders give permission to link the
# code of portions of this program with the OpenSSL library under certain
# conditions as described in each individual source file and distribute
# linked combinations including the program with the OpenSSL library. You
# must comply with the Server Side Public License in all respects for
# all of the code used other than as permitted herein. If you modify file(s)
# with this exception, you may extend this exception to your version of the
# file(s), but you are not obligated to do so. If you do not wish to do so,
# delete this exception statement from your version. If you delete this
# exception statement from all source files in the program, then also delete
# it in the license file.
#
"""Cost Model Calibrator entry point."""

import dataclasses
import os
import csv
import asyncio
from typing import Mapping, Sequence
from config import WriteMode
from cost_estimator import ExecutionStats, CostModelParameters
from data_generator import CollectionInfo, DataGenerator
from database_instance import DatabaseInstance
import abt_calibrator
import workload_execution
from workload_execution import Query, QueryParameters
import parameters_extractor
from calibration_settings import distributions, main_config

__all__ = []


def save_to_csv(parameters: Mapping[str, Sequence[CostModelParameters]], filepath: str) -> None:
    """Save model input parameters to a csv file."""
    abt_type_name = 'abt_type'
    fieldnames = [
        abt_type_name, *[f.name for f in dataclasses.fields(ExecutionStats)],
        *[f.name for f in dataclasses.fields(QueryParameters)]
    ]
    with open(filepath, 'w', newline='') as csvfile:
        writer = csv.DictWriter(csvfile, fieldnames=fieldnames)
        writer.writeheader()
        for abt_type, type_params_list in parameters.items():
            for type_params in type_params_list:
                fields = dataclasses.asdict(type_params.execution_stats) | dataclasses.asdict(
                    type_params.query_params)
                fields[abt_type_name] = abt_type
                writer.writerow(fields)


async def execute_general(database: DatabaseInstance, collections: Sequence[CollectionInfo]):
    requests = []
    for val in distributions['string_choice'].get_values()[::3]:
        keys_length = len(val) + 2
        for i in range(1, 5):
            requests.append(
                Query(pipeline=[{'$match': {f'choice{i}': val}}], keys_length_in_bytes=keys_length))

    await workload_execution.execute(database, main_config.workload_execution,
                                     [ci for ci in collections if ci.name.startswith('c_str')],
                                     requests)


async def execute_index_intersections_with_requests(database: DatabaseInstance,
                                                    collections: Sequence[CollectionInfo],
                                                    requests: Sequence[Query]):
    try:
        await database.set_parameter('internalCostModelCoefficients',
                                     '{"filterIncrementalCost": 10000.0}')
        await database.set_parameter('internalCascadesOptimizerDisableMergeJoinRIDIntersect', False)
        await database.set_parameter('internalCascadesOptimizerDisableHashJoinRIDIntersect', False)

        await workload_execution.execute(database, main_config.workload_execution, collections,
                                         requests)

        await database.set_parameter('internalCascadesOptimizerDisableMergeJoinRIDIntersect', True)
        await database.set_parameter('internalCascadesOptimizerDisableHashJoinRIDIntersect', True)

        main_config.workload_execution.write_mode = WriteMode.APPEND
        await workload_execution.execute(database, main_config.workload_execution, collections,
                                         requests[::4])

    finally:
        await database.set_parameter('internalCascadesOptimizerDisableMergeJoinRIDIntersect', False)
        await database.set_parameter('internalCascadesOptimizerDisableHashJoinRIDIntersect', False)
        await database.set_parameter('internalCostModelCoefficients', '')


async def execute_index_intersections(database: DatabaseInstance,
                                      collections: Sequence[CollectionInfo]):
    collections = [ci for ci in collections if ci.name.startswith('c_int')]

    requests = []

    for i in range(0, 1000, 100):
        requests.append(Query(pipeline=[{'$match': {'in1': i, 'in2': i}}], keys_length_in_bytes=1))

        requests.append(
            Query(pipeline=[{'$match': {'in1': i, 'in2': 1000 - i}}], keys_length_in_bytes=1))

        requests.append(
            Query(pipeline=[{'$match': {'in1': {'$lte': i}, 'in2': 1000 - i}}],
                  keys_length_in_bytes=1))

        requests.append(
            Query(pipeline=[{'$match': {'in1': i, 'in2': {'$gt': 1000 - i}}}],
                  keys_length_in_bytes=1))

    await execute_index_intersections_with_requests(database, collections, requests)


async def execute_evaluation(database: DatabaseInstance, collections: Sequence[CollectionInfo]):
    collections = [ci for ci in collections if ci.name.startswith('c_int')]
    requests = []

    for i in range(500, 1000, 100):
        requests.append(
            Query(pipeline=[{"$match": {'in1': {"$gt": i}}}, {'$project': {'proj1': 1}}],
                  keys_length_in_bytes=1, number_of_fields=1))

    await workload_execution.execute(database, main_config.workload_execution, collections,
                                     requests)


async def execute_unwind(database: DatabaseInstance, collections: Sequence[CollectionInfo]):
    collections = [ci for ci in collections if ci.name.startswith('c_arr_01')]
    requests = []
    # average size of arrays in the collection
    average_size_of_arrays = 10

    for _ in range(500, 1000, 100):
        requests.append(
            Query(pipeline=[{"$unwind": "$as"}], number_of_fields=average_size_of_arrays))

    await workload_execution.execute(database, main_config.workload_execution, collections,
                                     requests)


async def execute_small_queries(database: DatabaseInstance, collections: Sequence[CollectionInfo]):
    # strings
    requests = []
    for val in distributions['string_choice_small'].get_values():
        keys_length = len(val) + 2
        for i in range(1, 3):
            requests.append(
                Query(pipeline=[{'$match': {f'choice{i}': val}}], keys_length_in_bytes=keys_length))

    await workload_execution.execute(database, main_config.workload_execution,
                                     [ci for ci in collections if ci.name.startswith('c_str_02')],
                                     requests)

    # index intersection
    colls = [ci for ci in collections if ci.name.startswith('c_int_05')]
    requests = []

    for val in distributions['int_choice'].get_values():
        for val2 in distributions['int_choice'].get_values():
            requests.append(
                Query(pipeline=[{'$match': {'in1': val, 'in2': val2}}], keys_length_in_bytes=1))

        requests.append(
            Query(pipeline=[{'$match': {'in1': val, 'in2': {'$gt': 500}}}], keys_length_in_bytes=1))

        requests.append(
            Query(pipeline=[{'$match': {'in1': {'$lte': 500}, 'in2': val}}],
                  keys_length_in_bytes=1))

    await execute_index_intersections_with_requests(database, colls, requests)

    # Evaluation
    colls = [ci for ci in collections if ci.name.startswith('c_int_05')]
    requests = []

    for val in distributions['int_choice'].get_values():
        requests.append(
            Query(pipeline=[{"$match": {'in1': val}}, {'$project': {'proj1': 1}}],
                  keys_length_in_bytes=1, number_of_fields=1))

    await workload_execution.execute(database, main_config.workload_execution, colls, requests)

    # Unwind
    colls = [ci for ci in collections if ci.name.startswith('c_arr_01')]
    requests = []
    # average size of arrays in the collection
    average_size_of_arrays = 10

    for val in distributions['int_choice'].get_values():
        requests.append(
            Query(pipeline=[{"$match": {'in1': val}}, {"$unwind": "$as"}],
                  number_of_fields=average_size_of_arrays))

    await workload_execution.execute(database, main_config.workload_execution, colls, requests)


async def main():
    """Entry point function."""
    script_directory = os.path.abspath(os.path.dirname(__file__))
    os.chdir(script_directory)

    # 1. Database Instance provides connectivity to a MongoDB instance, it loads data optionally
    # from the dump on creating and stores data optionally to the dump on closing.
    with DatabaseInstance(main_config.database) as database:

        # 2. Data generation (optional), generates random data and populates collections with it.
        generator = DataGenerator(database, main_config.data_generator)
        await generator.populate_collections()

        # 3. Collecting data for calibration (optional).
        # It runs the pipelines and stores explains to the database.

        # Run this execute function only to collect calibration data in the "smaller" experiment.
        # await execute_small_queries(database, generator.collection_infos);

        await execute_general(database, generator.collection_infos)
        main_config.workload_execution.write_mode = WriteMode.APPEND

        await execute_index_intersections(database, generator.collection_infos)

        await execute_evaluation(database, generator.collection_infos)

        await execute_unwind(database, generator.collection_infos)

        # Calibration phase (optional).
        # Reads the explains stored on the previous step (this run and/or previous runs),
        # aparses the explains, nd calibrates the cost model for the ABT nodes.
        models = await abt_calibrator.calibrate(main_config.abt_calibrator, database)
        for abt, model in models.items():
            print(f'{abt}\t\t{model}')

        parameters = await parameters_extractor.extract_parameters(main_config.abt_calibrator,
                                                                   database, [])
        save_to_csv(parameters, 'parameters.csv')

    print("DONE!")


if __name__ == '__main__':
    loop = asyncio.get_event_loop()
    loop.run_until_complete(main())
