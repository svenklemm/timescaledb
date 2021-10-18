# Copyright (c) 2016-2021  Timescale, Inc. All Rights Reserved.
#
# This file is licensed under the Apache License, see LICENSE-APACHE
# at the top level directory of the timescaledb distribution.

# Python script to dynamically generate matrix for github action

# Since we want to run additional test configurations when triggered
# by a push to prerelease_test or by cron but github actions don't
# allow a dynamic matrix via yaml configuration, we generate the matrix
# with this python script. While we could always have the full matrix
# and have if checks on every step that would make the actual checks
# harder to browse because the workflow will have lots of entries and
# only by navigating into the individual jobs would it be visible
# if a job was actually run.

import json
import sys

# github event type which is either push, pull_request or schedule
event_type = sys.argv[1]

PG12_EARLIEST = "12.0"
PG12_LATEST = "12.8"
PG13_EARLIEST = "13.2"
PG13_LATEST = "13.4"
PG14_LATEST = "14.0"

m = {"include": [],}

# helper functions to generate matrix entries
# the release and apache config inherit from the
# debug config to reduce repetition
def build_debug_config(overrides):
  # llvm version and clang versions must match otherwise
  # there will be build errors this is true even when compiling
  # with gcc as clang is used to compile the llvm parts.
  #
  # Strictly speaking, WARNINGS_AS_ERRORS=ON is not needed here, but
  # we add it as a precation. Intention is to have at least one
  # release and one debug build with WARNINGS_AS_ERRORS=ON so that we
  # capture warnings generated due to changes in the code base or the
  # compiler.
  base_config = dict({
    "name": "Debug",
    "build_type": "Debug",
    "pg_build_args": "--enable-debug --enable-cassert",
    "tsdb_build_args": "-DCODECOVERAGE=ON -DWARNINGS_AS_ERRORS=ON",
    "installcheck_args": "IGNORES='bgw_db_scheduler'",
    "coverage": True,
    "extra_packages": "clang-9 llvm-9 llvm-9-dev llvm-9-tools",
    "llvm_config": "llvm-config-9",
    "clang": "clang-9",
    "os": "ubuntu-20.04",
    "cc": "gcc",
    "cxx": "g++",
  })
  base_config.update(overrides)
  return base_config

# We build this release configuration with WARNINGS_AS_ERRORS=ON to
# make sure that we can build with -Werrors even for release
# builds. This will capture some cases where warnings are generated
# for release builds but not for debug builds.
def build_release_config(overrides):
  base_config = build_debug_config({})
  release_config = dict({
    "name": "Release",
    "build_type": "Release",
    "pg_build_args": "",
    "tsdb_build_args": "-DWARNINGS_AS_ERRORS=ON",
    "coverage": False,
  })
  base_config.update(release_config)
  base_config.update(overrides)
  return base_config

def build_apache_config(overrides):
  base_config = build_debug_config({})
  apache_config = dict({
    "name": "ApacheOnly",
    "build_type": "Release",
    "tsdb_build_args": "-DAPACHE_ONLY=1",
    "pg_build_args": "",
    "coverage": False,
  })
  base_config.update(apache_config)
  base_config.update(overrides)
  return base_config

def macos_config(overrides):
  base_config = dict({
    "pg": PG12_LATEST,
    "os": "macos-10.15",
    "cc": "clang",
    "cxx": "clang++",
    "clang": "clang",
    "pg_extra_args": "--with-libraries=/usr/local/opt/openssl/lib --with-includes=/usr/local/opt/openssl/include",
    "tsdb_build_args": "-DASSERTIONS=ON -DOPENSSL_ROOT_DIR=/usr/local/opt/openssl",
    "llvm_config": "/usr/local/opt/llvm/bin/llvm-config",
    "coverage": False,
    "installcheck_args": "IGNORES='bgw_db_scheduler bgw_launcher remote_connection'",
    "extra_packages": "",
  })
  base_config.update(overrides)
  return base_config

# always test debug build on latest of all supported pg versions
m["include"].append(build_debug_config({"pg":PG12_LATEST}))
m["include"].append(build_debug_config({"pg":PG13_LATEST}))
m["include"].append(build_debug_config({"pg":PG14_LATEST}))

m["include"].append(build_release_config(macos_config({})))

# if this is not a pull request e.g. a scheduled run or a push
# to a specific branch like prerelease_test we add additional
# entries to the matrix
if event_type != "pull_request":

  # add debug test for first supported PG12 version
  pg12_debug_earliest = {
    "pg": PG12_EARLIEST,
    "installcheck_args": "IGNORES='cluster-12 compression_ddl continuous_aggs_concurrent_refresh continuous_aggs_insert continuous_aggs_multi deadlock_drop_chunks_compress deadlock_dropchunks_select dist_restore_point dropchunks_race insert_dropchunks_race isolation_nop multi_transaction_indexing read_committed_insert read_uncommitted_insert remote_create_chunk reorder_deadlock reorder_vs_insert reorder_vs_insert_other_chunk reorder_vs_select repeatable_read_insert serializable_insert serializable_insert_rollback'"
  }
  m["include"].append(build_debug_config(pg12_debug_earliest))

  # add debug test for first supported PG13 version
  pg13_debug_earliest = {
    "pg": PG13_EARLIEST,
    "installcheck_args": "IGNORES='compression_ddl continuous_aggs_concurrent_refresh continuous_aggs_insert continuous_aggs_multi deadlock_drop_chunks_compress deadlock_dropchunks_select dist_restore_point dropchunks_race insert_dropchunks_race isolation_nop multi_transaction_indexing read_committed_insert read_uncommitted_insert remote_create_chunk reorder_deadlock reorder_vs_insert reorder_vs_insert_other_chunk reorder_vs_select repeatable_read_insert serializable_insert serializable_insert_rollback'"
  }
  m["include"].append(build_debug_config(pg13_debug_earliest))

  # add debug test for MacOS
  m["include"].append(build_debug_config(macos_config({})))

  # add release test for latest pg 12 and 13
  m["include"].append(build_release_config({"pg":PG12_LATEST}))
  m["include"].append(build_release_config({"pg":PG13_LATEST}))
  m["include"].append(build_release_config({"pg":PG14_LATEST}))

  # add apache only test for latest pg
  m["include"].append(build_apache_config({"pg":PG12_LATEST}))
  m["include"].append(build_apache_config({"pg":PG13_LATEST}))
  m["include"].append(build_apache_config({"pg":PG14_LATEST}))

  # to discover issues with upcoming releases we run CI against
  # the stable branches of supported PG releases
  m["include"].append(build_debug_config({"pg":12,"snapshot":"snapshot"}))
  m["include"].append(build_debug_config({"pg":13,"snapshot":"snapshot"}))
  m["include"].append(build_debug_config({"pg":14,"snapshot":"snapshot"}))

# generate command to set github action variable
print(str.format("::set-output name=matrix::{0}",json.dumps(m)))

