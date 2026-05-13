#pragma once

// Build mode configuration for Dinero
// Controls stub behavior and development features

#if defined(NDEBUG)
  #define DIN_STUB_FATAL 1
  #define DIN_BUILD_MODE "release"
#else
  #define DIN_STUB_FATAL 0
  #define DIN_BUILD_MODE "debug"
#endif

// Development features (only in debug builds)
#ifndef NDEBUG
  #define DIN_ENABLE_DEV_FEATURES 1
  #define DIN_ENABLE_STUB_LOGGING 1
  #define DIN_ENABLE_TRACE_LOGGING 1
#else
  #define DIN_ENABLE_DEV_FEATURES 0
  #define DIN_ENABLE_STUB_LOGGING 0
  #define DIN_ENABLE_TRACE_LOGGING 0
#endif

// Stub behavior policy
// - In Release: stubs are fatal by default
// - In Debug: stubs log but continue unless DIN_FAIL_ON_STUB=1
// - Can be overridden at runtime with DIN_FAIL_ON_STUB environment variable
