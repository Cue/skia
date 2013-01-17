# Copyright (c) 2013 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.


"""Top-level presubmit script for Skia.

See http://dev.chromium.org/developers/how-tos/depottools/presubmit-scripts
for more details about the presubmit API built into gcl.
"""

def _CommonChecks(input_api, output_api):
  """Presubmit checks common to upload and commit."""
  results = []
  sources = lambda x: (x.LocalPath().endswith('.h') or
                       x.LocalPath().endswith('.gypi') or
                       x.LocalPath().endswith('.gyp') or
                       x.LocalPath().endswith('.py') or
                       x.LocalPath().endswith('.sh') or
                       x.LocalPath().endswith('.cpp'))
  results.extend(
      input_api.canned_checks.CheckChangeHasOnlyOneEol(
          input_api, output_api, source_file_filter=sources))
  return results


def CheckChangeOnUpload(input_api, output_api):
  """Presubmit checks for the change on upload.

  The following are the presubmit checks:
  * Check change has one and only one EOL.
  """
  results = []
  results.extend(_CommonChecks(input_api, output_api))
  return results


def CheckChangeOnCommit(input_api, output_api):
  """Presubmit checks for the change on commit.

  The following are the presubmit checks:
  * Check change has one and only one EOL.
  * Ensures that the Skia tree is not closed in
    http://skia-tree-status.appspot.com/
  """
  results = []
  results.extend(_CommonChecks(input_api, output_api))
  results.extend(
      input_api.canned_checks.CheckTreeIsOpen(
          input_api, output_api, json_url=(
              'http://skia-tree-status.appspot.com/banner-status?format=json')))
  return results
