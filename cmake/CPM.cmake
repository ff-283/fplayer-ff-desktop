# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2019-2023 Lars Melchior and contributors


set(_CPM_VENDOR_LOCATION "${CMAKE_CURRENT_LIST_DIR}/vendor/CPM.cmake")
if(EXISTS "${_CPM_VENDOR_LOCATION}")
  include("${_CPM_VENDOR_LOCATION}")
  return()
endif()

message(FATAL_ERROR
        "Vendored CPM.cmake not found at: ${_CPM_VENDOR_LOCATION}\n"
        "Please add it to the repo (recommended), or restore the download logic.")

